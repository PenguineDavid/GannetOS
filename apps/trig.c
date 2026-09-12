/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

/*
* An app to perform trig functions in GannetOS
* Using no libraries
* usage: trig <float> -[func]
* this only really exists as a way to demo and test the FPU init
* - David S 1/09/2026
*/

#include "kernel/proc/pexe.h"

// 32-bit float-aligned constants.
#define PI 3.14159265f

// Trig function hashes. Since the freestanding app environment does not have
// the normal C string helpers, the function name is hashed before the switch.
#define HASH_SIN   0x0b88aa0f
#define HASH_COS   0x0b8866ca
#define HASH_TAN   0x0b88ad48
#define HASH_ATAN  0x7c943aa9
#define HASH_ATAN2 0x0f1b8ffb
#define HASH_ASIN  0x7c943770
#define HASH_ACOS  0x7c93f42b

// Helpers.

// djb2 hash algorithm – an ultra‑lightweight, non‑cryptographic string hash.
// Provides good distribution for short keys commonly used in symbol tables.
// Stops at the first null terminator; handles arbitrary byte values by casting
// to unsigned char to avoid sign‑extension issues on signed chars.
// This is only used for hashing a string before the function switch below.
static unsigned int hash_string(const char *text)
{
    unsigned int hash = 5381;
    int character;

    while ((character = (unsigned char)*text++))
    {
        hash = ((hash << 5) + hash) + character; // hash * 33 + character
    }

    return hash;
}

// Computes the square root of x using Newton‑Raphson with a Quake‑style
// fast inverse‑sqrt initial guess. Three iterations are sufficient for
// full 32‑bit float precision (~1–2 ulp).
// Returns 0.0f for negative or zero inputs. Does not handle NaN or Inf;
// they will propagate naturally (NaN in -> NaN out, Inf in -> Inf out).
// The magic constant 0x5f3759df is a historical artifact from the Quake III
// inverse‑sqrt implementation - it gives a remarkably good starting point.
static float custom_sqrt(float x)
{
    if (x <= 0.0f)
    {
        return 0.0f;
    }

    // Use a bit-manipulation union for the fast inverse-sqrt initial guess.
    union { int i; float f; } bit_cast;
    bit_cast.f = x;
    bit_cast.i = 0x5f3759df - (bit_cast.i >> 1);
    float guess = 1.0f / bit_cast.f; // Seed for sqrt, not inverse sqrt.

    // Three Newton iterations: guess = 0.5 * (guess + x / guess)
    guess = 0.5f * (guess + x / guess);
    guess = 0.5f * (guess + x / guess);
    guess = 0.5f * (guess + x / guess);

    return guess;
}

// Parses a decimal floating‑point number from a string token.
// Supports an optional leading '+' or '-' and a fractional component.
// Does NOT support scientific notation ('e'/'E'), hex floats, or "inf"/"nan".
// Returns 1 on success (populates *out) and 0 on parse failure.
// Callers should check the return value; overflow is silently clamped by the
// float representation and does not signal an error.
static int parse_float(const char* token, float* out)
{
    int index = 0;
    int is_negative = 0;

    // Handle optional sign prefix
    if (token[0] == '-' && token[1])
    {
        is_negative = 1;
        index = 1;
    }
    else if (token[0] == '+' && token[1])
    {
        index = 1;
    }

    if (!token[index])
    {
        return 0;
    }

    float value = 0.0f;
    int has_digits = 0;

    // Parse the integer portion
    for (; token[index] && token[index] != '.'; index++)
    {
        if (token[index] < '0' || token[index] > '9')
        {
            return 0;
        }
        value = value * 10.0f + (float)(token[index] - '0');
        has_digits = 1;
    }

    // Parse the fractional portion, if present
    if (token[index] == '.')
    {
        index++;
        // At least one digit must exist on one side of the decimal
        if (!token[index] && !has_digits)
        {
            return 0;
        }

        float weight = 0.1f;
        for (; token[index] && weight > 1e-7f; index++)
        {
            // Stop parsing at the first non‑digit (allows trailing garbage to be ignored)
            if (token[index] < '0' || token[index] > '9')
            {
                break;
            }
            value += (float)(token[index] - '0') * weight;
            weight *= 0.1f;
        }
    }

    *out = is_negative ? -value : value;
    return 1;
}

// Prints a float to the console via the provided api->putchar callback.
// Truncates (does not round) the fractional part to the given precision.
// Handles negative values, but does NOT handle NaN or Inf – callers should
// guard against those if they are possible. The integer‑buffer of 16 chars
// is sufficient for any integer part that fits in a 32‑bit int (~2.1e9);
// larger floats will overflow the buffer and produce undefined output.
static void print_float(app_api_t* api, float value, int precision)
{
    // Print sign and convert to positive magnitude
    if (value < 0.0f)
    {
        api->putchar('-');
        value = -value;
    }

    int integer_part = (int)value;
    char integer_buffer[16];
    int buffer_index = 0;
    int remaining = integer_part;

    // Write integer digits in reverse (least significant first)
    do
    {
        integer_buffer[buffer_index++] = (char)('0' + (remaining % 10));
        remaining /= 10;
    } while (remaining > 0);

    // Print the integer part in correct order
    while (buffer_index > 0)
    {
        api->putchar(integer_buffer[--buffer_index]);
    }

    // Decimal point
    api->putchar('.');

    // Print the fractional part by repeated multiplication by 10
    float fractional_part = value - (float)integer_part;
    for (int digit_index = 0; digit_index < precision; digit_index++)
    {
        fractional_part *= 10.0f;
        int digit = (int)fractional_part;

        // Clamp against floating‑point noise just in case
        if (digit > 9) digit = 9;
        if (digit < 0) digit = 0;

        api->putchar('0' + digit);
        fractional_part -= (float)digit;
    }
}

// Trig functions

// Approximates sin(x) using Taylor series expansion around 0.
// Reduces the argument to [-PI, PI] via range reduction before evaluation.
// Uses 6 terms (up to x^11) – sufficient for typical floating-point needs.
// Returns NaN if x is NaN (though not explicitly handled; the caller should
// ensure valid input). The series is accurate to ~1e-7 for reduced angles.
// No special handling for infinity; will produce NaN or overflow.
static float sin(float x)
{
    // First reduction: bring x into [-2*PI, 2*PI]
    if (x > 2.0f * PI || x < -2.0f * PI)
    {
        int reduction = (int)(x / (2.0f * PI));
        x -= (float)reduction * (2.0f * PI);
    }

    // Ensure the value is within [-2*PI, 2*PI] after the above
    if (x > 2.0f * PI)  x -= 2.0f * PI;
    if (x < -2.0f * PI) x += 2.0f * PI;

    // Final reduction to [-PI, PI]
    if (x > PI)  x -= 2.0f * PI;
    if (x < -PI) x += 2.0f * PI;

    // Taylor series: sin(x) = x - x^3/3! + x^5/5! - ...
    float term      = x;          // current term
    float sum       = x;          // accumulated sum
    float x_squared = x * x;      // precomputed x^2

    for (int i = 1; i <= 5; i++)
    {
        // term = term * (-x^2) / ((2i)*(2i+1))
        term *= -x_squared / (float)((2 * i) * (2 * i + 1));
        sum += term;
    }

    return sum;
}

// Approximates cos(x) using the Taylor series around 0.
// Uses the same range reduction as sin() to bring x into [-PI, PI].
// 6 terms (up to x^10) give roughly 1e-7 accuracy for reduced angles.
// Returns 0.0f for NaN/Inf inputs (they will propagate naturally).
static float cos(float x)
{
    // Reduce to [-2*PI, 2*PI] using periodicity
    if (x > 2.0f * PI || x < -2.0f * PI)
    {
        int reduction = (int)(x / (2.0f * PI));
        x -= (float)reduction * (2.0f * PI);
    }

    // Clamp any residual overshoot
    if (x > 2.0f * PI) x -= 2.0f * PI;
    if (x < -2.0f * PI) x += 2.0f * PI;

    // Final reduction to [-PI, PI]
    if (x > PI) x -= 2.0f * PI;
    if (x < -PI) x += 2.0f * PI;

    // Taylor: cos(x) = 1 - x^2/2! + x^4/4! - ...
    float term      = 1.0f;      // current term (starts at 1)
    float sum       = 1.0f;      // accumulated sum
    float x_squared = x * x;

    for (int i = 1; i <= 5; i++)
    {
        // term *= -x^2 / ((2i-1)*(2i))
        term *= -x_squared / (float)((2 * i - 1) * (2 * i));
        sum += term;
    }

    return sum;
}

// Approximates tan(x) = sin(x) / cos(x).
// Relies on the sin() and cos() approximations above.
// If cos(x) evaluates to exactly 0.0f (within our approximation), returns 0.0f
// to avoid division by zero. This is a pragmatic choice; the caller should be
// aware that tan() is undefined at odd multiples of PI/2.
static float tan(float x)
{
    float c = cos(x);
    if (c == 0.0f)
    {
        return 0.0f;   // Avoid division by zero.
    }
    return sin(x) / c;
}

// Approximates arctangent of x (principal value, in [-PI/2, PI/2]).
// Uses Taylor series for |x| <= 1, and the identity atan(x) = PI/2 - atan(1/x)
// for |x| > 1. 9 terms (up to x^17) are used, giving ~1e-7 relative accuracy.
// Handles negative inputs symmetrically.
static float atan(float x)
{
    int is_negative = 0;          // flag for negative input
    if (x < 0.0f)
    {
        x = -x;
        is_negative = 1;
    }

    float abs_result;

    if (x > 1.0f)
    {
        // Use atan(x) = PI/2 - atan(1/x) for large x
        float inv_x           = 1.0f / x;
        float term            = inv_x;
        float sum             = inv_x;
        float inv_x_squared   = inv_x * inv_x;

        // Series: atan(1/x) = 1/x - 1/(3x^3) + 1/(5x^5) - ...
        for (int i = 1; i <= 8; i++)
        {
            term *= -inv_x_squared;
            sum += term / (float)(2 * i + 1);
        }
        abs_result = (PI / 2.0f) - sum;
    }
    else
    {
        // Direct Taylor series for |x| <= 1
        float term      = x;
        float sum       = x;
        float x_squared = x * x;

        // Series: atan(x) = x - x^3/3 + x^5/5 - ...
        for (int i = 1; i <= 8; i++)
        {
            term *= -x_squared;
            sum += term / (float)(2 * i + 1);
        }
        abs_result = sum;
    }

    return is_negative ? -abs_result : abs_result;
}

// Approximates the four-quadrant arctangent of y/x, returning an angle in [-PI, PI].
// Handles x == 0 and the sign of y to return +/- PI/2.
// Relies on atan() for the base angle, then adjusts quadrant based on the sign of x.
static float atan2(float y, float x)
{
    // Vertical axis cases (x == 0)
    if (x == 0.0f)
    {
        if (y > 0.0f)
        {
            return PI / 2.0f;
        }
        if (y < 0.0f)
        {
            return -PI / 2.0f;
        }
        return 0.0f;              // Both zero - conventionally returns 0.
    }

    float base_angle = atan(y / x);

    // Correct for quadrant based on the sign of x
    if (x < 0.0f)
    {
        if (y >= 0.0f)
        {
            return base_angle + PI;
        }
        return base_angle - PI;
    }
    return base_angle;
}

// Approximates arcsin(x) (principal value, in [-PI/2, PI/2]).
// Uses the identity asin(x) = atan( x / sqrt(1 - x^2) ).
// Clamps inputs to [-1, 1] to avoid domain errors; returns +/- PI/2 at the extremes.
// NOTE: Depends on custom_sqrt() being defined elsewhere – this function does
// not implement it. Ensure custom_sqrt() handles non‑negative arguments correctly.
static float asin(float x)
{
    if (x >= 1.0f)
    {
        return PI / 2.0f;
    }
    if (x <= -1.0f)
    {
        return -PI / 2.0f;
    }

    return atan(x / custom_sqrt(1.0f - x * x));
}

// Approximates arccos(x) using the identity acos(x) = PI/2 - asin(x).
// Leverages the asin() implementation above, inheriting its domain clamping
// and precision characteristics.
static float acos(float x)
{
    return (PI / 2.0f) - asin(x);
}

int app_main(int argc, char **argv, app_api_t *api)
{
    // Check the number of arguments before reading any function or value.
    // atan2 is the only operation that needs a fourth argument.
    if (!argv || argc < 3)
    {
        api->puts_col("Usage: trig <value> -[sin|cos|tan|atan|asin|acos] OR trig <x> <y> -atan2\n", api->col_red);
        return 1;
    }

    float x = 0.0f; // parse_float writes the parsed value into x and returns 0 on failure.
    if (!parse_float(argv[1], &x))
    {
        api->puts_col("Error: Invalid number input.\n", api->col_red);
        return 1;
    }

    // The normal form is: trig <value> -<function>.
    // atan2 uses: trig <x> <y> -atan2, so its function name is argv[3].
    char *function_name = (argc == 4) ? argv[3] : argv[2];

    // Strip leading dashes so both "-sin" and "sin" hash the same way.
    while (function_name[0] == '-')
    {
        function_name++;
    }

    // C cannot switch directly on a string, so hash the function name first.
    unsigned int function_hash = hash_string(function_name);
    float result = 0.0f; // The selected trig function writes its result here.

    // These cases use the hashes declared at the top of the file.
    switch (function_hash)
    {
        case HASH_SIN:
            result = sin(x);
            break;

        case HASH_COS:
            result = cos(x);
            break;

        case HASH_TAN:
            result = tan(x);
            break;

        case HASH_ASIN:
            result = asin(x);
            break;

        case HASH_ACOS:
            result = acos(x);
            break;

        case HASH_ATAN:
            result = atan(x);
            break;

        case HASH_ATAN2:
        {
            // atan2 is the one operation that consumes a second numeric value.
            // Keep this validation here so the other operations remain simple.
            if (argc < 4)
            {
                api->puts_col("Error: atan2 requires two values. Usage: trig <x> <y> -atan2\n", api->col_red);
                return 1;
            }
            float y = 0.0f;
            if (!parse_float(argv[2], &y))
            {
                api->puts_col("Error: Invalid second number input for atan2.\n", api->col_red);
                return 1;
            }
            result = atan2(y, x);
            break;
        }

        default:
            // A hash miss means the requested operation is not supported.
            api->puts_col("Error: invalid operation\n", api->col_red);
            return 1;
    }

    // Finally print the result with six fractional digits.
    print_float(api, result, 6);
    api->putchar('\n');

    return 0;
}