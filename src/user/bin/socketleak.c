#include "libc.h"

int main(void) {
    int opened = 0;
    for (int i = 0; i < 8; i++) {
        if (socket(AF_INET, SOCK_DGRAM, 0) < 0)
            break;
        opened++;
    }
    printf("socketleak: opened %d\n", opened);
    return opened == 8 ? 0 : 1;
}
