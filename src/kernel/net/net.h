#ifndef BUZZOS_NET_H
#define BUZZOS_NET_H

#include <stddef.h>
#include <stdint.h>
#define NET_IO_BASE 0x300
#define NET_IRQ     10

/* Ethernet frame layout */
struct eth_frame {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype;
    uint8_t  payload[];
} __attribute__((packed));

/* ARP packet */
struct arp_pkt {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} __attribute__((packed));

/* IP header */
struct ip_hdr {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed));

/* ICMP echo */
struct icmp_echo {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
    uint8_t  data[];
} __attribute__((packed));

/* Initialise the NE2000, set up MAC and IP. */
void net_init(void);

/* Send an Ethernet frame. Returns 0 on success. */
int  net_send(const void *data, size_t len);

/* Poll for a received frame. Returns length or 0 if none. */
size_t net_recv(void *buf, size_t max);

/* Send an ICMP echo request and wait for reply. Returns 0 on success. */
int  net_ping(uint32_t ip);

/* Print network status. */
void net_status(void);
int  net_status_text(char *buf, int size);

/* Our MAC and IP */
extern uint8_t  net_mac[6];
extern uint32_t net_ip;

/* UDP header */
struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

/* TCP header */
struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urg_ptr;
} __attribute__((packed));

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

/* Resolve hostname to IP via DNS (QEMU DNS proxy at 10.0.2.3:53).
 * Returns 0 on success, -1 on failure. */
int  net_dns_resolve(const char *hostname, uint32_t *ip_out);

/* Minimal TCP client — single connection at a time. */
#define NET_TCP_RX_CAP 8192

struct net_tcp_pcb {
    uint32_t dst_ip;
    uint16_t dst_port;
    uint16_t src_port;
    uint32_t seq;
    uint32_t ack;
    uint32_t snd_una;
    uint16_t peer_window;
    int      state;
    int      registered;
    int      rx_closed;
    int      rx_reset;
    size_t   rx_len;
    uint8_t  rx_buf[NET_TCP_RX_CAP];
    struct net_tcp_pcb *next;
};

void net_tcp_pcb_init(struct net_tcp_pcb *pcb);
int  net_tcp_connect_pcb(struct net_tcp_pcb *pcb, uint32_t ip, uint16_t port);
int  net_tcp_send_pcb(struct net_tcp_pcb *pcb, const void *data, size_t len);
int  net_tcp_recv_pcb(struct net_tcp_pcb *pcb, void *buf, size_t max);
void net_tcp_close_pcb(struct net_tcp_pcb *pcb);

int  net_tcp_connect(uint32_t ip, uint16_t port);
int  net_tcp_send(const void *data, size_t len);
int  net_tcp_recv(void *buf, size_t max);
void net_tcp_close(void);

int  net_udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                  const void *data, size_t len);
int  net_udp_recv(uint16_t local_port, uint32_t *src_ip, uint16_t *src_port,
                  void *buf, size_t max);
int  net_icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq,
                        const void *data, size_t len);
int  net_icmp_recv_echo(uint32_t src_ip, uint16_t id, uint16_t *seq_out,
                        void *buf, size_t max);

/* HTTP GET — resolves host, connects to port 80, sends GET / HTTP/1.0,
 * prints response via putc. */
int  net_wget(const char *host, void (*putc)(char));

/* DHCP — perform DISCOVER/OFFER/REQUEST/ACK to obtain an IP address.
 * Updates net_ip on success. Returns 0 or -1. */
int  net_dhcp(void);
#endif
