#ifndef BUZZOS_NET_PACKET_H
#define BUZZOS_NET_PACKET_H

#include <stddef.h>
#include <stdint.h>
#include "net.h"

enum net_packet_error {
    NET_PACKET_OK = 0,
    NET_PACKET_NOT_IPV4,
    NET_PACKET_TRUNCATED,
    NET_PACKET_BAD_VERSION,
    NET_PACKET_BAD_HEADER_LENGTH,
    NET_PACKET_BAD_DECLARED_LENGTH,
    NET_PACKET_FRAGMENTED,
    NET_PACKET_BAD_CHECKSUM,
    NET_PACKET_WRONG_PROTOCOL,
    NET_PACKET_BAD_TRANSPORT_LENGTH,
};

struct net_ipv4_view {
    const struct eth_frame *eth;
    const struct ip_hdr *ip;
    const uint8_t *payload;
    size_t header_len;
    size_t payload_len;
};

struct net_icmp_view {
    struct net_ipv4_view ipv4;
    const struct icmp_echo *icmp;
    const uint8_t *payload;
    size_t payload_len;
};

struct net_udp_view {
    struct net_ipv4_view ipv4;
    const struct udp_hdr *udp;
    const uint8_t *payload;
    size_t payload_len;
};

struct net_tcp_view {
    struct net_ipv4_view ipv4;
    const struct tcp_hdr *tcp;
    const uint8_t *payload;
    size_t header_len;
    size_t payload_len;
};

int net_parse_ipv4(const void *frame, size_t received_len,
                   struct net_ipv4_view *view, enum net_packet_error *error);
int net_parse_icmp(const void *frame, size_t received_len,
                   struct net_icmp_view *view, enum net_packet_error *error);
int net_parse_udp(const void *frame, size_t received_len,
                  struct net_udp_view *view, enum net_packet_error *error);
int net_parse_tcp(const void *frame, size_t received_len,
                  struct net_tcp_view *view, enum net_packet_error *error);

#endif
