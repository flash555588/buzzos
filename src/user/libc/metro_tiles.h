#ifndef BUZZOS_METRO_TILES_H
#define BUZZOS_METRO_TILES_H

#include <stdint.h>

/* Windows 8.1's 70px small cell and 10px gutters produce 150px medium,
 * 310x150 wide and 310px large tiles. Coordinates use the small-cell grid. */
struct metro_tile {
    const char *name;
    int group, col, row, cols, rows;
    uint32_t color;
};

static const struct metro_tile metro_tiles[] = {
    {"terminal",    0, 0, 0, 4, 2, 0xDA532Cu},
    {"calculator",  0, 4, 0, 2, 2, 0x009AAEu},
    {"filemanager", 0, 0, 2, 2, 2, 0xD79A00u},
    {"textedit",    0, 2, 2, 4, 2, 0x2B5797u},
    {"browser",     0, 0, 4, 4, 4, 0x0078D7u},
    {"paint",       1, 0, 0, 4, 4, 0xA200FFu},
    {"luaide",      1, 0, 4, 4, 2, 0x008A00u},
    {"taskmanager", 1, 0, 6, 4, 2, 0x008299u},
    {"music",       2, 0, 0, 4, 2, 0xD24726u},
    {"doom",        2, 0, 2, 2, 2, 0xB91D47u},
    {"gameboy",     2, 2, 2, 2, 2, 0x7E3878u},
    {"settings",    0, 4, 4, 1, 1, 0x5133ABu},
    {"search",      0, 5, 4, 1, 1, 0xAA00FFu},
};

static inline struct metro_tile metro_tile_for(const char *name, int fallback) {
    for (unsigned int i = 0; i < sizeof(metro_tiles) / sizeof(metro_tiles[0]); i++)
        if (name && strcmp(name, metro_tiles[i].name) == 0) return metro_tiles[i];
    int index = fallback > 10 ? fallback - 11 : fallback;
    return (struct metro_tile){name, 3, (index % 2) * 2, (index / 2) * 2,
                               2, 2, 0x5133ABu};
}

static inline int metro_scale(int height) {
    int scale = height * 100 / 630;
    return scale > 100 ? 100 : (scale < 30 ? 30 : scale);
}

static inline int metro_unit(int scale) { return 70 * scale / 100; }
static inline int metro_gap(int scale) { return 10 * scale / 100; }
static inline int metro_span(int cells, int scale) {
    return cells * metro_unit(scale) + (cells - 1) * metro_gap(scale);
}
static inline int metro_group_x(int group, int scale) {
    if (!group) return 0;
    return metro_span(6, scale) + 60 * scale / 100 +
           (group - 1) * (metro_span(4, scale) + 60 * scale / 100);
}

#endif
