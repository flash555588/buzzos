#include "packet.h"

static uint16_t read_be16(const void *ptr) {
    const uint8_t *p = (const uint8_t *)ptr;
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t checksum_add(uint32_t sum, const void *ptr, size_t len) {
    const uint8_t *p = (const uint8_t *)ptr;
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

static int checksum_valid(const void *ptr, size_t len) {
    return checksum_add(0, ptr, len) == 0xFFFFu;
}

static int transport_checksum_valid(const struct net_ipv4_view *ip,
                                    const void *segment, size_t len) {
    uint32_t sum = 0;
    sum = checksum_add(sum, &ip->ip->src_ip, 4);
    sum = checksum_add(sum, &ip->ip->dst_ip, 4);
    sum += ip->ip->protocol;
    sum += (uint16_t)len;
    sum = checksum_add(sum, segment, len);
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return sum == 0xFFFFu;
}

static int reject(enum net_packet_error value, enum net_packet_error *error) {
    if (error)
        *error = value;
    return -1;
}

int net_parse_ipv4(const void *frame, size_t received_len,
                   struct net_ipv4_view *view, enum net_packet_error *error) {
    if (error)
        *error = NET_PACKET_OK;
    if (!frame || !view || received_len < sizeof(struct eth_frame))
        return reject(NET_PACKET_TRUNCATED, error);

    const struct eth_frame *eth = (const struct eth_frame *)frame;
    if (read_be16(&eth->ethertype) != 0x0800)
        return reject(NET_PACKET_NOT_IPV4, error);
    if (received_len < sizeof(*eth) + sizeof(struct ip_hdr))
        return reject(NET_PACKET_TRUNCATED, error);

    const struct ip_hdr *ip = (const struct ip_hdr *)eth->payload;
    if ((ip->ver_ihl >> 4) != 4)
        return reject(NET_PACKET_BAD_VERSION, error);
    size_t header_len = (size_t)(ip->ver_ihl & 0x0Fu) * 4u;
    if (header_len < sizeof(*ip))
        return reject(NET_PACKET_BAD_HEADER_LENGTH, error);
    if (received_len < sizeof(*eth) + header_len)
        return reject(NET_PACKET_TRUNCATED, error);

    size_t declared_len = read_be16(&ip->total_len);
    if (declared_len < header_len)
        return reject(NET_PACKET_BAD_DECLARED_LENGTH, error);
    if (declared_len > received_len - sizeof(*eth))
        return reject(NET_PACKET_TRUNCATED, error);
    if ((read_be16(&ip->frag_off) & 0x3FFFu) != 0)
        return reject(NET_PACKET_FRAGMENTED, error);
    if (!checksum_valid(ip, header_len))
        return reject(NET_PACKET_BAD_CHECKSUM, error);

    view->eth = eth;
    view->ip = ip;
    view->header_len = header_len;
    view->payload = (const uint8_t *)ip + header_len;
    view->payload_len = declared_len - header_len;
    return 0;
}

int net_parse_icmp(const void *frame, size_t received_len,
                   struct net_icmp_view *view, enum net_packet_error *error) {
    if (!view)
        return reject(NET_PACKET_TRUNCATED, error);
    if (net_parse_ipv4(frame, received_len, &view->ipv4, error) < 0)
        return -1;
    if (view->ipv4.ip->protocol != 1)
        return reject(NET_PACKET_WRONG_PROTOCOL, error);
    if (view->ipv4.payload_len < sizeof(struct icmp_echo))
        return reject(NET_PACKET_BAD_TRANSPORT_LENGTH, error);
    if (!checksum_valid(view->ipv4.payload, view->ipv4.payload_len))
        return reject(NET_PACKET_BAD_CHECKSUM, error);
    view->icmp = (const struct icmp_echo *)view->ipv4.payload;
    view->payload = view->ipv4.payload + sizeof(*view->icmp);
    view->payload_len = view->ipv4.payload_len - sizeof(*view->icmp);
    return 0;
}

int net_parse_udp(const void *frame, size_t received_len,
                  struct net_udp_view *view, enum net_packet_error *error) {
    if (!view)
        return reject(NET_PACKET_TRUNCATED, error);
    if (net_parse_ipv4(frame, received_len, &view->ipv4, error) < 0)
        return -1;
    if (view->ipv4.ip->protocol != 17)
        return reject(NET_PACKET_WRONG_PROTOCOL, error);
    if (view->ipv4.payload_len < sizeof(struct udp_hdr))
        return reject(NET_PACKET_BAD_TRANSPORT_LENGTH, error);

    view->udp = (const struct udp_hdr *)view->ipv4.payload;
    size_t udp_len = read_be16(&view->udp->length);
    if (udp_len < sizeof(*view->udp) || udp_len > view->ipv4.payload_len)
        return reject(NET_PACKET_BAD_TRANSPORT_LENGTH, error);
    if (view->udp->checksum &&
        !transport_checksum_valid(&view->ipv4, view->udp, udp_len))
        return reject(NET_PACKET_BAD_CHECKSUM, error);
    view->payload = view->ipv4.payload + sizeof(*view->udp);
    view->payload_len = udp_len - sizeof(*view->udp);
    return 0;
}

int net_parse_tcp(const void *frame, size_t received_len,
                  struct net_tcp_view *view, enum net_packet_error *error) {
    if (!view)
        return reject(NET_PACKET_TRUNCATED, error);
    if (net_parse_ipv4(frame, received_len, &view->ipv4, error) < 0)
        return -1;
    if (view->ipv4.ip->protocol != 6)
        return reject(NET_PACKET_WRONG_PROTOCOL, error);
    if (view->ipv4.payload_len < sizeof(struct tcp_hdr))
        return reject(NET_PACKET_BAD_TRANSPORT_LENGTH, error);

    view->tcp = (const struct tcp_hdr *)view->ipv4.payload;
    view->header_len = (size_t)(view->tcp->data_off >> 4) * 4u;
    if (view->header_len < sizeof(*view->tcp) ||
        view->header_len > view->ipv4.payload_len)
        return reject(NET_PACKET_BAD_TRANSPORT_LENGTH, error);
    if (!transport_checksum_valid(&view->ipv4, view->tcp,
                                  view->ipv4.payload_len))
        return reject(NET_PACKET_BAD_CHECKSUM, error);
    view->payload = view->ipv4.payload + view->header_len;
    view->payload_len = view->ipv4.payload_len - view->header_len;
    return 0;
}
