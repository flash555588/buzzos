#include <stdint.h>

int main(void) {
    volatile const uint32_t *unmapped = (volatile const uint32_t *)0x02700000u;
    return (int)*unmapped;
}
