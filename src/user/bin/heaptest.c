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

static int check_large_requests(void) {
    const size_t sizes[] = {
        (size_t)UINT64_C(0x100000001),
        (size_t)UINT64_C(0x100001000),
        (size_t)UINT64_C(0x200000040),
        (size_t)-16,
        (size_t)-1,
    };
    uint8_t *original = malloc(64);
    if (!original)
        return fail("large-request-setup");
    memset(original, 0x5a, 64);
    for (size_t index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
        void *unexpected = malloc(sizes[index]);
        if (unexpected) {
            free(unexpected);
            free(original);
            return fail("large-malloc-truncated");
        }
        unexpected = realloc(original, sizes[index]);
        if (unexpected) {
            free(unexpected);
            return fail("large-realloc-truncated");
        }
        for (size_t offset = 0; offset < 64; offset++)
            if (original[offset] != 0x5a) {
                free(original);
                return fail("failed-realloc-data");
            }
        unexpected = calloc(1, sizes[index]);
        if (unexpected) {
            free(unexpected);
            free(original);
            return fail("large-calloc-truncated");
        }
    }
    free(original);
    puts("heaptest: ok 64-bit allocation boundaries");
    return 0;
}

static long free_page_count(void) {
    char text[1024];
    int descriptor = open("/proc/health", O_RDONLY);
    if (descriptor < 0)
        return -1;
    int length = read(descriptor, text, sizeof(text) - 1);
    close(descriptor);
    if (length < 0)
        return -1;
    text[length] = 0;
    const char *value = strstr(text, "mem_free_pages ");
    return value ? strtol(value + strlen("mem_free_pages "), NULL, 10) : -1;
}

static int check_physical_reclamation(void) {
    long before = free_page_count();
    uint8_t *large = malloc(8u * 1024u * 1024u);
    if (before < 0 || !large)
        return fail("physical-setup");
    memset(large, 0x6a, 8u * 1024u * 1024u);
    long during = free_page_count();
    free(large);
    long after = free_page_count();
    if (during < 0 || after < 0 || after - during < 1900 || after < before - 20)
        return fail("physical-free-pages");
    printf("heaptest: ok physical reclamation before=%ld during=%ld after=%ld\n", before, during, after);
    before = free_page_count();
    void *impossible = malloc(512u * 1024u * 1024u);
    if (impossible) {
        free(impossible);
        return fail("physical-oom-expected");
    }
    after = free_page_count();
    if (after < before)
        return fail("physical-oom-rollback");
    large = malloc(8u * 1024u * 1024u);
    if (!large)
        return fail("physical-after-oom");
    memset(large, 0x77, 8u * 1024u * 1024u);
    free(large);
    puts("heaptest: ok physical OOM rollback and reuse");
    large = malloc(8u * 1024u * 1024u);
    uint8_t *blocker = malloc(128);
    if (!large || !blocker)
        return fail("physical-interior-setup");
    memset(blocker, 0x5c, 128);
    during = free_page_count();
    free(large);
    after = free_page_count();
    if (after - during < 1900)
        return fail("physical-interior-release");
    large = malloc(8u * 1024u * 1024u);
    if (!large)
        return fail("physical-interior-recommit");
    memset(large, 0x5a, 8u * 1024u * 1024u);
    for (size_t index = 0; index < 128; index++)
        if (blocker[index] != 0x5c)
            return fail("physical-interior-live-data");
    free(large);
    free(blocker);
    puts("heaptest: ok physical interior reclamation and recommit");
    return 0;
}

static uint32_t next_random(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static int check_pattern(const uint8_t *data, size_t size, uint8_t expected) {
    for (size_t offset = 0; offset < size; offset++)
        if (data[offset] != expected)
            return 0;
    return 1;
}

static int check_mixed_allocations(void) {
    uint8_t *blocks[48] = {0};
    size_t sizes[48] = {0};
    uint32_t state = 0x42555a5au;
    for (size_t step = 0; step < 4096; step++) {
        size_t slot = (next_random(&state) >> 16) % 48u;
        size_t size = next_random(&state) % (128u * 1024u) + 1u;
        unsigned operation = (next_random(&state) >> 16) % 4u;
        uint8_t pattern = (uint8_t)(slot + 1u);
        if (blocks[slot] && !check_pattern(blocks[slot], sizes[slot], pattern))
            return fail("mixed-data-before");
        if (operation == 0) {
            free(blocks[slot]);
            blocks[slot] = 0;
            sizes[slot] = 0;
            continue;
        }
        uint8_t *replacement;
        if (blocks[slot]) {
            replacement = realloc(blocks[slot], size);
            if (replacement && !check_pattern(replacement,
                    sizes[slot] < size ? sizes[slot] : size, pattern))
                return fail("mixed-realloc-data");
        } else if (operation == 1) {
            replacement = calloc(size, 1);
            if (replacement && !check_pattern(replacement, size, 0))
                return fail("mixed-calloc-zero");
        } else {
            replacement = malloc(size);
        }
        if (!replacement || ((uintptr_t)replacement & 15u))
            return fail("mixed-allocation");
        memset(replacement, pattern, size);
        blocks[slot] = replacement;
        sizes[slot] = size;
    }
    for (size_t slot = 0; slot < 48; slot++) {
        if (blocks[slot] && !check_pattern(blocks[slot], sizes[slot], (uint8_t)(slot + 1u)))
            return fail("mixed-data-final");
        free(blocks[slot]);
    }
    puts("heaptest: ok 4096 mixed allocation cycles");
    return 0;
}

int main(void) {
    if (check_large_requests())
        return 1;
    void *small[33];
    for (int i = 0; i < 33; i++) {
        small[i] = malloc((size_t)i + 1);
        if (!small[i] || ((uintptr_t)small[i] & 15u))
            return fail("malloc-alignment");
    }
    for (int i = 0; i < 33; i += 2) {
        void *grown = realloc(small[i], (size_t)i + 129);
        if (!grown || ((uintptr_t)grown & 15u))
            return fail("realloc-alignment");
        small[i] = grown;
    }
    for (int i = 0; i < 33; i++)
        free(small[i]);
    void *aligned = calloc(3, 7);
    if (!aligned || ((uintptr_t)aligned & 15u))
        return fail("calloc-reuse-alignment");
    free(aligned);
    puts("heaptest: ok alignment 16");

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
    if (check_mixed_allocations())
        return 1;
    if (check_physical_reclamation())
        return 1;
    return 0;
}
