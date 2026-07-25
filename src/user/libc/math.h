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
double round(double value);
double fmod(double value, double divisor);
double pow(double value, double exponent);

#endif
