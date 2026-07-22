#include "rtc.h"
#include "io.h"

struct rtc_fields {
    uint8_t second, minute, hour, day, month, year, century, status_b;
};

static uint8_t cmos_read(uint8_t index) {
    outb(0x70, (uint8_t)(0x80u | index));
    io_wait();
    return inb(0x71);
}

static int rtc_sample(struct rtc_fields *out) {
    for (int wait = 0; wait < 10000; wait++) {
        if ((cmos_read(0x0A) & 0x80u) == 0)
            break;
        if (wait == 9999)
            return -1;
    }
    out->second = cmos_read(0x00);
    out->minute = cmos_read(0x02);
    out->hour = cmos_read(0x04);
    out->day = cmos_read(0x07);
    out->month = cmos_read(0x08);
    out->year = cmos_read(0x09);
    out->status_b = cmos_read(0x0B);
    out->century = cmos_read(0x32);
    return 0;
}

static int same_sample(const struct rtc_fields *a, const struct rtc_fields *b) {
    return a->second == b->second && a->minute == b->minute &&
           a->hour == b->hour && a->day == b->day &&
           a->month == b->month && a->year == b->year &&
           a->century == b->century && a->status_b == b->status_b;
}

static uint8_t from_bcd(uint8_t value) {
    return (uint8_t)((value & 0x0Fu) + ((value >> 4) * 10u));
}

static int leap_year(uint32_t year) {
    return (year % 4u == 0u && year % 100u != 0u) || year % 400u == 0u;
}

int32_t rtc_unix_time(void) {
    struct rtc_fields a, b;
    int stable = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        if (rtc_sample(&a) < 0 || rtc_sample(&b) < 0)
            return -1;
        if (same_sample(&a, &b)) {
            stable = 1;
            break;
        }
    }
    if (!stable)
        return -1;

    int pm = (a.hour & 0x80u) != 0;
    a.hour &= 0x7Fu;
    if ((a.status_b & 0x04u) == 0) {
        a.second = from_bcd(a.second);
        a.minute = from_bcd(a.minute);
        a.hour = from_bcd(a.hour);
        a.day = from_bcd(a.day);
        a.month = from_bcd(a.month);
        a.year = from_bcd(a.year);
        a.century = from_bcd(a.century);
    }
    if ((a.status_b & 0x02u) == 0) {
        if (pm && a.hour < 12) a.hour = (uint8_t)(a.hour + 12);
        if (!pm && a.hour == 12) a.hour = 0;
    }

    uint32_t year = (a.century >= 19 && a.century <= 99) ?
        (uint32_t)a.century * 100u + a.year : 2000u + a.year;
    static const uint8_t month_days[12] =
        {31,28,31,30,31,30,31,31,30,31,30,31};
    if (year < 1970 || year > 2037 || a.month < 1 || a.month > 12 ||
        a.day < 1 || a.hour > 23 || a.minute > 59 || a.second > 60)
        return -1;
    uint32_t max_day = month_days[a.month - 1];
    if (a.month == 2 && leap_year(year)) max_day++;
    if (a.day > max_day)
        return -1;

    uint32_t days = 0;
    for (uint32_t y = 1970; y < year; y++)
        days += leap_year(y) ? 366u : 365u;
    for (uint32_t m = 1; m < a.month; m++) {
        days += month_days[m - 1];
        if (m == 2 && leap_year(year)) days++;
    }
    days += (uint32_t)a.day - 1u;
    return (int32_t)(days * 86400u + (uint32_t)a.hour * 3600u +
                     (uint32_t)a.minute * 60u + a.second);
}
