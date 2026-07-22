#ifndef BUZZOS_MATH_COMPAT_H
#define BUZZOS_MATH_COMPAT_H

#include "libc.h"

#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define isnan(value) __builtin_isnan(value)
#define isfinite(value) __builtin_isfinite(value)

double tan(double value);
double floor(double value);
double ceil(double value);
float ceilf(float value);
double round(double value);
double fmod(double value, double divisor);
double pow(double value, double exponent);

#endif
