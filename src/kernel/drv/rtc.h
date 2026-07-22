#ifndef BUZZOS_RTC_H
#define BUZZOS_RTC_H

#include <stdint.h>

/* Read the PC/AT CMOS real-time clock and return Unix UTC seconds.
 * Returns -1 if the clock is updating or contains an invalid date. */
int32_t rtc_unix_time(void);

#endif
