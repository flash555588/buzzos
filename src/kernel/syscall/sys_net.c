#include <stddef.h>
#include <stdint.h>
#include "net.h"
#include "serial.h"
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
    NET_SYSCALL_BUFFER = 4096,
    NET_DATAGRAM_MAX = 1472,
    SOCKET_STATE_FREE = 0,
    SOCKET_STATE_OPEN,
    SOCKET_STATE_CONNECTING,
    SOCKET_STATE_CONNECTED,
    SOCKET_STATE_CLOSING,
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
    int state;
    uint32_t refs;
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
    if (sockets[sd].used != 1 || sockets[sd].owner != socket_owner() ||
        sockets[sd].state == SOCKET_STATE_CLOSING)
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
    s->state = SOCKET_STATE_FREE;
    s->refs = 0;
}

static void socket_release(struct socket_entry *s) {
    socket_lock();
    if (s && s->refs)
        s->refs--;
    socket_unlock();
}

intptr_t sys_socket(uintptr_t domain, uintptr_t type, uintptr_t protocol, uintptr_t d, uintptr_t e) {
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
            sockets[i].state = SOCKET_STATE_OPEN;
            sockets[i].refs = 0;
            net_tcp_pcb_init(&sockets[i].tcp);
            socket_unlock();
            return i;
        }
    }
    socket_unlock();
    return -1;
}

intptr_t sys_connect(uintptr_t sd_arg, uintptr_t addr_arg, uintptr_t addrlen,
                uintptr_t d, uintptr_t e) {
    (void)d; (void)e;
    struct k_sockaddr_in addr;
    if (addrlen < sizeof(addr) ||
        copy_from_user(&addr, addr_arg, sizeof(addr)) < 0) {
        serial_puts("[net] connect: bad address\n");
        return -1;
    }
    if (addr.sin_family != AF_INET_K) {
        serial_puts("[net] connect: bad family\n");
        return -1;
    }
    uint32_t peer_ip = addr.sin_addr;
    uint16_t peer_port = ntoh16(addr.sin_port);

    socket_lock();
    struct socket_entry *s = socket_get((int)sd_arg);
    if (!s) {
        serial_puts("[net] connect: bad socket\n");
        socket_unlock();
        return -1;
    }
    if (s->type == SOCK_DGRAM_K || s->type == SOCK_RAW_K) {
        s->peer_ip = peer_ip;
        s->peer_port = peer_port;
        s->connected = 1;
        s->state = SOCKET_STATE_CONNECTED;
        socket_unlock();
        return 0;
    }
    if (s->type != SOCK_STREAM_K || s->state != SOCKET_STATE_OPEN) {
        serial_puts("[net] connect: invalid state\n");
        socket_unlock();
        return -1;
    }
    struct net_tcp_pcb *tcp = &s->tcp;
    struct socket_entry *held = s;
    net_tcp_pcb_init(tcp);
    s->state = SOCKET_STATE_CONNECTING;
    s->refs++;
    socket_unlock();

    int ret = net_tcp_connect_pcb(tcp, peer_ip, peer_port);
    socket_lock();
    s = socket_get((int)sd_arg);
    if (ret < 0) {
        if (s && s->type == SOCK_STREAM_K &&
            s->state == SOCKET_STATE_CONNECTING) {
            s->connected = 0;
            s->state = SOCKET_STATE_OPEN;
            net_tcp_pcb_init(&s->tcp);
        }
        if (held->refs) held->refs--;
        socket_unlock();
        return -1;
    }
    if (s && s->type == SOCK_STREAM_K &&
        s->state == SOCKET_STATE_CONNECTING) {
        s->peer_ip = peer_ip;
        s->peer_port = peer_port;
        s->connected = 1;
        s->state = SOCKET_STATE_CONNECTED;
        if (s->refs) s->refs--;
        socket_unlock();
        return 0;
    }
    int closing = held->state == SOCKET_STATE_CLOSING;
    if (held->refs) held->refs--;
    socket_unlock();
    if (!closing)
        net_tcp_close_pcb(tcp);
    return -1;
}

intptr_t sys_send(uintptr_t sd_arg, uintptr_t buf, uintptr_t len, uintptr_t flags, uintptr_t e) {
    (void)flags; (void)e;
    if (len > INT32_MAX)
        return -1;
    socket_lock();
    struct socket_entry *s = socket_get((int)sd_arg);
    if (!s || s->state != SOCKET_STATE_CONNECTED) {
        socket_unlock();
        return -1;
    }
    s->refs++;
    int type = s->type;
    uint16_t local_port = s->local_port;
    uint32_t peer_ip = s->peer_ip;
    uint16_t peer_port = s->peer_port;
    struct net_tcp_pcb *tcp = &s->tcp;
    socket_unlock();
    uint8_t bounce[NET_SYSCALL_BUFFER];
    int ret = -1;
    if (type == SOCK_STREAM_K) {
        size_t done = 0;
        while (done < len) {
            size_t chunk = len - done;
            if (chunk > sizeof(bounce)) chunk = sizeof(bounce);
            if (copy_from_user(bounce, buf + done, chunk) < 0) {
                ret = done ? (int)done : -1;
                break;
            }
            if (net_tcp_send_pcb(tcp, bounce, chunk) < 0) {
                ret = done ? (int)done : -1;
                break;
            }
            done += chunk;
            ret = (int)done;
        }
        if (len == 0) ret = 0;
    } else if (len > NET_DATAGRAM_MAX ||
               copy_from_user(bounce, buf, (size_t)len) < 0) {
        ret = -1;
    } else if (type == SOCK_DGRAM_K) {
        ret = net_udp_send(peer_ip, local_port, peer_port,
                           bounce, (size_t)len) < 0 ? -1 : (int)len;
    } else if (type == SOCK_RAW_K && len <= 1200u) {
        ret = net_icmp_send_echo(peer_ip, local_port, 1,
                                 bounce, (size_t)len) < 0 ? -1 : (int)len;
    }
    socket_release(s);
    return ret;
}

intptr_t sys_recv(uintptr_t sd_arg, uintptr_t buf, uintptr_t len, uintptr_t flags, uintptr_t e) {
    (void)flags; (void)e;
    socket_lock();
    struct socket_entry *s = socket_get((int)sd_arg);
    if (!s || s->state != SOCKET_STATE_CONNECTED) {
        socket_unlock();
        return -1;
    }
    s->refs++;
    int type = s->type;
    uint16_t local_port = s->local_port;
    uint32_t peer_ip = s->peer_ip;
    struct net_tcp_pcb *tcp = &s->tcp;
    socket_unlock();
    uint8_t bounce[NET_SYSCALL_BUFFER];
    size_t cap = len < sizeof(bounce) ? (size_t)len : sizeof(bounce);
    int ret = -1;
    if (type == SOCK_STREAM_K)
        ret = net_tcp_recv_pcb(tcp, bounce, cap);
    else if (type == SOCK_DGRAM_K)
        ret = net_udp_recv(local_port, 0, 0, bounce, cap);
    else if (type == SOCK_RAW_K)
        ret = net_icmp_recv_echo(peer_ip, local_port, 0, bounce, cap);
    if (ret > 0 && copy_to_user(buf, bounce, (size_t)ret) < 0)
        ret = -1;
    socket_release(s);
    return ret;
}

intptr_t sys_bind(uintptr_t sd_arg, uintptr_t addr_arg, uintptr_t addrlen,
             uintptr_t d, uintptr_t e) {
    (void)d; (void)e;
    struct k_sockaddr_in addr;
    if (addrlen < sizeof(addr) ||
        copy_from_user(&addr, addr_arg, sizeof(addr)) < 0)
        return -1;
    if (addr.sin_family != AF_INET_K)
        return -1;
    socket_lock();
    struct socket_entry *s = socket_get((int)sd_arg);
    if (!s) {
        socket_unlock();
        return -1;
    }
    s->local_port = ntoh16(addr.sin_port);
    socket_unlock();
    return 0;
}

intptr_t sys_sendto(uintptr_t sd_arg, uintptr_t buf, uintptr_t len,
               uintptr_t addr_arg, uintptr_t addrlen) {
    struct k_sockaddr_in addr;
    uint8_t bounce[NET_DATAGRAM_MAX];
    if (len > sizeof(bounce) || len > INT32_MAX || addrlen < sizeof(addr) ||
        copy_from_user(&addr, addr_arg, sizeof(addr)) < 0 ||
        copy_from_user(bounce, buf, (size_t)len) < 0)
        return -1;
    if (addr.sin_family != AF_INET_K)
        return -1;
    socket_lock();
    struct socket_entry *s = socket_get((int)sd_arg);
    if (!s) {
        socket_unlock();
        return -1;
    }
    s->refs++;
    int type = s->type;
    uint16_t local_port = s->local_port;
    socket_unlock();
    int ret = -1;
    if (type == SOCK_DGRAM_K)
        ret = net_udp_send(addr.sin_addr, local_port, ntoh16(addr.sin_port),
                           bounce, (size_t)len);
    else if (type == SOCK_RAW_K && len <= 1200u)
        ret = net_icmp_send_echo(addr.sin_addr, local_port, 1,
                                 bounce, (size_t)len);
    socket_release(s);
    return ret < 0 ? ret : (int)len;
}

intptr_t sys_recvfrom(uintptr_t sd_arg, uintptr_t buf, uintptr_t len,
                 uintptr_t addr_arg, uintptr_t addrlen) {
    if (addr_arg && addrlen < sizeof(struct k_sockaddr_in))
        return -1;
    socket_lock();
    struct socket_entry *s = socket_get((int)sd_arg);
    if (!s) {
        socket_unlock();
        return -1;
    }
    s->refs++;
    int type = s->type;
    uint16_t local_port = s->local_port;
    uint32_t peer_ip = s->peer_ip;
    socket_unlock();
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    uint8_t bounce[NET_SYSCALL_BUFFER];
    size_t cap = len < sizeof(bounce) ? (size_t)len : sizeof(bounce);
    int ret;
    if (type == SOCK_DGRAM_K) {
        ret = net_udp_recv(local_port, &src_ip, &src_port, bounce, cap);
    } else if (type == SOCK_RAW_K) {
        ret = net_icmp_recv_echo(peer_ip, local_port, 0, bounce, cap);
        src_ip = peer_ip;
    } else {
        socket_release(s);
        return -1;
    }
    if (ret > 0 && copy_to_user(buf, bounce, (size_t)ret) < 0)
        ret = -1;
    if (ret >= 0 && addr_arg) {
        struct k_sockaddr_in addr;
        addr.sin_family = AF_INET_K;
        addr.sin_port = ntoh16(src_port);
        addr.sin_addr = src_ip;
        if (copy_to_user(addr_arg, &addr, sizeof(addr)) < 0)
            ret = -1;
    }
    socket_release(s);
    return ret;
}

intptr_t sys_closesocket(uintptr_t sd_arg, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
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
    s->state = SOCKET_STATE_CLOSING;
    s->connected = 0;
    socket_unlock();
    for (;;) {
        socket_lock();
        uint32_t refs = s->refs;
        socket_unlock();
        if (!refs)
            break;
        task_yield();
    }
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
        s->state = SOCKET_STATE_CLOSING;
        s->connected = 0;
        /* Owner cleanup runs after every task in the process is dead, so no
         * operation can resume to release an outstanding reference. */
        s->refs = 0;
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

intptr_t sys_dns_resolve(uintptr_t host_arg, uintptr_t ip_out_arg, uintptr_t c,
                    uintptr_t d, uintptr_t e) {
    (void)c; (void)d; (void)e;
    char host[256];
    uint32_t ip;
    if (copy_string_from_user(host, sizeof(host), host_arg) < 0)
        return -1;
    if (net_dns_resolve(host, &ip) < 0)
        return -1;
    return copy_to_user(ip_out_arg, &ip, sizeof(ip));
}

intptr_t sys_netinfo(uintptr_t mac_arg, uintptr_t ip_arg, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)c; (void)d; (void)e;
    if (mac_arg && copy_to_user(mac_arg, net_mac, 6) < 0)
        return -1;
    if (ip_arg && copy_to_user(ip_arg, &net_ip, sizeof(net_ip)) < 0)
        return -1;
    return 0;
}
