#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "packet.h"

static uint16_t be16(uint16_t value) {
    return (uint16_t)((value << 8) | (value >> 8));
}

static uint32_t sum_words(uint32_t sum, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    while (len >= 2) {
        sum += ((uint16_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint16_t)p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return sum;
}

static uint16_t checksum(const void *data, size_t len) {
    return be16((uint16_t)~sum_words(0, data, len));
}

static uint16_t transport_checksum(const struct ip_hdr *ip,
                                   const void *segment, size_t len) {
    uint32_t sum = 0;
    sum = sum_words(sum, &ip->src_ip, 4);
    sum = sum_words(sum, &ip->dst_ip, 4);
    sum += ip->protocol;
    sum += (uint16_t)len;
    sum = sum_words(sum, segment, len);
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return be16((uint16_t)~sum);
}

static size_t ipv4_frame(uint8_t *frame, uint8_t protocol,
                         const void *payload, size_t payload_len) {
    memset(frame, 0, 128);
    struct eth_frame *eth = (struct eth_frame *)frame;
    eth->ethertype = be16(0x0800);
    struct ip_hdr *ip = (struct ip_hdr *)eth->payload;
    ip->ver_ihl = 0x45;
    ip->total_len = be16((uint16_t)(sizeof(*ip) + payload_len));
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->src_ip = 0x0100000Au;
    ip->dst_ip = 0x0200000Au;
    memcpy(ip + 1, payload, payload_len);
    ip->checksum = checksum(ip, sizeof(*ip));
    return sizeof(*eth) + sizeof(*ip) + payload_len;
}

static int expect_rejected(const char *name, int result,
                           enum net_packet_error actual,
                           enum net_packet_error expected) {
    if (result >= 0 || actual != expected) {
        fprintf(stderr, "%s: result=%d error=%d expected=%d\n",
                name, result, actual, expected);
        return 1;
    }
    return 0;
}

int main(void) {
    uint8_t frame[128];
    enum net_packet_error error;
    int failed = 0;

    struct {
        struct udp_hdr udp;
        uint8_t data[4];
    } udp_payload = {{be16(9000), be16(7000), be16(12), 0}, {1, 2, 3, 4}};
    size_t len = ipv4_frame(frame, 17, &udp_payload, sizeof(udp_payload));
    struct net_udp_view udp;
    int result = net_parse_udp(frame, len, &udp, &error);
    if (result < 0 || udp.payload_len != 4) {
        fprintf(stderr, "valid udp: result=%d error=%d payload_len=%zu\n",
                result, error, result < 0 ? 0u : udp.payload_len);
        failed = 1;
    }

    struct ip_hdr *ip = (struct ip_hdr *)((struct eth_frame *)frame)->payload;
    ip->total_len = be16((uint16_t)(sizeof(*ip) + sizeof(udp_payload) + 8));
    ip->checksum = 0;
    ip->checksum = checksum(ip, sizeof(*ip));
    result = net_parse_udp(frame, len, &udp, &error);
    failed |= expect_rejected("truncated ipv4", result, error,
                              NET_PACKET_TRUNCATED);

    len = ipv4_frame(frame, 17, &udp_payload, sizeof(udp_payload));
    ((struct udp_hdr *)(ip + 1))->length = be16(40);
    result = net_parse_udp(frame, len, &udp, &error);
    failed |= expect_rejected("oversized udp length", result, error,
                              NET_PACKET_BAD_TRANSPORT_LENGTH);

    uint8_t short_icmp[4] = {0};
    len = ipv4_frame(frame, 1, short_icmp, sizeof(short_icmp));
    struct net_icmp_view icmp;
    result = net_parse_icmp(frame, len, &icmp, &error);
    failed |= expect_rejected("short icmp", result, error,
                              NET_PACKET_BAD_TRANSPORT_LENGTH);

    struct tcp_hdr tcp;
    memset(&tcp, 0, sizeof(tcp));
    tcp.src_port = be16(80);
    tcp.dst_port = be16(49152);
    tcp.data_off = 4u << 4;
    len = ipv4_frame(frame, 6, &tcp, sizeof(tcp));
    struct net_tcp_view tcp_view;
    result = net_parse_tcp(frame, len, &tcp_view, &error);
    failed |= expect_rejected("invalid tcp data offset", result, error,
                              NET_PACKET_BAD_TRANSPORT_LENGTH);

    tcp.data_off = 5u << 4;
    len = ipv4_frame(frame, 6, &tcp, sizeof(tcp));
    ip = (struct ip_hdr *)((struct eth_frame *)frame)->payload;
    struct tcp_hdr *wire_tcp = (struct tcp_hdr *)(ip + 1);
    wire_tcp->checksum = transport_checksum(ip, wire_tcp, sizeof(*wire_tcp));
    if (net_parse_tcp(frame, len, &tcp_view, &error) < 0) {
        fprintf(stderr, "valid tcp: error=%d\n", error);
        failed = 1;
    }

    ip->frag_off = be16(0x2000);
    ip->checksum = 0;
    ip->checksum = checksum(ip, sizeof(*ip));
    result = net_parse_tcp(frame, len, &tcp_view, &error);
    failed |= expect_rejected("fragmented ipv4", result, error,
                              NET_PACKET_FRAGMENTED);

    ip->frag_off = 0;
    ip->checksum ^= 1;
    result = net_parse_tcp(frame, len, &tcp_view, &error);
    failed |= expect_rejected("bad ipv4 checksum", result, error,
                              NET_PACKET_BAD_CHECKSUM);

    if (failed)
        return 1;
    puts("net packet parser: all boundary cases passed");
    return 0;
}
