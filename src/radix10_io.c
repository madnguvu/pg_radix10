/*
 * radix10_io.c — String ↔ Radix10Numeric conversion and NUMERIC interop
 *
 * Two public conversions, each one path:
 *   r10_from_cstring: parse → mantissa digits + exponent → limbs
 *   r10_to_cstring:   limbs → full digit string → insert decimal point
 *
 * And two NUMERIC bridges (via text, to avoid coupling to NUMERIC internals):
 *   r10_from_numeric, r10_to_numeric
 *
 * Copyright (c) 2026, pg_radix10 Contributors
 * PostgreSQL License (see LICENSE)
 */

#include "radix10.h"
#include <ctype.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

/*
 * Raise a parse error.  Factored out so the parser reads as logic, not error
 * handling.
 */
static void
r10_parse_error(const char *str)
{
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
             errmsg("invalid input syntax for type radix10_numeric: \"%s\"",
                    str)));
}

/*
 * Pack a NUL-padded decimal digit string into base-10^9 limbs.
 *
 * `digits` has exactly `total_len` characters (only '0'-'9'), padded to a
 * multiple of R10_LIMB_DIGITS.  We slice it left-to-right into 9-character
 * chunks and parse each as a uint32.
 */
static void
r10_digits_to_limbs(const char *digits, int total_len, uint32 *limbs)
{
    int     i;
    char    chunk[R10_LIMB_DIGITS + 1];

    Assert(total_len % R10_LIMB_DIGITS == 0);

    for (i = 0; i < total_len; i += R10_LIMB_DIGITS)
    {
        memcpy(chunk, digits + i, R10_LIMB_DIGITS);
        chunk[R10_LIMB_DIGITS] = '\0';
        *limbs++ = (uint32) strtoul(chunk, NULL, 10);
    }
}

/* ================================================================
 * String → Radix10Numeric
 *
 * Algorithm:
 *   1. Parse sign, mantissa digits (skipping '.'), and optional exponent.
 *   2. Compute the logical decimal-point position within the mantissa
 *      (int_digits = digits-before-dot + exponent, clamped to [0, total]).
 *   3. Build one flat digit string: left-pad the integer part to a multiple
 *      of 9, right-pad the fractional part to a multiple of 9.
 *   4. Slice into 9-digit limbs.  weight = int_limbs − 1.
 *
 * One path.  No branches for positive/negative exponents.  No intermediate
 * buffers rebuilt or replaced.
 * ================================================================ */

Radix10Numeric
r10_from_cstring(const char *str)
{
    const char *cp = str;
    const char *mantissa_start;
    bool        negative = false;
    int         digits_before_dp = 0;
    int         digits_after_dp = 0;
    int         mantissa_len;
    int         exponent = 0;
    int         int_digits, frac_digits;
    int         int_limbs, frac_limbs;
    int         padded_int, padded_frac, padded_total;
    int         dscale;
    char       *mantissa;
    char       *padded;
    int         d, i;
    Radix10Numeric result;

    /* --- Leading whitespace --- */
    while (isspace((unsigned char) *cp))
        cp++;

    /* --- Special values --- */
    if (pg_strncasecmp(cp, "NaN", 3) == 0)
        return r10_alloc(0, 0, R10_NAN, 0);
    if (pg_strncasecmp(cp, "-Infinity", 9) == 0 ||
        pg_strncasecmp(cp, "-Inf", 4) == 0)
        return r10_alloc(0, 0, R10_NINF, 0);
    if (pg_strncasecmp(cp, "Infinity", 8) == 0 ||
        pg_strncasecmp(cp, "+Infinity", 9) == 0 ||
        pg_strncasecmp(cp, "Inf", 3) == 0 ||
        pg_strncasecmp(cp, "+Inf", 4) == 0)
        return r10_alloc(0, 0, R10_PINF, 0);

    /* --- Sign --- */
    if (*cp == '-')      { negative = true; cp++; }
    else if (*cp == '+') { cp++; }

    while (isspace((unsigned char) *cp))
        cp++;

    /* --- Count mantissa digits (two spans separated by optional '.') --- */
    mantissa_start = cp;

    while (isdigit((unsigned char) *cp)) { digits_before_dp++; cp++; }
    if (*cp == '.')
    {
        cp++;
        while (isdigit((unsigned char) *cp)) { digits_after_dp++; cp++; }
    }
    mantissa_len = digits_before_dp + digits_after_dp;

    if (mantissa_len == 0)
        r10_parse_error(str);

    /* --- Optional exponent --- */
    if (*cp == 'e' || *cp == 'E')
    {
        bool exp_neg = false;
        cp++;
        if (*cp == '-')      { exp_neg = true; cp++; }
        else if (*cp == '+') { cp++; }
        if (!isdigit((unsigned char) *cp))
            r10_parse_error(str);
        while (isdigit((unsigned char) *cp))
        {
            exponent = exponent * 10 + (*cp - '0');
            cp++;
        }
        if (exp_neg)
            exponent = -exponent;
    }

    /* --- Trailing whitespace, then must be end-of-string --- */
    while (isspace((unsigned char) *cp))
        cp++;
    if (*cp != '\0')
        r10_parse_error(str);

    /* --- Collect mantissa digits into a contiguous buffer --- */
    mantissa = (char *) palloc(mantissa_len + 1);
    cp = mantissa_start;
    d = 0;
    while (isdigit((unsigned char) *cp))
        mantissa[d++] = *cp++;
    if (*cp == '.')
        cp++;
    while (isdigit((unsigned char) *cp))
        mantissa[d++] = *cp++;
    mantissa[d] = '\0';

    /*
     * Logical position of the decimal point within the mantissa.
     *
     * The mantissa has `digits_before_dp` digits to the left of the original
     * dot.  The exponent shifts that boundary rightward (positive) or
     * leftward (negative).  Clamping to [0, mantissa_len] is correct:
     * positions outside that range become virtual zeros.
     */
    int_digits  = digits_before_dp + exponent;
    frac_digits = mantissa_len - digits_before_dp - exponent;

    /* Clamp: negative counts become zero (the slack is virtual zeros) */
    if (int_digits < 0)  int_digits = 0;
    if (frac_digits < 0) frac_digits = 0;

    dscale = frac_digits;

    /* If the entire logical value is zero length, treat as zero */
    if (int_digits + frac_digits == 0)
    {
        pfree(mantissa);
        return r10_alloc(0, 0, R10_POS, 0);
    }

    /*
     * Build the padded digit string.
     *
     * Integer part: right-aligned in ceil(int_digits / 9) × 9 characters.
     * Fractional part: left-aligned in ceil(frac_digits / 9) × 9 characters.
     * The mantissa digits land at a known offset; everything else is '0'.
     */
    int_limbs  = int_digits  > 0 ? (int_digits  + R10_LIMB_DIGITS - 1) / R10_LIMB_DIGITS : 0;
    frac_limbs = frac_digits > 0 ? (frac_digits + R10_LIMB_DIGITS - 1) / R10_LIMB_DIGITS : 0;
    padded_int  = int_limbs  * R10_LIMB_DIGITS;
    padded_frac = frac_limbs * R10_LIMB_DIGITS;
    padded_total = padded_int + padded_frac;

    if (int_limbs + frac_limbs == 0)
    {
        pfree(mantissa);
        return r10_alloc(0, 0, R10_POS, dscale);
    }

    /* Start with all '0', then overlay the mantissa digits */
    padded = (char *) palloc(padded_total + 1);
    memset(padded, '0', padded_total);
    padded[padded_total] = '\0';

    /*
     * Where do the mantissa digits go?
     *
     * The first mantissa digit corresponds to logical integer position
     * (int_digits - digits_before_dp), which in the padded buffer is at
     * offset (padded_int - digits_before_dp).  But if the exponent pushed
     * the decimal point beyond the mantissa, some mantissa digits may map
     * to the fractional zone, or vice versa.  The simplest correct
     * expression: the mantissa starts at padded_int - int_digits, offset
     * by however many leading virtual zeros exist before the first real
     * mantissa digit.
     *
     * Because int_digits = digits_before_dp + exponent (clamped ≥ 0), the
     * first mantissa digit lands at:
     *   padded_int - min(int_digits, digits_before_dp)
     * But that's just padded_int - digits_before_dp when exponent ≥ 0,
     * or padded_int when int_digits = 0.  Unified:
     */
    {
        int mantissa_offset;    /* position in padded[] of first mantissa digit */
        int copy_len;           /* how many mantissa digits to copy */

        if (int_digits >= digits_before_dp)
        {
            /* Normal or positive exponent: mantissa starts in the integer zone */
            mantissa_offset = padded_int - digits_before_dp;
        }
        else
        {
            /* Negative exponent ate into the integer part: some leading mantissa
             * digits have been pushed into the fractional zone.  The first
             * mantissa digit that still represents an integer digit is at index
             * (digits_before_dp - int_digits) within the mantissa, and it maps
             * to padded[0]. But we want to place the *entire* mantissa
             * contiguously and let the padding absorb the shift.
             *
             * First mantissa digit goes at padded_int - int_digits (which equals
             * padded_int when int_digits = 0, i.e. the fractional zone start,
             * minus however many leading frac zeros are needed).
             */
            mantissa_offset = padded_int - int_digits;
        }

        /* Don't overrun the padded buffer */
        copy_len = mantissa_len;
        if (mantissa_offset + copy_len > padded_total)
            copy_len = padded_total - mantissa_offset;
        if (mantissa_offset < 0)
        {
            /* Exponent so large that leading mantissa digits are off the left
             * edge — impossible because we clamped int_digits ≥ 0, but guard. */
            copy_len += mantissa_offset;  /* reduce */
            mantissa_offset = 0;
        }
        if (copy_len > 0)
            memcpy(padded + mantissa_offset, mantissa, copy_len);
    }

    pfree(mantissa);

    /* --- Check for all-zero --- */
    for (i = 0; i < padded_total; i++)
    {
        if (padded[i] != '0')
            break;
    }
    if (i == padded_total)
    {
        pfree(padded);
        return r10_alloc(0, 0, R10_POS, dscale);
    }

    /* --- Slice into limbs --- */
    result = r10_alloc(int_limbs + frac_limbs,
                       int_limbs > 0 ? int_limbs - 1 : -1,
                       negative ? R10_NEG : R10_POS,
                       dscale);
    r10_digits_to_limbs(padded, padded_total, result->digits);
    pfree(padded);

    r10_strip(&result);
    return result;
}

/* ================================================================
 * Radix10Numeric → String
 *
 * Algorithm:
 *   1. Expand all limbs into a flat digit string (9 chars each).
 *   2. Compute dp_pos = (weight + 1) × 9.
 *   3. Insert the decimal point, strip leading integer zeros,
 *      pad fractional zeros to dscale.
 *
 * One path.  The decimal point either falls before, within, or after the
 * digit string — but the logic is the same: emit integer part, emit dot,
 * emit fractional part.
 * ================================================================ */

char *
r10_to_cstring(Radix10Numeric num)
{
    StringInfoData  str;
    char           *digits;
    int             total_chars, dp_pos;
    int             int_start, int_end, frac_start, frac_end;
    int             i;

    initStringInfo(&str);

    /* Special values */
    if (R10_IS_NAN(num))  { appendStringInfoString(&str, "NaN");        return str.data; }
    if (R10_IS_PINF(num)) { appendStringInfoString(&str, "Infinity");   return str.data; }
    if (R10_IS_NINF(num)) { appendStringInfoString(&str, "-Infinity");  return str.data; }

    /* Sign */
    if (num->sign == R10_NEG)
        appendStringInfoChar(&str, '-');

    /* Zero (may still have dscale) */
    if (num->ndigits == 0)
    {
        appendStringInfoChar(&str, '0');
        if (num->dscale > 0)
        {
            appendStringInfoChar(&str, '.');
            for (i = 0; i < num->dscale; i++)
                appendStringInfoChar(&str, '0');
        }
        return str.data;
    }

    /* Expand limbs to a flat digit string */
    total_chars = num->ndigits * R10_LIMB_DIGITS;
    digits = (char *) palloc(total_chars + 1);
    for (i = 0; i < num->ndigits; i++)
    {
        char buf[R10_LIMB_DIGITS + 1];
        snprintf(buf, sizeof(buf), "%09u", num->digits[i]);
        memcpy(digits + i * R10_LIMB_DIGITS, buf, R10_LIMB_DIGITS);
    }
    digits[total_chars] = '\0';

    /*
     * dp_pos: how many characters belong to the integer part.
     * May be ≤ 0 (value < 1) or ≥ total_chars (pure integer).
     */
    dp_pos = (num->weight + 1) * R10_LIMB_DIGITS;

    /*
     * Integer part: digits[0..dp_pos-1], but:
     *   - if dp_pos ≤ 0, integer part is just "0"
     *   - strip leading '0's (but keep at least one digit)
     *   - if dp_pos > total_chars, append trailing zeros
     */
    if (dp_pos <= 0)
    {
        appendStringInfoChar(&str, '0');
    }
    else
    {
        int_start = 0;
        int_end   = (dp_pos < total_chars) ? dp_pos : total_chars;

        /* Strip leading zeros, keep at least one */
        while (int_start < int_end - 1 && digits[int_start] == '0')
            int_start++;

        appendBinaryStringInfo(&str, digits + int_start, int_end - int_start);

        /* Trailing zeros if dp_pos extends beyond the digit string */
        for (i = total_chars; i < dp_pos; i++)
            appendStringInfoChar(&str, '0');
    }

    /*
     * Fractional part (only if dscale > 0).
     *   - if dp_pos < 0, we need leading zeros before the digit string
     *   - emit digits from the fractional zone of the digit string
     *   - pad with trailing zeros to reach dscale
     */
    if (num->dscale > 0)
    {
        int leading_zeros;      /* zeros before any real digit */
        int frac_from_digits;   /* digits available in the fractional zone */
        int emit;

        appendStringInfoChar(&str, '.');

        leading_zeros = (dp_pos < 0) ? -dp_pos : 0;
        if (leading_zeros > num->dscale)
            leading_zeros = num->dscale;
        for (i = 0; i < leading_zeros; i++)
            appendStringInfoChar(&str, '0');

        /* Fractional digits from the string */
        frac_start = (dp_pos > 0) ? dp_pos : 0;
        frac_from_digits = total_chars - frac_start;
        if (frac_from_digits < 0)
            frac_from_digits = 0;

        emit = num->dscale - leading_zeros;
        if (emit > frac_from_digits)
            emit = frac_from_digits;
        if (emit > 0)
            appendBinaryStringInfo(&str, digits + frac_start, emit);

        /* Trailing zeros to fill dscale */
        for (i = leading_zeros + emit; i < num->dscale; i++)
            appendStringInfoChar(&str, '0');
    }

    pfree(digits);
    return str.data;
}

/* ================================================================
 * NUMERIC ↔ Radix10Numeric
 *
 * Via text representation.  Safe, correct, zero coupling to NUMERIC
 * internals.  A direct limb-to-limb path is a v0.2 optimization.
 * ================================================================ */

Radix10Numeric
r10_from_numeric(Numeric num)
{
    Datum   d;
    char   *s;

    if (NUMERIC_IS_NAN(num))
        return r10_alloc(0, 0, R10_NAN, 0);

    d = DirectFunctionCall1(numeric_out, NumericGetDatum(num));
    s = DatumGetCString(d);
    return r10_from_cstring(s);
}

Numeric
r10_to_numeric(Radix10Numeric rnum)
{
    char   *s = r10_to_cstring(rnum);
    Datum   d = DirectFunctionCall3(numeric_in,
                                    CStringGetDatum(s),
                                    ObjectIdGetDatum(InvalidOid),
                                    Int32GetDatum(-1));
    pfree(s);
    return DatumGetNumeric(d);
}
