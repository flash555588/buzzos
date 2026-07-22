#ifndef BUZZOS_NETSURF_GUI_PLOT_H
#define BUZZOS_NETSURF_GUI_PLOT_H

#include <stdint.h>

struct buzzos_plot_target {
    uint8_t *pixels;
    int width;
    int height;
    int offset_y;
    int clip_x0;
    int clip_y0;
    int clip_x1;
    int clip_y1;
};

struct plotter_table;
struct gui_layout_table;
extern const struct plotter_table buzzos_plotters;
extern struct gui_layout_table *buzzos_layout_table;
void buzzos_plot_target_init(struct buzzos_plot_target *target,
                             uint8_t *pixels, int width, int height,
                             int offset_y);

#endif
