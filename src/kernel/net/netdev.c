#include "netdev.h"

static struct netdev *g_dev;

void netdev_register(struct netdev *dev) { g_dev = dev; }
struct netdev *netdev_get(void)          { return g_dev; }

int netdev_init(void) {
    g_dev = 0;
    if (pcnet_init_device() == 0)
        return 0;
    if (ne2000_init_device() == 0)
        return 0;
    return -1;
}
