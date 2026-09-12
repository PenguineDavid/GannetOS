/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/proc/pexe.h"

// Arithmetic evaluator                                               
//                                                                     
// Operates on `float` throughout, not `int` - this is what actually   
// fixes division: $((1/2)) used to truncate to 0 (correct C integer   
// division, but not what anyone typing that expects from a shell),    
// and $((10/3)) had no way to be anything but 3. Same plain-float,    
// no-libc technique apps/trig.c already uses successfully in this     
// freestanding environment - loader.c's enable_fpu_sse() sets up the  
// FPU/SSE control words before any app runs, so hardware float        
//  arithmetic here is safe, not something bolted on without support.   

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int is_space(char c)
{
    return c == ' ' || c == '\t';
}

typedef struct
{
    const char *cursor;
} parser_state_t;

static float expr(parser_state_t *state);

static void skip_whitespace(parser_state_t *state)
{
    while (is_space(*state->cursor))
    {
        state->cursor++;
    }
}

/*
 * primary: handles literals (now with an optional fractional part,
 * e.g. "3.14"), unary minus, and parenthesised subexpressions.
 *
 * Previously the '(' check was only at the top of primary(), so a unary
 * minus before a '(' (e.g. -(5+3)) would consume the '-', set is_negative=1,
 * then fall into the digit loop with '(' as the first character -- reading
 * zero digits and returning 0.
 *
 * Fix: after consuming a '-' and skipping whitespace, check again for '('
 * and recurse through expr() if found, then negate the result.
 */
static float primary(parser_state_t *state)
{
    skip_whitespace(state);

    /* Parenthesised subexpression (positive) */
    if (*state->cursor == '(')
    {
        state->cursor++;
        float value = expr(state);
        skip_whitespace(state);
        if (*state->cursor == ')')
        {
            state->cursor++;
        }
        return value;
    }

    /* Unary minus */
    int is_negative = 0;
    if (*state->cursor == '-')
    {
        is_negative = 1;
        state->cursor++;
        skip_whitespace(state);
        /* Unary minus applied to a parenthesised subexpression: -(expr) */
        if (*state->cursor == '(')
        {
            state->cursor++;
            float value = expr(state);
            skip_whitespace(state);
            if (*state->cursor == ')')
            {
                state->cursor++;
            }
            return -value;
        }
    }

    /* Numeric literal: an integer part, then an optional ".fraction"
       part. A bare "." with no leading digits (e.g. ".5") is not
       accepted - same restriction apps/trig.c's parse_float documents,
       kept here for consistency between the two. */
    float value = 0.0f;
    while (is_digit(*state->cursor))
    {
        value = value * 10.0f + (float)(*state->cursor++ - '0');
    }

    if (*state->cursor == '.')
    {
        state->cursor++;
        float weight = 0.1f;
        while (is_digit(*state->cursor))
        {
            value += (float)(*state->cursor++ - '0') * weight;
            weight *= 0.1f;
        }
    }

    return is_negative ? -value : value;
}

static float term(parser_state_t *state)
{
    float left_value = primary(state);
    for (;;)
    {
        skip_whitespace(state);
        char op = *state->cursor;
        if (op != '*' && op != '/' && op != '%')
        {
            break;
        }
        state->cursor++;
        float right_value = primary(state);
        if (op == '*')
        {
            left_value = left_value * right_value;
        }
        else if (op == '/')
        {
            left_value = (right_value != 0.0f) ? left_value / right_value : 0.0f;
        }
        else
        {
            /* '%' - C has no modulo operator for float operands, so
               this truncates both sides to int first, same semantics
               the original all-integer evaluator had. A fractional
               modulo (e.g. 5.5 % 2) is intentionally not supported -
               not something a shell arithmetic expansion typically
               needs, and adding it would mean pulling in fmodf-style
               logic for a case nobody's asked for. */
            left_value = (right_value != 0.0f) ? (float)((int)left_value % (int)right_value) : 0.0f;
        }
    }
    return left_value;
}

static float expr(parser_state_t *state)
{
    float left_value = term(state);
    for (;;)
    {
        skip_whitespace(state);
        char op = *state->cursor;
        if (op != '+' && op != '-')
        {
            break;
        }
        state->cursor++;
        float right_value = term(state);
        left_value = (op == '+') ? left_value + right_value : left_value - right_value;
    }
    return left_value;
}

static float eval(const char *expression_text)
{
    parser_state_t state = {expression_text};
    return expr(&state);
}

/* Formats val into out using up to ECHO_FRAC_DIGITS fractional digits,
 * then trims trailing zero digits (and the decimal point itself, if
 * every fractional digit came out zero) - so a whole-number result
 * like 4.0 prints as "4", not "4.000000", while 0.5 prints as "0.5"
 * and 10/3 prints as "3.333333". Same digit-by-digit extraction
 * apps/trig.c's print_float uses, adapted to build a string instead of
 * printing directly, and to trim instead of always showing a fixed
 * number of digits.
 *
 * Adds half a unit-in-the-last-place before splitting into integer/
 * fractional parts, which rounds rather than truncates at the final
 * displayed digit - without it, something like 0.1+0.2 (not exactly
 * representable in binary floating point) would print as
 * "0.299999..." instead of the "0.3" a person actually typed.
 *
 * Returns the number of characters written (excluding the terminating
 * NUL), matching the original int-only fmt()'s contract. Caller's
 * buffer must be at least 24 bytes: sign + up to 10 integer digits +
 * '.' + ECHO_FRAC_DIGITS fractional digits + NUL. */
#define ECHO_FRAC_DIGITS 6
static int fmt(float val, char *out)
{
    int out_index = 0;
    int is_negative = (val < 0.0f);
    if (is_negative)
    {
        val = -val;
    }

    float half_ulp = 0.5f;
    for (int d = 0; d < ECHO_FRAC_DIGITS; d++)
    {
        half_ulp *= 0.1f;
    }
    val += half_ulp;

    int int_part = (int)val;
    char int_buf[16];
    int int_digit_count = 0;
    int remaining = int_part;
    do
    {
        int_buf[int_digit_count++] = (char)('0' + (remaining % 10));
        remaining /= 10;
    } while (remaining > 0);

    if (is_negative)
    {
        out[out_index++] = '-';
    }
    for (int digit_index = int_digit_count - 1; digit_index >= 0; digit_index--)
    {
        out[out_index++] = int_buf[digit_index];
    }

    char frac_digits[ECHO_FRAC_DIGITS];
    float frac = val - (float)int_part;
    int last_nonzero = -1;
    for (int d = 0; d < ECHO_FRAC_DIGITS; d++)
    {
        frac *= 10.0f;
        int digit = (int)frac;
        if (digit > 9)
        {
            digit = 9; /* clamp against floating-point noise right at a digit boundary */
        }
        if (digit < 0)
        {
            digit = 0;
        }
        frac_digits[d] = (char)('0' + digit);
        if (digit != 0)
        {
            last_nonzero = d;
        }
        frac -= (float)digit;
    }

    if (last_nonzero >= 0)
    {
        out[out_index++] = '.';
        for (int d = 0; d <= last_nonzero; d++)
        {
            out[out_index++] = frac_digits[d];
        }
    }

    out[out_index] = '\0';
    return out_index;
}

/* ------------------------------------------------------------------ */
/* app_main                                                            */
/* ------------------------------------------------------------------ */
int app_main(int argc, char **argv, app_api_t *api)
{
    for (int i = 1; i < argc; i++)
    {
        if (i > 1)
        {
            api->putchar(' ');
        }
        const char *cursor = argv[i];
        while (*cursor)
        {
            if (cursor[0] == '$' && cursor[1] == '(' && cursor[2] == '(')
            {
                cursor += 3;
                const char *start = cursor;

                /* Scan for the true closing '))' by tracking nesting depth */
                int depth = 0;
                while (*cursor)
                {
                    if (cursor[0] == '(')
                    {
                        depth++;
                        cursor++;
                    }
                    else if (cursor[0] == ')')
                    {
                        if (depth > 0)
                        {
                            depth--;
                            cursor++;
                        }
                        else if (cursor[1] == ')')
                        {
                            // We are at the base depth and found the final '))'
                            break;
                        }
                        else
                        {
                            // Mismatched standalone ')' at base depth, treat as literal token character
                            cursor++;
                        }
                    }
                    else
                    {
                        cursor++;
                    }
                }

                if (cursor[0] == ')' && cursor[1] == ')')
                {
                    /* Well-formed expansion -- evaluate it. */
                    char expr_buf[128];
                    int expr_len = 0;
                    const char *expr_cursor = start;
                    while (expr_cursor < cursor && expr_len < 127)
                    {
                        expr_buf[expr_len++] = *expr_cursor++;
                    }
                    expr_buf[expr_len] = '\0';
                    cursor += 2; /* skip '))' */
                    char result_buf[24];
                    fmt(eval(expr_buf), result_buf);
                    api->puts(result_buf);
                }
                /* Unterminated: cursor now points at '\0', outer while exits. */
            }
            else
            {
                api->putchar(*cursor++);
            }
        }
    }
    api->putchar('\n');
    return 0;
}