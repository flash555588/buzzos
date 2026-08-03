#ifndef BUZZOS_GUI_EVENT_H
#define BUZZOS_GUI_EVENT_H

/* Wake the current display server after an input IRQ publishes new state.
 * This is IRQ-safe: callers do not need to know which process owns display. */
void gui_event_notify_display(void);

#endif
