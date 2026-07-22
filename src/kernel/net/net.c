#include <stddef.h>
#include <stdint.h>
#include "net.h"
#include "netdev.h"
#include "serial.h"
#include "task.h"
#include "timer.h"

enum {
    NET_TCP_CONNECT_TIMEOUT_MS = 5000,
    NET_TCP_RECV_TIMEOUT_MS = 15000,
    NET_TCP_RETRY_MS = 350,
    NET_TCP_RETRIES = 4,
    NET_DNS_RETRY_TIMEOUT_MS = 1200,
    NET_DNS_RETRIES = 3,
};

static void *memset(void *d, int c, size_t n) { for (size_t i=0;i<n;i++) ((uint8_t*)d)[i]=(uint8_t)c; return d; }
static void *memcpy(void *d, const void *s, size_t n) { for (size_t i=0;i<n;i++) ((uint8_t*)d)[i]=((const uint8_t*)s)[i]; return d; }

uint8_t  net_mac[6];
uint32_t net_ip   = 0x0202000A; /* 10.0.2.2 host → guest is 10.0.2.15 */

static struct netdev *nd;
static int arp_cache_valid;
static uint32_t arp_cache_ip;
static uint8_t arp_cache_mac[6];
static uint32_t net_gateway_ip = 0x0202000A;
static uint32_t net_dns_ip = 0x0302000A;
static uint32_t net_last_dhcp_offer;
static int net_dhcp_status;
static uint32_t net_tx_frames;
static uint32_t net_rx_frames;
static uint32_t net_arp_tx, net_arp_rx;
static uint32_t net_ip_tx, net_ip_rx;
static uint32_t net_icmp_tx, net_icmp_rx;
static uint32_t net_udp_tx, net_udp_rx;
static uint32_t net_tcp_tx, net_tcp_rx;
static uint32_t net_dhcp_tx, net_dhcp_rx;
static uint32_t net_dns_tx, net_dns_rx;

static void dbg(const char *s) { serial_puts("[net] "); serial_puts(s); }
static int net_tcp_dispatch_frame(const void *frame, size_t len);

static uint32_t net_ms_to_ticks(uint32_t ms) {
    return (ms / 1000u) * TIMER_HZ
         + ((ms % 1000u) * TIMER_HZ + 999u) / 1000u;
}

static int net_timer_ready(void) {
    return current_task && timer_ticks() != 0;
}

static uint32_t net_deadline_after_ms(uint32_t ms) {
    uint32_t ticks = net_ms_to_ticks(ms);
    uint32_t deadline = timer_ticks() + (ticks ? ticks : 1);
    return deadline ? deadline : 1;
}

static int net_deadline_expired(uint32_t deadline) {
    return deadline && (int32_t)(timer_ticks() - deadline) >= 0;
}

static void net_poll_backoff(void) {
    if (current_task) {
        task_yield();
        return;
    }
    for (volatile int j = 0; j < 50000; j++) {}
}

/* --- helpers --- */
static uint16_t bswap16(uint16_t v) { return (v << 8) | (v >> 8); }
static uint32_t bswap32(uint32_t v) {
    return ((uint32_t)bswap16((uint16_t)(v >> 16)))
         | ((uint32_t)bswap16((uint16_t)v) << 16);
}

static void count_pair(uint32_t *tx, uint32_t *rx, int is_tx) {
    if (is_tx)
        (*tx)++;
    else
        (*rx)++;
}

static void count_frame_protocols(const void *data, size_t len, int is_tx) {
    if (!data || len < sizeof(struct eth_frame))
        return;
    const struct eth_frame *eth = (const struct eth_frame *)data;
    uint16_t etype = bswap16(eth->ethertype);
    if (etype == 0x0806) {
        count_pair(&net_arp_tx, &net_arp_rx, is_tx);
        return;
    }
    if (etype != 0x0800)
        return;

    count_pair(&net_ip_tx, &net_ip_rx, is_tx);
    if (len < sizeof(struct eth_frame) + sizeof(struct ip_hdr))
        return;
    const struct ip_hdr *ip = (const struct ip_hdr *)eth->payload;
    uint8_t ip_hlen = (uint8_t)((ip->ver_ihl & 0x0F) * 4);
    if (ip_hlen < sizeof(struct ip_hdr) || len < sizeof(struct eth_frame) + ip_hlen)
        return;

    if (ip->protocol == 1) {
        count_pair(&net_icmp_tx, &net_icmp_rx, is_tx);
        return;
    }
    if (ip->protocol == 6) {
        count_pair(&net_tcp_tx, &net_tcp_rx, is_tx);
        return;
    }
    if (ip->protocol != 17)
        return;

    count_pair(&net_udp_tx, &net_udp_rx, is_tx);
    if (len < sizeof(struct eth_frame) + ip_hlen + sizeof(struct udp_hdr))
        return;
    const struct udp_hdr *udp = (const struct udp_hdr *)((const uint8_t *)ip + ip_hlen);
    uint16_t src = bswap16(udp->src_port);
    uint16_t dst = bswap16(udp->dst_port);
    if ((src == 67 && dst == 68) || (src == 68 && dst == 67))
        count_pair(&net_dhcp_tx, &net_dhcp_rx, is_tx);
    if (src == 53 || dst == 53)
        count_pair(&net_dns_tx, &net_dns_rx, is_tx);
}

static int dev_send(const void *data, size_t len) {
    int ret = nd->send(nd, data, len);
    if (ret == 0) {
        net_tx_frames++;
        count_frame_protocols(data, len, 1);
    }
    return ret;
}

static size_t dev_recv_raw(void *buf, size_t max) {
    size_t n = nd->recv(nd, buf, max);
    if (n > 0) {
        net_rx_frames++;
        count_frame_protocols(buf, n, 0);
    }
    return n;
}

static size_t dev_recv(void *buf, size_t max) {
    if (!buf || max == 0)
        return 0;
    uint8_t raw[1514];
    void *rxbuf = buf;
    size_t rxmax = max;
    if (max < sizeof(raw)) {
        rxbuf = raw;
        rxmax = sizeof(raw);
    }

    for (int drained = 0; drained < 4; drained++) {
        size_t n = dev_recv_raw(rxbuf, rxmax);
        if (n == 0)
            return 0;
        if (net_tcp_dispatch_frame(rxbuf, n))
            continue;
        if (rxbuf != buf) {
            size_t copy = n < max ? n : max;
            memcpy(buf, rxbuf, copy);
            return copy;
        }
        return n;
    }
    return 0;
}

void net_init(void) {
    ne2000_init_device();
    nd = netdev_get();
    if (!nd || (uintptr_t)nd == (uintptr_t)~0u) {
        serial_puts("[net] no network device, using static IP\n");
        net_ip = 0x0F02000A; /* 10.0.2.15 */
        net_dhcp_status = -1;
        return;
    }
    for (int i = 0; i < 6; i++) net_mac[i] = nd->mac[i];

    /* Try DHCP; fall back to static 10.0.2.15 if it fails */
    net_ip = 0;
    net_last_dhcp_offer = 0;
    net_dhcp_status = 0;
    if (net_dhcp() < 0) {
        net_ip = 0x0F02000A; /* 10.0.2.15 */
        net_dhcp_status = -1;
        dbg("dhcp failed, using static IP\n");
    }
    serial_puts("[net] IP=");
    serial_puthex(net_ip);
    serial_puts("\n");
}

static uint16_t ip_checksum(const void *data, size_t len) {
    uint32_t sum = 0;
    const uint8_t *b = (const uint8_t *)data;
    /* sum 16-bit words in network byte order */
    for (size_t i = 0; i + 1 < len; i += 2)
        sum += ((uint16_t)b[i] << 8) | b[i + 1];
    if (len & 1) sum += (uint16_t)b[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    /* Words were summed big-endian, so ~sum is the checksum value in host
     * order. The field is network byte order, so swap before storing (the
     * direct assignment on little-endian x86 would otherwise reverse it). */
    return bswap16((uint16_t)~sum);
}

/* --- ARP --- */
static int arp_resolve(uint32_t ip, uint8_t *mac_out) {
    if (arp_cache_valid && arp_cache_ip == ip) {
        memcpy(mac_out, arp_cache_mac, 6);
        return 0;
    }

    uint8_t pkt[42];
    struct eth_frame *eth = (struct eth_frame *)pkt;
    struct arp_pkt   *arp = (struct arp_pkt *)eth->payload;

    memset(eth->dst, 0xFF, 6);
    memcpy(eth->src, net_mac, 6);
    eth->ethertype = bswap16(0x0806);

    arp->htype = bswap16(1);
    arp->ptype = bswap16(0x0800);
    arp->hlen  = 6;
    arp->plen  = 4;
    arp->oper  = bswap16(1); /* request */
    memcpy(arp->sha, net_mac, 6);
    arp->spa = net_ip;
    memset(arp->tha, 0, 6);
    arp->tpa = ip;

    dev_send(pkt, sizeof(pkt));
    dbg("arp sent, waiting reply\n");
    for (int tries = 0; tries < 50; tries++) {
        uint8_t rbuf[64];
        size_t n = dev_recv(rbuf, sizeof(rbuf));
        if (n == 0) { net_poll_backoff(); continue; }
        if (n >= 42) {
            struct eth_frame *re = (struct eth_frame *)rbuf;
            if (bswap16(re->ethertype) == 0x0806) {
                struct arp_pkt *ra = (struct arp_pkt *)re->payload;
                if (bswap16(ra->oper) == 2 && ra->spa == ip) {
                    memcpy(mac_out, ra->sha, 6);
                    memcpy(arp_cache_mac, ra->sha, 6);
                    arp_cache_ip = ip;
                    arp_cache_valid = 1;
                    return 0;
                }
            }
        }
    }
    dbg("arp resolve timeout\n");
    return -1;
}

/* --- IP send --- */
static int ip_send(uint32_t dst_ip, uint8_t proto, const void *data, size_t len) {
    uint8_t dst_mac[6];
    if (dst_ip == 0xFFFFFFFFu) {
        memset(dst_mac, 0xFF, 6);
    } else {
        /* Route through gateway (10.0.2.2) for non-local destinations */
        uint32_t arp_ip = dst_ip;
        if ((dst_ip & 0x00FFFFFF) != (net_ip & 0x00FFFFFF))
            arp_ip = 0x0202000A; /* QEMU user-mode gateway */
        if (arp_resolve(arp_ip, dst_mac) < 0) return -1;
    }

    uint8_t buf[1514];
    struct eth_frame *eth = (struct eth_frame *)buf;
    struct ip_hdr    *ip  = (struct ip_hdr *)eth->payload;

    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, net_mac, 6);
    eth->ethertype = bswap16(0x0800);

    memset(ip, 0, sizeof(*ip));
    ip->ver_ihl   = 0x45;
    ip->total_len = bswap16((uint16_t)(sizeof(*ip) + len));
    ip->id        = bswap16(1);
    ip->ttl       = 64;
    ip->protocol  = proto;
    ip->src_ip    = net_ip;
    ip->dst_ip    = dst_ip;
    ip->checksum  = ip_checksum(ip, sizeof(*ip));

    memcpy(ip + 1, data, len);
    return dev_send(buf, sizeof(struct eth_frame) + sizeof(*ip) + len);
}
int net_ping(uint32_t ip) {
    struct icmp_echo echo;
    memset(&echo, 0, sizeof(echo));
    echo.type = 8; /* echo request */
    echo.code = 0;
    echo.id   = bswap16(0x42);
    echo.seq  = bswap16(1);
    echo.checksum = 0;
    echo.checksum = ip_checksum(&echo, sizeof(echo));
    int ret = ip_send(ip, 1, &echo, sizeof(echo));
    if (ret < 0) { dbg("ip_send failed\n"); return -1; }
    dbg("ping sent, waiting reply\n");
    for (int tries = 0; tries < 50; tries++) {
        uint8_t rbuf[1514];
        size_t n = dev_recv(rbuf, sizeof(rbuf));
        if (n == 0) { net_poll_backoff(); continue; }
        dbg("ping recv loop got packet\n");
        if (n < sizeof(struct eth_frame) + sizeof(struct ip_hdr)) { dbg("too short\n"); continue; }
        struct eth_frame *re = (struct eth_frame *)rbuf;
        uint16_t etype = bswap16(re->ethertype);
        if (etype != 0x0800) {
            serial_puts("[net] ping loop rx non-IP ethertype=");
            serial_puthex(etype);
            serial_puts("\n");
            continue;
        }
        struct ip_hdr *rip = (struct ip_hdr *)re->payload;
        if (rip->protocol != 1) {
            serial_puts("[net] ping loop rx non-ICMP proto=");
            serial_puthex(rip->protocol);
            serial_puts("\n");
            continue;
        }
        if (rip->src_ip != ip) {
            serial_puts("[net] ping loop src_ip mismatch\n");
            continue;
        }
        struct icmp_echo *recho = (struct icmp_echo *)(rip + 1);
        if (recho->type == 0 && bswap16(recho->id) == 0x42) {
            serial_puts("[net] ping reply from ");
            serial_puthex(ip);
            serial_puts("\n");
            return 0;
        }
        serial_puts("[net] ping loop ICMP type=");
        serial_puthex(recho->type);
        serial_puts("\n");
    }
    return -1;
}

int net_icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq,
                       const void *data, size_t len) {
    if (len > 1200)
        len = 1200;
    uint8_t pkt[sizeof(struct icmp_echo) + 1200];
    struct icmp_echo *echo = (struct icmp_echo *)pkt;
    memset(echo, 0, sizeof(*echo) + len);
    echo->type = 8;
    echo->code = 0;
    echo->id = bswap16(id);
    echo->seq = bswap16(seq);
    memcpy(echo->data, data, len);
    echo->checksum = ip_checksum(echo, sizeof(*echo) + len);
    return ip_send(dst_ip, 1, echo, sizeof(*echo) + len);
}

int net_icmp_recv_echo(uint32_t src_ip, uint16_t id, uint16_t *seq_out,
                       void *buf, size_t max) {
    for (int tries = 0; tries < 1000; tries++) {
        uint8_t rbuf[1514];
        size_t n = dev_recv(rbuf, sizeof(rbuf));
        if (n == 0) { net_poll_backoff(); continue; }
        if (n < sizeof(struct eth_frame) + sizeof(struct ip_hdr) + sizeof(struct icmp_echo))
            continue;
        struct eth_frame *eth = (struct eth_frame *)rbuf;
        if (bswap16(eth->ethertype) != 0x0800)
            continue;
        struct ip_hdr *ip = (struct ip_hdr *)eth->payload;
        if (ip->protocol != 1)
            continue;
        if (src_ip && ip->src_ip != src_ip)
            continue;
        uint16_t ip_total = bswap16(ip->total_len);
        uint8_t ip_hlen = (uint8_t)((ip->ver_ihl & 0x0F) * 4);
        if (ip_hlen < sizeof(struct ip_hdr) || ip_total < ip_hlen + sizeof(struct icmp_echo))
            continue;
        struct icmp_echo *echo = (struct icmp_echo *)((uint8_t *)ip + ip_hlen);
        if (echo->type != 0 || bswap16(echo->id) != id)
            continue;
        size_t plen = ip_total - ip_hlen - sizeof(*echo);
        if (plen > max)
            plen = max;
        memcpy(buf, echo->data, plen);
        if (seq_out)
            *seq_out = bswap16(echo->seq);
        return (int)plen;
    }
    return -1;
}

void net_status(void) {
    serial_puts("[net] MAC=");
    for (int i=0;i<6;i++){serial_puthex(net_mac[i]);serial_putc(':');}
    serial_puts(" IP="); serial_puthex(net_ip); serial_puts("\n");
}

/* ================================================================
 *  Raw send / recv wrappers
 * ================================================================ */
int net_send(const void *data, size_t len) {
    return dev_send(data, len);
}

size_t net_recv(void *buf, size_t max) {
    return dev_recv(buf, max);
}

/* ================================================================
 *  Transport-layer checksum (pseudo-header + segment)
 * ================================================================ */
static uint16_t trans_checksum(uint32_t src_ip, uint32_t dst_ip,
                                uint8_t proto, const void *seg, size_t len) {
    uint32_t sum = 0;
    const uint8_t *b;
    b = (const uint8_t *)&src_ip;
    sum += ((uint16_t)b[0] << 8) | b[1];
    sum += ((uint16_t)b[2] << 8) | b[3];
    b = (const uint8_t *)&dst_ip;
    sum += ((uint16_t)b[0] << 8) | b[1];
    sum += ((uint16_t)b[2] << 8) | b[3];
    sum += proto;
    /* Pseudo-header segment length: added as its numeric value (network-order
     * bytes 00 LL read big-endian == len), NOT byte-swapped. */
    sum += (uint16_t)len;
    b = (const uint8_t *)seg;
    for (size_t i = 0; i + 1 < len; i += 2)
        sum += ((uint16_t)b[i] << 8) | b[i + 1];
    if (len & 1) sum += (uint16_t)b[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return bswap16((uint16_t)~sum);
}

/* ================================================================
 *  UDP
 * ================================================================ */
static int udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                    const void *data, size_t len) {
    uint8_t pkt[1514];
    struct udp_hdr *udp = (struct udp_hdr *)pkt;
    udp->src_port = bswap16(src_port);
    udp->dst_port = bswap16(dst_port);
    udp->length   = bswap16((uint16_t)(sizeof(*udp) + len));
    udp->checksum = 0;
    memcpy(pkt + sizeof(*udp), data, len);
    return ip_send(dst_ip, 17, pkt, sizeof(*udp) + len);
}

int net_udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                 const void *data, size_t len) {
    return udp_send(dst_ip, src_port, dst_port, data, len);
}

int net_udp_recv(uint16_t local_port, uint32_t *src_ip, uint16_t *src_port,
                 void *buf, size_t max) {
    for (int tries = 0; tries < 2000; tries++) {
        uint8_t rbuf[1514];
        size_t n = dev_recv(rbuf, sizeof(rbuf));
        if (n == 0) { net_poll_backoff(); continue; }
        if (n < sizeof(struct eth_frame) + sizeof(struct ip_hdr) + sizeof(struct udp_hdr))
            continue;
        struct eth_frame *eth = (struct eth_frame *)rbuf;
        if (bswap16(eth->ethertype) != 0x0800)
            continue;
        struct ip_hdr *ip = (struct ip_hdr *)eth->payload;
        if (ip->protocol != 17)
            continue;
        uint8_t ip_hlen = (uint8_t)((ip->ver_ihl & 0x0F) * 4);
        if (ip_hlen < sizeof(struct ip_hdr))
            continue;
        if (n < sizeof(struct eth_frame) + ip_hlen + sizeof(struct udp_hdr))
            continue;
        struct udp_hdr *udp = (struct udp_hdr *)((uint8_t *)ip + ip_hlen);
        if (bswap16(udp->dst_port) != local_port)
            continue;
        size_t ulen = bswap16(udp->length);
        if (ulen < sizeof(*udp))
            continue;
        size_t plen = ulen - sizeof(*udp);
        if (plen > max)
            plen = max;
        memcpy(buf, udp + 1, plen);
        if (src_ip)
            *src_ip = ip->src_ip;
        if (src_port)
            *src_port = bswap16(udp->src_port);
        return (int)plen;
    }
    return -1;
}

/* ================================================================
 *  DNS — simple A-record resolver
 * ================================================================ */

static size_t dns_encode_name(uint8_t *out, const char *name) {
    uint8_t *start = out;
    while (*name) {
        const char *dot = name;
        while (*dot && *dot != '.') dot++;
        size_t seg = (size_t)(dot - name);
        if (seg > 63) seg = 63;
        *out++ = (uint8_t)seg;
        for (size_t i = 0; i < seg; i++) *out++ = (uint8_t)name[i];
        name = dot;
        if (*name == '.') name++;
    }
    *out++ = 0;
    return (size_t)(out - start);
}

static const uint8_t *dns_skip_name(const uint8_t *p, const uint8_t *end) {
    while (p < end) {
        uint8_t label = *p++;
        if ((label & 0xC0) == 0xC0)
            return p < end ? p + 1 : 0;
        if (label == 0)
            return p;
        if (label > 63 || (size_t)(end - p) < label)
            return 0;
        p += label;
    }
    return 0;
}

static uint16_t dns_next_id = 0x1234;
static uint16_t dns_next_port = 49152;

int net_dns_resolve(const char *hostname, uint32_t *ip_out) {
    if (!hostname || !hostname[0] || !ip_out)
        return -1;

    uint8_t qbuf[512];
    memset(qbuf, 0, sizeof(qbuf));
    uint16_t txid = ++dns_next_id;
    uint16_t local_port = ++dns_next_port;
    if (dns_next_port < 49152 || dns_next_port == 65535)
        dns_next_port = 49152;
    qbuf[0] = (uint8_t)(txid >> 8);
    qbuf[1] = (uint8_t)txid;
    qbuf[2] = 0x01; qbuf[3] = 0x00;
    qbuf[4] = 0x00; qbuf[5] = 0x01;
    size_t nlen = dns_encode_name(qbuf + 12, hostname);
    uint8_t *q = qbuf + 12 + nlen;
    q[0] = 0x00; q[1] = 0x01;
    q[2] = 0x00; q[3] = 0x01;
    size_t qlen = (size_t)(q + 4 - qbuf);

    for (int attempt = 0; attempt < NET_DNS_RETRIES; attempt++) {
        dbg(attempt ? "dns: retrying query\n" : "dns: sending query\n");
        if (udp_send(net_dns_ip, local_port, 53, qbuf, qlen) < 0) {
            dbg("dns: udp send failed\n");
            continue;
        }

        uint32_t deadline = net_deadline_after_ms(NET_DNS_RETRY_TIMEOUT_MS);
        while (!net_deadline_expired(deadline)) {
            uint8_t rbuf[1514];
            size_t n = dev_recv(rbuf, sizeof(rbuf));
            if (n == 0) {
                if (net_timer_ready())
                    task_sleep_until(timer_ticks() + 1);
                else
                    net_poll_backoff();
                continue;
            }
            if (n < sizeof(struct eth_frame) + sizeof(struct ip_hdr) + sizeof(struct udp_hdr))
                continue;
            struct eth_frame *re = (struct eth_frame *)rbuf;
            if (bswap16(re->ethertype) != 0x0800) continue;
            struct ip_hdr *rip = (struct ip_hdr *)re->payload;
            uint8_t ip_hlen = (uint8_t)((rip->ver_ihl & 0x0F) * 4);
            if (rip->protocol != 17 || ip_hlen < sizeof(*rip)) continue;
            if (rip->src_ip != net_dns_ip) continue;
            if (n < sizeof(struct eth_frame) + ip_hlen + sizeof(struct udp_hdr)) continue;
            struct udp_hdr *rudp = (struct udp_hdr *)((uint8_t *)rip + ip_hlen);
            if (bswap16(rudp->src_port) != 53) continue;
            if (bswap16(rudp->dst_port) != local_port) continue;

            size_t udp_len = bswap16(rudp->length);
            if (udp_len < sizeof(*rudp) + 12 ||
                udp_len > n - sizeof(struct eth_frame) - ip_hlen)
                continue;
            const uint8_t *dns = (const uint8_t *)(rudp + 1);
            size_t dlen = udp_len - sizeof(*rudp);
            const uint8_t *end = dns + dlen;
            if (dns[0] != (uint8_t)(txid >> 8) || dns[1] != (uint8_t)txid)
                continue;
            if (!(dns[2] & 0x80)) continue; /* Not a response. */
            uint8_t rcode = dns[3] & 0x0F;
            if (rcode == 3) { dbg("dns: name does not exist\n"); return -1; }
            if (rcode != 0) break; /* Retry transient server errors. */

            uint16_t qdcount = ((uint16_t)dns[4] << 8) | dns[5];
            uint16_t ancount = ((uint16_t)dns[6] << 8) | dns[7];
            const uint8_t *p = dns + 12;
            for (uint16_t i = 0; i < qdcount; i++) {
                p = dns_skip_name(p, end);
                if (!p || (size_t)(end - p) < 4) { p = 0; break; }
                p += 4;
            }
            if (!p) continue;

            for (uint16_t i = 0; i < ancount; i++) {
                p = dns_skip_name(p, end);
                if (!p || (size_t)(end - p) < 10) break;
                uint16_t rtype = ((uint16_t)p[0] << 8) | p[1];
                uint16_t rclass = ((uint16_t)p[2] << 8) | p[3];
                uint16_t rdlen = ((uint16_t)p[8] << 8) | p[9];
                p += 10;
                if ((size_t)(end - p) < rdlen) break;
                if (rtype == 1 && rclass == 1 && rdlen == 4) {
                    *ip_out = ((uint32_t)p[0]) | ((uint32_t)p[1] << 8)
                            | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                    serial_puts("[dns] resolved ");
                    serial_puts(hostname);
                    serial_puts(" -> ");
                    serial_puthex(*ip_out);
                    serial_puts("\n");
                    return 0;
                }
                p += rdlen;
            }
        }
    }
    dbg("dns: timeout\n");
    return -1;
}

/* ================================================================
 *  Minimal TCP client
 * ================================================================ */

static struct net_tcp_pcb legacy_tcp;
static uint16_t tcp_next_port = 40960;
static uint32_t tcp_next_seq = 0x12345678;
static int net_tcp_open_pcbs;
static uint32_t net_tcp_rx_buffered;
static uint32_t net_tcp_rx_dropped;
static struct net_tcp_pcb *tcp_pcbs;

enum {
    TCP_STATE_CLOSED = 0,
    TCP_STATE_SYN_SENT = 1,
    TCP_STATE_ESTABLISHED = 2,
};

static void net_tcp_unregister_pcb(struct net_tcp_pcb *pcb) {
    if (!pcb || !pcb->registered)
        return;
    struct net_tcp_pcb **link = &tcp_pcbs;
    while (*link) {
        if (*link == pcb) {
            *link = pcb->next;
            break;
        }
        link = &(*link)->next;
    }
    pcb->registered = 0;
    pcb->next = 0;
}

static void net_tcp_register_pcb(struct net_tcp_pcb *pcb) {
    if (!pcb || pcb->registered)
        return;
    pcb->next = tcp_pcbs;
    pcb->registered = 1;
    tcp_pcbs = pcb;
}

void net_tcp_pcb_init(struct net_tcp_pcb *pcb) {
    if (!pcb)
        return;
    if (pcb->state == TCP_STATE_ESTABLISHED && net_tcp_open_pcbs > 0)
        net_tcp_open_pcbs--;
    if (pcb->rx_len > 0) {
        if (net_tcp_rx_buffered >= pcb->rx_len)
            net_tcp_rx_buffered -= (uint32_t)pcb->rx_len;
        else
            net_tcp_rx_buffered = 0;
    }
    net_tcp_unregister_pcb(pcb);
    memset(pcb, 0, sizeof(*pcb));
}

static void net_tcp_mark_closed(struct net_tcp_pcb *pcb) {
    if (!pcb)
        return;
    if (pcb->state == TCP_STATE_ESTABLISHED && net_tcp_open_pcbs > 0)
        net_tcp_open_pcbs--;
    pcb->state = TCP_STATE_CLOSED;
    net_tcp_unregister_pcb(pcb);
}

static uint16_t net_tcp_receive_window(const struct net_tcp_pcb *pcb) {
    if (!pcb || pcb->rx_len >= NET_TCP_RX_CAP)
        return 0;
    size_t space = NET_TCP_RX_CAP - pcb->rx_len;
    /* Keep an incoming burst below the roughly 14.5 KiB NE2000 ring. */
    if (space > 8192u)
        space = 8192u;
    return (uint16_t)(space > 0xFFFFu ? 0xFFFFu : space);
}

static int net_tcp_send_ack(struct net_tcp_pcb *pcb) {
    uint8_t ak[sizeof(struct tcp_hdr)];
    struct tcp_hdr *ah = (struct tcp_hdr *)ak;
    if (!pcb)
        return -1;
    memset(ah, 0, sizeof(*ah));
    ah->src_port = bswap16(pcb->src_port);
    ah->dst_port = bswap16(pcb->dst_port);
    ah->seq      = bswap32(pcb->seq);
    ah->ack      = bswap32(pcb->ack);
    ah->data_off = (uint8_t)(sizeof(*ah) / 4) << 4;
    ah->flags    = TCP_ACK;
    ah->window   = bswap16(net_tcp_receive_window(pcb));
    ah->checksum = trans_checksum(net_ip, pcb->dst_ip, 6, ah, sizeof(*ah));
    return ip_send(pcb->dst_ip, 6, ah, sizeof(*ah));
}

static struct net_tcp_pcb *net_tcp_match_pcb(uint32_t src_ip, uint16_t src_port,
                                             uint16_t dst_port) {
    struct net_tcp_pcb *pcb = tcp_pcbs;
    while (pcb) {
        if (pcb->registered && pcb->src_port == dst_port &&
            pcb->dst_port == src_port && pcb->dst_ip == src_ip)
            return pcb;
        pcb = pcb->next;
    }
    return 0;
}

static size_t net_tcp_queue_rx(struct net_tcp_pcb *pcb, const uint8_t *payload,
                               size_t plen) {
    if (!pcb || !payload || plen == 0)
        return 0;
    size_t space = NET_TCP_RX_CAP - pcb->rx_len;
    size_t copy = plen < space ? plen : space;
    if (copy > 0) {
        memcpy(pcb->rx_buf + pcb->rx_len, payload, copy);
        pcb->rx_len += copy;
        net_tcp_rx_buffered += (uint32_t)copy;
    }
    if (copy < plen)
        net_tcp_rx_dropped += (uint32_t)(plen - copy);
    return copy;
}

static int net_tcp_take_rx(struct net_tcp_pcb *pcb, void *buf, size_t max) {
    if (!pcb || !buf || max == 0 || pcb->rx_len == 0)
        return 0;
    size_t take = pcb->rx_len < max ? pcb->rx_len : max;
    memcpy(buf, pcb->rx_buf, take);
    for (size_t i = take; i < pcb->rx_len; i++)
        pcb->rx_buf[i - take] = pcb->rx_buf[i];
    pcb->rx_len -= take;
    if (net_tcp_rx_buffered >= take)
        net_tcp_rx_buffered -= (uint32_t)take;
    else
        net_tcp_rx_buffered = 0;
    if (pcb->state == TCP_STATE_ESTABLISHED)
        (void)net_tcp_send_ack(pcb);
    return (int)take;
}

static int net_tcp_poll_once(void) {
    uint8_t rbuf[1514];
    size_t n = dev_recv_raw(rbuf, sizeof(rbuf));
    if (n == 0)
        return 0;
    (void)net_tcp_dispatch_frame(rbuf, n);
    return 1;
}

/* Polling is our receive interrupt. Empty the hardware ring in one visit so
 * a fast peer cannot refill it between a series of recv system calls. */
static int net_tcp_poll_available(void) {
    int handled = 0;
    for (int packets = 0; packets < 64; packets++) {
        int result = net_tcp_poll_once();
        if (result == 0)
            break;
        if (result > 0)
            handled++;
    }
    return handled;
}

static int net_tcp_dispatch_frame(const void *frame, size_t len) {
    if (!frame || len < sizeof(struct eth_frame) + sizeof(struct ip_hdr) + sizeof(struct tcp_hdr))
        return 0;
    const struct eth_frame *eth = (const struct eth_frame *)frame;
    if (bswap16(eth->ethertype) != 0x0800)
        return 0;
    const struct ip_hdr *ip = (const struct ip_hdr *)eth->payload;
    if (ip->protocol != 6 || (net_ip && ip->dst_ip != net_ip))
        return 0;

    uint16_t ip_total = bswap16(ip->total_len);
    uint8_t ip_hlen = (uint8_t)((ip->ver_ihl & 0x0F) * 4);
    if (ip_hlen < sizeof(struct ip_hdr))
        return 0;
    if (ip_total < ip_hlen + sizeof(struct tcp_hdr))
        return 0;
    if (len < sizeof(struct eth_frame) + ip_total)
        return 0;

    const struct tcp_hdr *tcp = (const struct tcp_hdr *)((const uint8_t *)ip + ip_hlen);
    uint16_t src_port = bswap16(tcp->src_port);
    uint16_t dst_port = bswap16(tcp->dst_port);
    struct net_tcp_pcb *pcb = net_tcp_match_pcb(ip->src_ip, src_port, dst_port);
    if (!pcb)
        return 0;

    if (tcp->flags & TCP_RST) {
        pcb->rx_reset = 1;
        net_tcp_mark_closed(pcb);
        return 1;
    }

    if (pcb->state == TCP_STATE_SYN_SENT) {
        if ((tcp->flags & (TCP_SYN | TCP_ACK)) != (TCP_SYN | TCP_ACK))
            return 1;
        uint32_t peer_ack = bswap32(tcp->ack);
        if (peer_ack != pcb->seq + 1u)
            return 1;
        pcb->ack = bswap32(tcp->seq) + 1;
        pcb->seq = peer_ack;
        pcb->snd_una = peer_ack;
        pcb->peer_window = bswap16(tcp->window);
        net_tcp_send_ack(pcb);
        pcb->state = TCP_STATE_ESTABLISHED;
        net_tcp_open_pcbs++;
        dbg("tcp: connected\n");
        return 1;
    }

    if (pcb->state != TCP_STATE_ESTABLISHED)
        return 1;

    uint8_t doff = (uint8_t)((tcp->data_off >> 4) * 4);
    if (doff < sizeof(struct tcp_hdr))
        return 1;
    size_t tcp_len = (size_t)ip_total - ip_hlen;
    if (tcp_len < doff)
        return 1;

    const uint8_t *payload = ((const uint8_t *)tcp) + doff;
    size_t plen = tcp_len - doff;
    uint32_t peer_seq = bswap32(tcp->seq);
    uint32_t fin_seq = peer_seq + (uint32_t)plen;
    if (tcp->flags & TCP_ACK) {
        uint32_t peer_ack = bswap32(tcp->ack);
        if ((int32_t)(peer_ack - pcb->snd_una) >= 0 &&
            (int32_t)(pcb->seq - peer_ack) >= 0)
            pcb->snd_una = peer_ack;
        pcb->peer_window = bswap16(tcp->window);
    }

    if ((int32_t)(peer_seq - pcb->ack) < 0) {
        uint32_t skip = pcb->ack - peer_seq;
        if (skip >= plen) {
            payload += plen;
            plen = 0;
        } else {
            payload += skip;
            plen -= skip;
        }
        peer_seq = pcb->ack;
    }

    size_t queued = 0;
    if (peer_seq == pcb->ack && plen > 0) {
        queued = net_tcp_queue_rx(pcb, payload, plen);
        pcb->ack += (uint32_t)queued;
    }

    int accepted_fin = (tcp->flags & TCP_FIN) && pcb->ack == fin_seq;
    if (accepted_fin)
        pcb->ack++;
    if (plen > 0 || (tcp->flags & TCP_FIN))
        (void)net_tcp_send_ack(pcb);

    if (accepted_fin) {
        pcb->rx_closed = 1;
        net_tcp_mark_closed(pcb);
    }
    return 1;
}

static int net_tcp_send_syn(struct net_tcp_pcb *pcb) {
    uint8_t pkt[sizeof(struct tcp_hdr)];
    struct tcp_hdr *th = (struct tcp_hdr *)pkt;
    memset(th, 0, sizeof(*th));
    th->src_port = bswap16(pcb->src_port);
    th->dst_port = bswap16(pcb->dst_port);
    th->seq      = bswap32(pcb->seq);
    th->data_off = (uint8_t)(sizeof(*th) / 4) << 4;
    th->flags    = TCP_SYN;
    th->window   = bswap16(net_tcp_receive_window(pcb));
    th->checksum = trans_checksum(net_ip, pcb->dst_ip, 6, th, sizeof(*th));
    return ip_send(pcb->dst_ip, 6, th, sizeof(*th));
}

int net_tcp_connect_pcb(struct net_tcp_pcb *pcb, uint32_t ip, uint16_t port) {
    if (!pcb)
        return -1;
    if (pcb->state != TCP_STATE_CLOSED || pcb->registered)
        return -1;

    pcb->dst_ip   = ip;
    pcb->dst_port = port;
    pcb->src_port = tcp_next_port++;
    if (tcp_next_port < 40960 || tcp_next_port > 49151)
        tcp_next_port = 40960;
    pcb->seq      = tcp_next_seq;
    tcp_next_seq += 0x01010101;
    pcb->ack      = 0;
    pcb->snd_una  = pcb->seq;
    pcb->peer_window = 0;
    pcb->rx_closed = 0;
    pcb->rx_reset = 0;
    pcb->state    = TCP_STATE_SYN_SENT;
    net_tcp_register_pcb(pcb);

    if (net_tcp_send_syn(pcb) < 0) {
        net_tcp_mark_closed(pcb);
        dbg("tcp: syn send failed\n"); return -1;
    }
    dbg("tcp: syn sent\n");

    /* Wait for SYN-ACK. Once the scheduler timer is running, use real elapsed
     * time; a pure yield loop can burn through retry counts before WAN replies
     * have a chance to arrive. */
    uint32_t deadline = net_timer_ready() ?
        net_deadline_after_ms(NET_TCP_CONNECT_TIMEOUT_MS) : 0;
    uint32_t retry_deadline = net_timer_ready() ?
        net_deadline_after_ms(NET_TCP_RETRY_MS) : 0;
    int retries = 0;
    for (int tries = 0;; tries++) {
        net_tcp_poll_once();
        if (pcb->state == TCP_STATE_ESTABLISHED)
            return 0;
        if (pcb->rx_reset)
            return -1;
        if (deadline) {
            if (net_deadline_expired(deadline))
                break;
            if (net_deadline_expired(retry_deadline) && retries < NET_TCP_RETRIES) {
                (void)net_tcp_send_syn(pcb);
                retries++;
                retry_deadline = net_deadline_after_ms(NET_TCP_RETRY_MS);
            }
        } else if (tries >= 200) {
            break;
        }
        net_poll_backoff();
    }
    net_tcp_mark_closed(pcb);
    dbg("tcp: connect timeout\n");
    return -1;
}

int net_tcp_send_pcb(struct net_tcp_pcb *pcb, const void *data, size_t len) {
    if (!pcb || pcb->state != TCP_STATE_ESTABLISHED) return -1;
    const uint8_t *p = (const uint8_t *)data;
    size_t left = len;
    if (left == 0)
        return 0;
    while (left > 0) {
        size_t chunk = left > 1200 ? 1200 : left;
        uint8_t pkt[sizeof(struct tcp_hdr) + 1200];
        struct tcp_hdr *th = (struct tcp_hdr *)pkt;
        uint32_t send_seq = pcb->seq;
        uint32_t want_ack = send_seq + (uint32_t)chunk;
        memset(th, 0, sizeof(*th));
        th->src_port = bswap16(pcb->src_port);
        th->dst_port = bswap16(pcb->dst_port);
        th->seq      = bswap32(send_seq);
        th->ack      = bswap32(pcb->ack);
        th->data_off = (uint8_t)(sizeof(*th) / 4) << 4;
        th->flags    = TCP_PSH | TCP_ACK;
        th->window   = bswap16(net_tcp_receive_window(pcb));
        memcpy(pkt + sizeof(*th), p, chunk);
        th->checksum = trans_checksum(net_ip, pcb->dst_ip, 6, pkt, sizeof(*th) + chunk);

        pcb->seq = want_ack;
        int acknowledged = 0;
        for (int attempt = 0; attempt <= NET_TCP_RETRIES && !acknowledged; attempt++) {
            if (ip_send(pcb->dst_ip, 6, pkt, sizeof(*th) + chunk) < 0)
                return -1;
            uint32_t deadline = net_timer_ready() ?
                net_deadline_after_ms(NET_TCP_RETRY_MS) : 0;
            for (int polls = 0;; polls++) {
                net_tcp_poll_once();
                if ((int32_t)(pcb->snd_una - want_ack) >= 0) {
                    acknowledged = 1;
                    break;
                }
                if (pcb->rx_reset || pcb->state == TCP_STATE_CLOSED)
                    return -1;
                if ((deadline && net_deadline_expired(deadline)) ||
                    (!deadline && polls >= 200))
                    break;
                net_poll_backoff();
            }
        }
        if (!acknowledged)
            return -1;
        p += chunk;
        left -= chunk;
    }
    return 0;
}

int net_tcp_recv_pcb(struct net_tcp_pcb *pcb, void *buf, size_t max) {
    if (!pcb) return -1;
    net_tcp_poll_available();
    int queued = net_tcp_take_rx(pcb, buf, max);
    if (queued > 0)
        return queued;
    if (pcb->rx_reset) {
        pcb->rx_reset = 0;
        return -1;
    }
    if (pcb->state == TCP_STATE_CLOSED || pcb->rx_closed) return 0;
    if (pcb->state != TCP_STATE_ESTABLISHED) return -1;

    uint32_t deadline = net_timer_ready() ?
        net_deadline_after_ms(NET_TCP_RECV_TIMEOUT_MS) : 0;
    for (int tries = 0;; tries++) {
        net_tcp_poll_available();
        queued = net_tcp_take_rx(pcb, buf, max);
        if (queued > 0)
            return queued;
        if (pcb->rx_reset) {
            pcb->rx_reset = 0;
            return -1;
        }
        if (pcb->state == TCP_STATE_CLOSED || pcb->rx_closed)
            return 0;
        if (deadline) {
            if (net_deadline_expired(deadline))
                break;
        } else if (tries >= 5000) {
            break;
        }
        net_poll_backoff();
    }
    dbg("tcp: recv timeout\n");
    return -1;
}

void net_tcp_close_pcb(struct net_tcp_pcb *pcb) {
    if (!pcb || pcb->state == TCP_STATE_CLOSED) return;
    if (pcb->state == TCP_STATE_ESTABLISHED) {
        uint8_t fn[sizeof(struct tcp_hdr)];
        struct tcp_hdr *fh = (struct tcp_hdr *)fn;
        memset(fh, 0, sizeof(*fh));
        fh->src_port = bswap16(pcb->src_port);
        fh->dst_port = bswap16(pcb->dst_port);
        uint32_t fin_seq = pcb->seq;
        uint32_t fin_ack = fin_seq + 1u;
        fh->seq      = bswap32(fin_seq);
        fh->ack      = bswap32(pcb->ack);
        fh->data_off = (uint8_t)(sizeof(*fh) / 4) << 4;
        fh->flags    = TCP_FIN | TCP_ACK;
        fh->window   = bswap16(net_tcp_receive_window(pcb));
        fh->checksum = trans_checksum(net_ip, pcb->dst_ip, 6, fh, sizeof(*fh));
        pcb->seq = fin_ack;
        uint32_t deadline = net_timer_ready() ? net_deadline_after_ms(1000) : 0;
        uint32_t retry_deadline = net_timer_ready() ? net_deadline_after_ms(250) : 0;
        for (int polls = 0, retries = 0; pcb->state == TCP_STATE_ESTABLISHED;) {
            if (polls == 0 || (retry_deadline && net_deadline_expired(retry_deadline))) {
                (void)ip_send(pcb->dst_ip, 6, fh, sizeof(*fh));
                if (polls != 0) retries++;
                retry_deadline = net_timer_ready() ? net_deadline_after_ms(250) : 0;
                if (retries >= 3 && (int32_t)(pcb->snd_una - fin_ack) >= 0)
                    break;
            }
            net_tcp_poll_available();
            if (pcb->state == TCP_STATE_CLOSED)
                break;
            if ((deadline && net_deadline_expired(deadline)) || (!deadline && polls >= 400))
                break;
            if (net_timer_ready())
                task_sleep_until(timer_ticks() + 1);
            else
                net_poll_backoff();
            polls++;
        }
    }
    if (pcb->state != TCP_STATE_CLOSED)
        net_tcp_mark_closed(pcb);
    dbg("tcp: closed\n");
}

int net_tcp_connect(uint32_t ip, uint16_t port) {
    return net_tcp_connect_pcb(&legacy_tcp, ip, port);
}

int net_tcp_send(const void *data, size_t len) {
    return net_tcp_send_pcb(&legacy_tcp, data, len);
}

int net_tcp_recv(void *buf, size_t max) {
    return net_tcp_recv_pcb(&legacy_tcp, buf, max);
}

void net_tcp_close(void) {
    net_tcp_close_pcb(&legacy_tcp);
}

/* ================================================================
 *  HTTP GET
 * ================================================================ */
int net_wget(const char *host, void (*putc)(char)) {
    uint32_t ip;
    if (net_dns_resolve(host, &ip) < 0) {
        dbg("wget: dns failed\n");
        return -1;
    }
    if (net_tcp_connect(ip, 80) < 0) {
        dbg("wget: tcp connect failed\n");
        return -1;
    }

    char req[256];
    int rlen = 0;
    {
        const char *fmt = "GET / HTTP/1.0\r\nHost: ";
        while (*fmt) req[rlen++] = *fmt++;
        const char *h = host;
        while (*h) req[rlen++] = *h++;
        const char *tail = "\r\nUser-Agent: BuzzOS/1.0\r\nConnection: close\r\n\r\n";
        const char *t = tail;
        while (*t) req[rlen++] = *t++;
    }

    dbg("wget: sending request\n");
    if (net_tcp_send(req, (size_t)rlen) < 0) {
        dbg("wget: send failed\n");
        net_tcp_close();
        return -1;
    }

    dbg("wget: receiving...\n");
    char rbuf[2048];
    int ok = 1;
    for (;;) {
        int n = net_tcp_recv(rbuf, sizeof(rbuf));
        if (n < 0) { dbg("wget: recv error\n"); ok = 0; break; }
        if (n == 0) break;
        for (int i = 0; i < n; i++) putc(rbuf[i]);
    }
    net_tcp_close();
    return ok ? 0 : -1;
}

/* ================================================================
 *  DHCP client (RFC 2131 — minimal 4-step handshake)
 * ================================================================ */

/* DHCP message types */
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

/* BOOTP/DHCP fixed header (simplified — we only use what we need). */
struct dhcp_pkt {
    uint8_t  op;          /* 1=request, 2=reply */
    uint8_t  htype;       /* 1=ethernet */
    uint8_t  hlen;        /* 6 */
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;      /* "your" IP (offered to client) */
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t cookie;      /* magic cookie 0x63825363 */
    uint8_t  options[64]; /* DHCP options */
} __attribute__((packed));

#define DHCP_MAGIC 0x63538263  /* 99.130.83.99 in LE */

/* Build and send a raw DHCP frame (broadcast, no IP stack needed). */
static int dhcp_send(uint8_t msg_type, uint32_t xid, uint32_t req_ip) {
    uint8_t frame[sizeof(struct eth_frame) + sizeof(struct ip_hdr)
                  + sizeof(struct udp_hdr) + sizeof(struct dhcp_pkt)];
    memset(frame, 0, sizeof(frame));

    struct eth_frame *eth = (struct eth_frame *)frame;
    struct ip_hdr    *ip  = (struct ip_hdr *)eth->payload;
    struct udp_hdr   *udp = (struct udp_hdr *)(ip + 1);
    struct dhcp_pkt  *dhcp = (struct dhcp_pkt *)(udp + 1);

    /* Ethernet: broadcast */
    memset(eth->dst, 0xFF, 6);
    memcpy(eth->src, net_mac, 6);
    eth->ethertype = bswap16(0x0800);

    /* IP: 0.0.0.0 → 255.255.255.255 */
    size_t udp_total = sizeof(*udp) + sizeof(*dhcp);
    ip->ver_ihl   = 0x45;
    ip->total_len = bswap16((uint16_t)(sizeof(*ip) + udp_total));
    ip->ttl       = 64;
    ip->protocol  = 17;
    ip->src_ip    = 0;
    ip->dst_ip    = 0xFFFFFFFF;
    ip->checksum  = ip_checksum(ip, sizeof(*ip));

    /* UDP: 68 → 67 */
    udp->src_port = bswap16(68);
    udp->dst_port = bswap16(67);
    udp->length   = bswap16((uint16_t)udp_total);
    udp->checksum = 0;  /* optional for IPv4 UDP */

    /* DHCP */
    dhcp->op    = 1;
    dhcp->htype = 1;
    dhcp->hlen  = 6;
    dhcp->xid   = xid;
    dhcp->flags = bswap16(0x8000); /* broadcast flag */
    memcpy(dhcp->chaddr, net_mac, 6);
    dhcp->cookie = DHCP_MAGIC;

    /* Options */
    uint8_t *opt = dhcp->options;
    *opt++ = 53; *opt++ = 1; *opt++ = msg_type;  /* DHCP message type */
    if (msg_type == DHCP_REQUEST && req_ip) {
        *opt++ = 50; *opt++ = 4;                  /* requested IP */
        memcpy(opt, &req_ip, 4); opt += 4;
    }
    *opt++ = 55; *opt++ = 3; *opt++ = 1; *opt++ = 3; *opt++ = 6; /* param req */
    *opt++ = 255;                                 /* end */

    return dev_send(frame, sizeof(frame));
}

/* Wait for a DHCP reply of the given type. Returns the offered IP. */
static uint32_t dhcp_recv(uint8_t expect_type, uint32_t xid) {
    for (int tries = 0; tries < 300; tries++) {
        uint8_t rbuf[1514];
        size_t n = dev_recv(rbuf, sizeof(rbuf));
        if (n == 0) { net_poll_backoff(); continue; }
        if (n < sizeof(struct eth_frame) + sizeof(struct ip_hdr)
               + sizeof(struct udp_hdr) + 240)
            continue;
        struct eth_frame *re = (struct eth_frame *)rbuf;
        if (bswap16(re->ethertype) != 0x0800) continue;
        struct ip_hdr *rip = (struct ip_hdr *)re->payload;
        if (rip->protocol != 17) continue;
        struct udp_hdr *rudp = (struct udp_hdr *)(rip + 1);
        if (bswap16(rudp->src_port) != 67) continue;
        if (bswap16(rudp->dst_port) != 68) continue;

        struct dhcp_pkt *dhcp = (struct dhcp_pkt *)(rudp + 1);
        if (dhcp->op != 2) continue;
        if (dhcp->xid != xid) continue;
        if (dhcp->cookie != DHCP_MAGIC) continue;

        /* Find message type in options */
        uint8_t *opt = dhcp->options;
        uint8_t *end = opt + sizeof(dhcp->options);
        uint8_t found_type = 0;
        while (opt < end && *opt != 255) {
            uint8_t code = *opt++;
            if (code == 0) continue;  /* padding */
            uint8_t olen = *opt++;
            if (code == 53 && olen >= 1) found_type = *opt;
            opt += olen;
        }
        if (found_type != expect_type) continue;

        return dhcp->yiaddr;
    }
    return 0;
}

int net_dhcp(void) {
    uint32_t xid = 0xBEEF0001;

    net_dhcp_status = 0;
    net_last_dhcp_offer = 0;
    dbg("dhcp: DISCOVER\n");
    if (dhcp_send(DHCP_DISCOVER, xid, 0) < 0) {
        net_dhcp_status = -1;
        dbg("dhcp: send failed\n"); return -1;
    }

    uint32_t offered = dhcp_recv(DHCP_OFFER, xid);
    if (!offered) { net_dhcp_status = -1; dbg("dhcp: no OFFER\n"); return -1; }
    net_last_dhcp_offer = offered;
    serial_puts("[dhcp] offered IP=");
    serial_puthex(offered);
    serial_puts("\n");

    dbg("dhcp: REQUEST\n");
    if (dhcp_send(DHCP_REQUEST, xid, offered) < 0) {
        net_dhcp_status = -1;
        dbg("dhcp: send failed\n"); return -1;
    }

    uint32_t acked = dhcp_recv(DHCP_ACK, xid);
    if (!acked) { net_dhcp_status = -1; dbg("dhcp: no ACK\n"); return -1; }

    net_ip = acked;
    net_dhcp_status = 1;
    serial_puts("[dhcp] IP assigned: ");
    serial_puthex(net_ip);
    serial_puts("\n");
    return 0;
}

static void status_append_char(char *buf, int *pos, int cap, char ch) {
    if (*pos < cap - 1)
        buf[*pos] = ch;
    (*pos)++;
}

static void status_append_text(char *buf, int *pos, int cap, const char *s) {
    while (s && *s)
        status_append_char(buf, pos, cap, *s++);
}

static void status_append_u32(char *buf, int *pos, int cap, uint32_t value) {
    char tmp[10];
    int n = 0;
    if (value == 0) {
        status_append_char(buf, pos, cap, '0');
        return;
    }
    while (value && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (n > 0)
        status_append_char(buf, pos, cap, tmp[--n]);
}

static void status_append_hex8(char *buf, int *pos, int cap, uint8_t value) {
    static const char hex[] = "0123456789ABCDEF";
    status_append_char(buf, pos, cap, hex[(value >> 4) & 0x0F]);
    status_append_char(buf, pos, cap, hex[value & 0x0F]);
}

static void status_append_ipv4(char *buf, int *pos, int cap, uint32_t ip) {
    status_append_u32(buf, pos, cap, ip & 0xFFu);
    status_append_char(buf, pos, cap, '.');
    status_append_u32(buf, pos, cap, (ip >> 8) & 0xFFu);
    status_append_char(buf, pos, cap, '.');
    status_append_u32(buf, pos, cap, (ip >> 16) & 0xFFu);
    status_append_char(buf, pos, cap, '.');
    status_append_u32(buf, pos, cap, (ip >> 24) & 0xFFu);
}

static void status_append_mac(char *buf, int *pos, int cap, const uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        if (i)
            status_append_char(buf, pos, cap, ':');
        status_append_hex8(buf, pos, cap, mac[i]);
    }
}

static void status_append_counter_pair(char *buf, int *pos, int cap,
                                       const char *name, uint32_t tx, uint32_t rx) {
    status_append_text(buf, pos, cap, name);
    status_append_text(buf, pos, cap, " tx ");
    status_append_u32(buf, pos, cap, tx);
    status_append_text(buf, pos, cap, " rx ");
    status_append_u32(buf, pos, cap, rx);
    status_append_char(buf, pos, cap, '\n');
}

int net_status_text(char *buf, int size) {
    int pos = 0;
    if (!buf || size <= 0)
        return -1;

    status_append_text(buf, &pos, size, "driver ne2000\n");

    status_append_text(buf, &pos, size, "mac ");
    status_append_mac(buf, &pos, size, net_mac);
    status_append_char(buf, &pos, size, '\n');

    status_append_text(buf, &pos, size, "ip ");
    status_append_ipv4(buf, &pos, size, net_ip);
    status_append_char(buf, &pos, size, '\n');

    status_append_text(buf, &pos, size, "gateway ");
    status_append_ipv4(buf, &pos, size, net_gateway_ip);
    status_append_char(buf, &pos, size, '\n');

    status_append_text(buf, &pos, size, "dns ");
    status_append_ipv4(buf, &pos, size, net_dns_ip);
    status_append_char(buf, &pos, size, '\n');

    status_append_text(buf, &pos, size, "dhcp ");
    status_append_text(buf, &pos, size,
                       net_dhcp_status > 0 ? "assigned" :
                       (net_dhcp_status < 0 ? "fallback" : "pending"));
    status_append_char(buf, &pos, size, '\n');

    if (net_last_dhcp_offer) {
        status_append_text(buf, &pos, size, "dhcp_offer ");
        status_append_ipv4(buf, &pos, size, net_last_dhcp_offer);
        status_append_char(buf, &pos, size, '\n');
    }

    status_append_text(buf, &pos, size, "arp ");
    if (arp_cache_valid) {
        status_append_ipv4(buf, &pos, size, arp_cache_ip);
        status_append_char(buf, &pos, size, ' ');
        status_append_mac(buf, &pos, size, arp_cache_mac);
    } else {
        status_append_text(buf, &pos, size, "empty");
    }
    status_append_char(buf, &pos, size, '\n');

    status_append_text(buf, &pos, size, "tcp ");
    status_append_text(buf, &pos, size, net_tcp_open_pcbs > 0 ? "connected" : "closed");
    status_append_text(buf, &pos, size, " open ");
    status_append_u32(buf, &pos, size, (uint32_t)net_tcp_open_pcbs);
    if (legacy_tcp.state == TCP_STATE_ESTABLISHED) {
        status_append_text(buf, &pos, size, " dst ");
        status_append_ipv4(buf, &pos, size, legacy_tcp.dst_ip);
        status_append_char(buf, &pos, size, ':');
        status_append_u32(buf, &pos, size, legacy_tcp.dst_port);
    }
    status_append_char(buf, &pos, size, '\n');

    status_append_text(buf, &pos, size, "tcp_rx_buffered ");
    status_append_u32(buf, &pos, size, net_tcp_rx_buffered);
    status_append_char(buf, &pos, size, '\n');

    status_append_text(buf, &pos, size, "tcp_rx_dropped ");
    status_append_u32(buf, &pos, size, net_tcp_rx_dropped);
    status_append_char(buf, &pos, size, '\n');

    status_append_text(buf, &pos, size, "tx_frames ");
    status_append_u32(buf, &pos, size, net_tx_frames);
    status_append_char(buf, &pos, size, '\n');

    status_append_text(buf, &pos, size, "rx_frames ");
    status_append_u32(buf, &pos, size, net_rx_frames);
    status_append_char(buf, &pos, size, '\n');

    status_append_counter_pair(buf, &pos, size, "arp_frames", net_arp_tx, net_arp_rx);
    status_append_counter_pair(buf, &pos, size, "ip_frames", net_ip_tx, net_ip_rx);
    status_append_counter_pair(buf, &pos, size, "icmp_packets", net_icmp_tx, net_icmp_rx);
    status_append_counter_pair(buf, &pos, size, "udp_packets", net_udp_tx, net_udp_rx);
    status_append_counter_pair(buf, &pos, size, "tcp_packets", net_tcp_tx, net_tcp_rx);
    status_append_counter_pair(buf, &pos, size, "dhcp_packets", net_dhcp_tx, net_dhcp_rx);
    status_append_counter_pair(buf, &pos, size, "dns_packets", net_dns_tx, net_dns_rx);

    if (pos > size - 1)
        pos = size - 1;
    buf[pos] = 0;
    return pos;
}
