#ifndef BUZZOS_MATH_COMPAT_H
#define BUZZOS_MATH_COMPAT_H

#include "libc.h"

#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
#define HUGE_VAL (__builtin_huge_val())
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define isnan(value) __builtin_isnan(value)
#define isfinite(value) __builtin_isfinite(value)
#define isinf(value) __builtin_isinf(value)
#define signbit(value) __builtin_signbit(value)

double tan(double value);
double asin(double value);
double acos(double value);
double atan(double value);
double atan2(double y, double x);
double exp(double value);
double log(double value);
double log2(double value);
double log10(double value);
double frexp(double value, int *exponent);
double ldexp(double value, int exponent);
double floor(double value);
double ceil(double value);
float ceilf(float value);
static inline float fabsf(float value) { return __builtin_fabsf(value); }
double round(double value);
static inline float roundf(float value) { return (float)round((double)value); }
static inline long lroundf(float value) {
    double v = round((double)value);
    if (!isfinite(v) || v >= 9223372036854775808.0 || v < -9223372036854775808.0)
        return (-9223372036854775807L - 1L);
    return (long)v;
}
double trunc(double value);
double cbrt(double value);
double fmod(double value, double divisor);
double pow(double value, double exponent);
/* Implemented by the native browser's OpenLibm portability archive. */
double scalbn(double value, int exponent);
double copysign(double value, double sign);
long lrint(double value);
double hypot(double x, double y);
double sinh(double value);
double cosh(double value);
double tanh(double value);
double acosh(double value);
double asinh(double value);
double atanh(double value);
double expm1(double value);
double log1p(double value);
static inline float floorf(float v) { return (float)floor(v); }
static inline float sqrtf(float v) { return (float)sqrt(v); }
static inline float sinf(float v) { return (float)sin(v); }
static inline float cosf(float v) { return (float)cos(v); }
static inline float expf(float v) { return (float)exp(v); }
static inline float powf(float v, float e) { return (float)pow(v, e); }
static inline float fmodf(float v, float d) { return (float)fmod(v, d); }
static inline float hypotf(float x, float y) { return (float)hypot(x, y); }
static inline double fmin(double x, double y) {
    if (isnan(x)) return y;
    if (isnan(y)) return x;
    return x == y ? (signbit(x) ? x : y) : x < y ? x : y;
}
static inline double fmax(double x, double y) {
    if (isnan(x)) return y;
    if (isnan(y)) return x;
    return x == y ? (signbit(x) ? y : x) : x > y ? x : y;
}
static inline float fminf(float x, float y) { return (float)fmin(x, y); }
static inline float fmaxf(float x, float y) { return (float)fmax(x, y); }

#endif
