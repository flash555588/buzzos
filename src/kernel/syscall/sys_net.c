#include <stddef.h>
#include <stdint.h>
#include "net.h"
#include "syscall_internal.h"
#include "task.h"

enum {
    AF_INET_K      = 2,
    SOCK_STREAM_K  = 1,
    SOCK_DGRAM_K   = 2,
    SOCK_RAW_K     = 3,
    IPPROTO_ICMP_K = 1,
    IPPROTO_UDP_K  = 17,
    MAX_SOCKETS    = 8,
};

struct k_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
};

struct socket_entry {
    int used;
    int owner;
    int domain;
    int type;
    int protocol;
    uint16_t local_port;
    uint32_t peer_ip;
    uint16_t peer_port;
    int connected;
    struct net_tcp_pcb tcp;
};

static struct socket_entry sockets[MAX_SOCKETS];
static volatile int socket_locked;

static uint16_t ntoh16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static void socket_lock(void) {
    while (__sync_lock_test_and_set(&socket_locked, 1))
        task_yield();
}

static void socket_unlock(void) {
    __sync_lock_release(&socket_locked);
}

static int socket_owner(void) {
    if (!current_task)
        return 0;
    if (current_task->fd_owner >= 0 && current_task->fd_owner < MAX_TASKS)
        return current_task->fd_owner;
    return current_task->id;
}

static struct socket_entry *socket_get(int sd) {
    if (sd < 0 || sd >= MAX_SOCKETS)
        return 0;
    if (!sockets[sd].used || sockets[sd].owner != socket_owner())
        return 0;
    return &sockets[sd];
}

static void socket_clear(struct socket_entry *s) {
    s->used = 0;
    s->owner = -1;
    s->domain = 0;
    s->type = 0;
    s->protocol = 0;
    s->local_port = 0;
    s->peer_ip = 0;
    s->peer_port = 0;
    s->connected = 0;
}

int sys_socket(uint32_t domain, uint32_t type, uint32_t protocol, uint32_t d, uint32_t e) {
    (void)d; (void)e;
    if ((int)domain != AF_INET_K)
        return -1;
    if (!((int)type == SOCK_STREAM_K && protocol == 0) &&
        !((int)type == SOCK_DGRAM_K && (protocol == 0 || protocol == IPPROTO_UDP_K)) &&
        !((int)type == SOCK_RAW_K && protocol == IPPROTO_ICMP_K))
        return -1;
    socket_lock();
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].used) {
            sockets[i].used = 1;
            sockets[i].owner = socket_owner();
            sockets[i].domain = (int)domain;
            sockets[i].type = (int)type;
            sockets[i].protocol = (int)protocol;
            sockets[i].local_port = (uint16_t)(49152 + i);
            sockets[i].peer_ip = 0;
            sockets[i].peer_port = 0;
            sockets[i].connected = 0;
            net_tcp_pcb_init(&sockets[i].tcp);
            socket_unlock();
            return i;
        }
    }
    socket_unlock();
    return -1;
}

int sys_connect(uint32_t sd_arg, uint32_t addr_arg, uint32_t addrlen,
                uint32_t d, uint32_t e) {
    (void)d; (void)e;
    if (addrlen < sizeof(struct k_sockaddr_in) ||
        !user_range_ok(addr_arg, sizeof(struct k_sockaddr_in)))
        return -1;
    struct k_sockaddr_in *addr = (struct k_sockaddr_in *)(uintptr_t)addr_arg;
    if (addr->sin_family != AF_INET_K)
        return -1;
    uint32_t peer_ip = addr->sin_addr;
    uint16_t peer_port = ntoh16(addr->sin_port);

    socket_lock();
    struct socket_entry *s = socket_get((int)sd_arg);
    if (!s) {
        socket_unlock();
        return -1;
    }
    if (s->type == SOCK_DGRAM_K || s->type == SOCK_RAW_K) {
        s->peer_ip = peer_ip;
        s->peer_port = peer_port;
        s->connected = 1;
        socket_unlock();
        return 0;
    }
    if (s->type != SOCK_STREAM_K || s->connected) {
        socket_unlock();
        return -1;
    }
    struct net_tcp_pcb *tcp = &s->tcp;
    net_tcp_pcb_init(tcp);
    socket_unlock();

    int ret = net_tcp_connect_pcb(tcp, peer_ip, peer_port);
    socket_lock();
    s = socket_get((int)sd_arg);
    if (ret < 0) {
        if (s && s->type == SOCK_STREAM_K) {
            s->connected = 0;
            net_tcp_pcb_init(&s->tcp);
        }
        socket_unlock();
        return -1;
    }
    if (s && s->type == SOCK_STREAM_K) {
        s->peer_ip = peer_ip;
        s->peer_port = peer_port;
        s->connected = 1;
        socket_unlock();
        return 0;
    }
    socket_unlock();
    net_tcp_close_pcb(tcp);
    return -1;
}

int sys_send(uint32_t sd_arg, uint32_t buf, uint32_t len, uint32_t flags, uint32_t e) {
    (void)flags; (void)e;
    if (!user_range_ok(buf, len))
        return -1;
    socket_lock();
    struct socket_entry *s = socket_get((int)sd_arg);
    if (!s || !s->connected) {
        socket_unlock();
        return -1;
    }
    int type = s->type;
    uint16_t local_port = s->local_port;
    uint32_t peer_ip = s->peer_ip;
    uint16_t peer_port = s->peer_port;
    struct net_tcp_pcb *tcp = &s->tcp;
    socket_unlock();
    int ret = -1;
    if (type == SOCK_STREAM_K)
        ret = net_tcp_send_pcb(tcp, (const void *)(uintptr_t)buf, (size_t)len);
    else if (type == SOCK_DGRAM_K)
        ret = net_udp_send(peer_ip, local_port, peer_port,
                           (const void *)(uintptr_t)buf, (size_t)len);
    else if (type == SOCK_RAW_K)
        ret = net_icmp_send_echo(peer_ip, local_port, 1,
                                 (const void *)(uintptr_t)buf, (size_t)len);
    return ret < 0 ? ret : (int)len;
}

int sys_recv(uint32_t sd_arg, uint32_t buf, uint32_t len, uint32_t flags, uint32_t e) {
    (void)flags; (void)e;
    if (!user_range_writable(buf, len))
        return -1;
    socket_lock();
    struct socket_entry *s = socket_get((int)sd_arg);
    if (!s || !s->connected) {
        socket_unlock();
        return -1;
    }
    int type = s->type;
    uint16_t local_port = s->local_port;
    uint32_t peer_ip = s->peer_ip;
    struct net_tcp_pcb *tcp = &s->tcp;
    socket_unlock();
    if (type == SOCK_STREAM_K)
        return net_tcp_recv_pcb(tcp, (void *)(uintptr_t)buf, (size_t)len);
    if (type == SOCK_DGRAM_K)
        return net_udp_recv(local_port, 0, 0, (void *)(uintptr_t)buf, (size_t)len);
    if (type == SOCK_RAW_K)
        return net_icmp_recv_echo(peer_ip, local_port, 0, (void *)(uintptr_t)buf, (size_t)len);
    return -1;
}

int sys_bind(uint32_t sd_arg, uint32_t addr_arg, uint32_t addrlen,
             uint32_t d, uint32_t e) {
    (void)d; (void)e;
    if (addrlen < sizeof(struct k_sockaddr_in) ||
        !user_range_ok(addr_arg, sizeof(struct k_sockaddr_in)))
        return -1;
    struct k_sockaddr_in *addr = (struct k_sockaddr_in *)(uintptr_t)addr_arg;
    if (addr->sin_family != AF_INET_K)
        return -1;
    socket_lock();
    struct socket_entry *s = socket_get((int)sd_arg);
    if (!s) {
        socket_unlock();
        return -1;
    }
    s->local_port = ntoh16(addr->sin_port);
    socket_unlock();
    return 0;
}

int sys_sendto(uint32_t sd_arg, uint32_t buf, uint32_t len,
               uint32_t addr_arg, uint32_t addrlen) {
    if (!user_range_ok(buf, len) || addrlen < sizeof(struct k_sockaddr_in) ||
        !user_range_ok(addr_arg, sizeof(struct k_sockaddr_in)))
        return -1;
    struct k_sockaddr_in *addr = (struct k_sockaddr_in *)(uintptr_t)addr_arg;
    if (addr->sin_family != AF_INET_K)
        return -1;
    socket_lock();
    struct socket_entry *s = socket_get((int)sd_arg);
    if (!s) {
        socket_unlock();
        return -1;
    }
    int type = s->type;
    uint16_t local_port = s->local_port;
    socket_unlock();
    int ret = -1;
    if (type == SOCK_DGRAM_K)
        ret = net_udp_send(addr->sin_addr, local_port, ntoh16(addr->sin_port),
                           (const void *)(uintptr_t)buf, (size_t)len);
    else if (type == SOCK_RAW_K)
        ret = net_icmp_send_echo(addr->sin_addr, local_port, 1,
                                 (const void *)(uintptr_t)buf, (size_t)len);
    return ret < 0 ? ret : (int)len;
}

int sys_recvfrom(uint32_t sd_arg, uint32_t buf, uint32_t len,
                 uint32_t addr_arg, uint32_t addrlen) {
    if (!user_range_writable(buf, len))
        return -1;
    if (addr_arg && (addrlen < sizeof(struct k_sockaddr_in) ||
        !user_range_writable(addr_arg, sizeof(struct k_sockaddr_in))))
        return -1;
    socket_lock();
    struct socket_entry *s = socket_get((int)sd_arg);
    if (!s) {
        socket_unlock();
        return -1;
    }
    int type = s->type;
    uint16_t local_port = s->local_port;
    uint32_t peer_ip = s->peer_ip;
    socket_unlock();
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    int ret;
    if (type == SOCK_DGRAM_K) {
        ret = net_udp_recv(local_port, &src_ip, &src_port, (void *)(uintptr_t)buf, (size_t)len);
    } else if (type == SOCK_RAW_K) {
        ret = net_icmp_recv_echo(peer_ip, local_port, 0, (void *)(uintptr_t)buf, (size_t)len);
        src_ip = peer_ip;
    } else {
        return -1;
    }
    if (ret >= 0 && addr_arg) {
        struct k_sockaddr_in *addr = (struct k_sockaddr_in *)(uintptr_t)addr_arg;
        addr->sin_family = AF_INET_K;
        addr->sin_port = ntoh16(src_port);
        addr->sin_addr = src_ip;
    }
    return ret;
}

int sys_closesocket(uint32_t sd_arg, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    socket_lock();
    struct socket_entry *s = socket_get((int)sd_arg);
    if (!s) {
        socket_unlock();
        return -1;
    }
    int close_tcp = s->type == SOCK_STREAM_K && (s->tcp.state != 0 || s->tcp.registered);
    struct net_tcp_pcb *tcp = &s->tcp;
    s->used = 2;
    s->owner = -1;
    s->connected = 0;
    socket_unlock();
    if (close_tcp)
        net_tcp_close_pcb(tcp);
    net_tcp_pcb_init(tcp);
    socket_lock();
    if ((int)sd_arg >= 0 && (int)sd_arg < MAX_SOCKETS &&
        sockets[sd_arg].used == 2 && &sockets[sd_arg].tcp == tcp) {
        socket_clear(&sockets[sd_arg]);
    }
    socket_unlock();
    return 0;
}

void sys_net_cleanup_owner(int owner) {
    if (owner < 0 || owner >= MAX_TASKS)
        return;
    for (int i = 0; i < MAX_SOCKETS; i++) {
        socket_lock();
        struct socket_entry *s = &sockets[i];
        if (s->used != 1 || s->owner != owner) {
            socket_unlock();
            continue;
        }
        int close_tcp = s->type == SOCK_STREAM_K &&
                        (s->tcp.state != 0 || s->tcp.registered);
        struct net_tcp_pcb *tcp = &s->tcp;
        s->used = 2;
        s->owner = -1;
        s->connected = 0;
        socket_unlock();

        if (close_tcp)
            net_tcp_close_pcb(tcp);
        net_tcp_pcb_init(tcp);

        socket_lock();
        if (s->used == 2)
            socket_clear(s);
        socket_unlock();
    }
}

int sys_dns_resolve(uint32_t host_arg, uint32_t ip_out_arg, uint32_t c,
                    uint32_t d, uint32_t e) {
    (void)c; (void)d; (void)e;
    const char *host = (const char *)(uintptr_t)host_arg;
    if (!user_string_ok(host) || !user_range_writable(ip_out_arg, sizeof(uint32_t)))
        return -1;
    return net_dns_resolve(host, (uint32_t *)(uintptr_t)ip_out_arg);
}

int sys_netinfo(uint32_t mac_arg, uint32_t ip_arg, uint32_t c, uint32_t d, uint32_t e) {
    (void)c; (void)d; (void)e;
    if (mac_arg && !user_range_writable(mac_arg, 6))
        return -1;
    if (ip_arg && !user_range_writable(ip_arg, sizeof(uint32_t)))
        return -1;
    if (mac_arg) {
        uint8_t *mac = (uint8_t *)(uintptr_t)mac_arg;
        for (int i = 0; i < 6; i++)
            mac[i] = net_mac[i];
    }
    if (ip_arg)
        *(uint32_t *)(uintptr_t)ip_arg = net_ip;
    return 0;
}
