#include "libc.h"

enum {
    FIRST_SIZE = 192 * 1024,
    ZERO_SIZE = 64 * 1024,
    GROWN_SIZE = 320 * 1024,
};

static int fail(const char *stage) {
    printf("heaptest: failed %s\n", stage);
    return 1;
}

int main(void) {
    uint8_t *first = malloc(FIRST_SIZE);
    if (!first)
        return fail("malloc");
    for (int i = 0; i < FIRST_SIZE; i++)
        first[i] = (uint8_t)(i * 37 + 11);

    uint8_t *zeroed = calloc(ZERO_SIZE, 1);
    if (!zeroed)
        return fail("calloc");
    for (int i = 0; i < ZERO_SIZE; i++)
        if (zeroed[i] != 0)
            return fail("calloc-zero");

    uint8_t *grown = realloc(first, GROWN_SIZE);
    if (!grown)
        return fail("realloc");
    for (int i = 0; i < FIRST_SIZE; i++)
        if (grown[i] != (uint8_t)(i * 37 + 11))
            return fail("realloc-data");

    free(zeroed);
    free(grown);
    uint8_t *reused = malloc(GROWN_SIZE);
    if (!reused)
        return fail("reuse");
    free(reused);

    if (calloc((size_t)-1, 2) != 0)
        return fail("overflow");
    puts("heaptest: ok 320K realloc reuse");
    return 0;
}
