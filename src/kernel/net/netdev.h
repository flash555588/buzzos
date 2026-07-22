#ifndef BUZZOS_NETDEV_H
#define BUZZOS_NETDEV_H
#include <stdint.h>
#include <stddef.h>

struct netdev {
    uint8_t  mac[6];
    void    *priv;

    int    (*init)(struct netdev *dev);
    int    (*send)(struct netdev *dev, const void *data, size_t len);
    size_t (*recv)(struct netdev *dev, void *buf, size_t max);
};

void netdev_register(struct netdev *dev);
struct netdev *netdev_get(void);

/* Built-in drivers */
void ne2000_init_device(void);
void ne2000_irq_handler(void);

#endif
