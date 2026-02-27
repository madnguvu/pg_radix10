/*
 * radix10_ops.c — Arithmetic and comparison operators for radix10_numeric
 *
 * Native:    add, subtract, multiply, abs, negate, sign, min, max, cmp.
 * Delegated: div, mod, round, trunc, power, sqrt, floor, ceil
 *            (convert to NUMERIC, compute, convert back — v0.2 goes native).
 *
 * The key property of base-10^9: uint32 × uint32 fits in uint64 without
 * overflow, so every carry propagation is a single 64-bit divide.
 *
 * Copyright (c) 2026, pg_radix10 Contributors
 * PostgreSQL License (see LICENSE)
 */

#include "radix10.h"
#include <string.h>

/* Forward declarations of internal helpers */
static Radix10Numeric r10_add_abs(Radix10Numeric a, Radix10Numeric b);
static Radix10Numeric r10_sub_abs(Radix10Numeric a, Radix10Numeric b);
static int  r10_cmp_abs(Radix10Numeric a, Radix10Numeric b);
static void r10_align(Radix10Numeric a, Radix10Numeric b,
                      uint32 **a_digits, int *a_ndigits,
                      uint32 **b_digits, int *b_ndigits,
                      int *aligned_weight, int *out_rscale);

/* ================================================================
 * NUMERIC delegation helper
 *
 * Eight operations share the same pattern: convert both operands (or one)
 * to core NUMERIC, call a PG built-in, convert the result back.  Rather
 * than writing the pattern eight times, we write it once.
 * ================================================================ */

/*
 * Delegate a two-argument numeric function.
 * func must accept (Numeric, Numeric) and return Numeric.
 */
static Radix10Numeric
r10_delegate2(Radix10Numeric a, Radix10Numeric b, PGFunction func)
{
    Numeric na = r10_to_numeric(a);
    Numeric nb = r10_to_numeric(b);
    Numeric nr = DatumGetNumeric(DirectFunctionCall2(func,
                                                     NumericGetDatum(na),
                                                     NumericGetDatum(nb)));
    return r10_from_numeric(nr);
}

/*
 * Delegate a one-argument numeric function.
 * func must accept (Numeric) and return Numeric.
 */
static Radix10Numeric
r10_delegate1(Radix10Numeric a, PGFunction func)
{
    Numeric na = r10_to_numeric(a);
    Numeric nr = DatumGetNumeric(DirectFunctionCall1(func,
                                                     NumericGetDatum(na)));
    return r10_from_numeric(nr);
}

/*
 * Delegate a (Numeric, int32) function (round, trunc).
 */
static Radix10Numeric
r10_delegate_scale(Radix10Numeric a, int scale, PGFunction func)
{
    Numeric na = r10_to_numeric(a);
    Numeric nr = DatumGetNumeric(DirectFunctionCall2(func,
                                                     NumericGetDatum(na),
                                                     Int32GetDatum(scale)));
    return r10_from_numeric(nr);
}

/* ================================================================
 * Comparison
 * ================================================================ */

int
r10_cmp(Radix10Numeric a, Radix10Numeric b)
{
    /* NaN compares equal to NaN, greater than everything else (PG convention) */
    if (R10_IS_NAN(a) || R10_IS_NAN(b))
        return 0;

    if (R10_IS_PINF(a))  return R10_IS_PINF(b) ? 0 :  1;
    if (R10_IS_NINF(a))  return R10_IS_NINF(b) ? 0 : -1;
    if (R10_IS_PINF(b))  return -1;
    if (R10_IS_NINF(b))  return  1;

    /* Opposite signs */
    if (a->sign == R10_POS && b->sign == R10_NEG)  return  1;
    if (a->sign == R10_NEG && b->sign == R10_POS)  return -1;

    /* Same sign: compare magnitudes (flip if negative) */
    {
        int cmp = r10_cmp_abs(a, b);
        return (a->sign == R10_NEG) ? -cmp : cmp;
    }
}

static int
r10_cmp_abs(Radix10Numeric a, Radix10Numeric b)
{
    int common, i;

    if (a->weight != b->weight)
        return (a->weight > b->weight) ? 1 : -1;

    common = (a->ndigits < b->ndigits) ? a->ndigits : b->ndigits;

    for (i = 0; i < common; i++)
    {
        if (a->digits[i] != b->digits[i])
            return (a->digits[i] > b->digits[i]) ? 1 : -1;
    }

    /* Longer number wins if its trailing limbs are non-zero */
    for (i = common; i < a->ndigits; i++)
        if (a->digits[i] != 0) return 1;
    for (i = common; i < b->ndigits; i++)
        if (b->digits[i] != 0) return -1;

    return 0;
}

/* ================================================================
 * Alignment: extend two operands to a common limb grid
 * ================================================================ */

static void
r10_align(Radix10Numeric a, Radix10Numeric b,
          uint32 **a_out, int *a_nd,
          uint32 **b_out, int *b_nd,
          int *aligned_weight, int *out_rscale)
{
    int     a_hi = a->weight,              b_hi = b->weight;
    int     a_lo = a->weight - a->ndigits + 1,  b_lo = b->weight - b->ndigits + 1;
    int     hi   = (a_hi > b_hi) ? a_hi : b_hi;
    int     lo   = (a_lo < b_lo) ? a_lo : b_lo;
    int     span = hi - lo + 1;
    uint32 *ad, *bd;
    int     i;

    ad = (uint32 *) palloc0(span * sizeof(uint32));
    bd = (uint32 *) palloc0(span * sizeof(uint32));

    for (i = 0; i < a->ndigits; i++)
        ad[hi - a->weight + i] = a->digits[i];
    for (i = 0; i < b->ndigits; i++)
        bd[hi - b->weight + i] = b->digits[i];

    *a_out = ad;
    *b_out = bd;
    *a_nd  = span;
    *b_nd  = span;
    *aligned_weight = hi;
    *out_rscale = (a->dscale > b->dscale) ? a->dscale : b->dscale;
}

/* ================================================================
 * Core arithmetic: add/sub absolute values
 * ================================================================ */

static Radix10Numeric
r10_add_abs(Radix10Numeric a, Radix10Numeric b)
{
    uint32         *ad, *bd;
    int             and_, bnd, res_weight, rscale;
    int             res_ndigits, i;
    uint64          carry = 0;
    Radix10Numeric  result;

    r10_align(a, b, &ad, &and_, &bd, &bnd, &res_weight, &rscale);

    res_ndigits = and_ + 1;  /* one extra for possible carry */
    result = r10_alloc(res_ndigits, res_weight + 1, R10_POS, rscale);

    for (i = and_ - 1; i >= 0; i--)
    {
        uint64 sum = (uint64) ad[i] + (uint64) bd[i] + carry;
        result->digits[i + 1] = (uint32)(sum % R10_NBASE);
        carry = sum / R10_NBASE;
    }
    result->digits[0] = (uint32) carry;

    pfree(ad);
    pfree(bd);

    r10_strip(&result);
    return result;
}

static Radix10Numeric
r10_sub_abs(Radix10Numeric a, Radix10Numeric b)
{
    uint32         *ad, *bd;
    int             and_, bnd, res_weight, rscale;
    int             i;
    int64           borrow = 0;
    Radix10Numeric  result;

    r10_align(a, b, &ad, &and_, &bd, &bnd, &res_weight, &rscale);

    result = r10_alloc(and_, res_weight, R10_POS, rscale);

    for (i = and_ - 1; i >= 0; i--)
    {
        int64 diff = (int64) ad[i] - (int64) bd[i] - borrow;
        if (diff < 0)
        {
            diff += R10_NBASE;
            borrow = 1;
        }
        else
            borrow = 0;

        result->digits[i] = (uint32) diff;
    }

    pfree(ad);
    pfree(bd);

    r10_strip(&result);
    return result;
}

/* ================================================================
 * Public arithmetic
 * ================================================================ */

Radix10Numeric
r10_add(Radix10Numeric a, Radix10Numeric b)
{
    Radix10Numeric result;

    /* Special values */
    if (R10_IS_SPECIAL(a) || R10_IS_SPECIAL(b))
    {
        if (R10_IS_NAN(a) || R10_IS_NAN(b))
            return r10_alloc(0, 0, R10_NAN, 0);
        if ((R10_IS_PINF(a) && R10_IS_NINF(b)) ||
            (R10_IS_NINF(a) && R10_IS_PINF(b)))
            return r10_alloc(0, 0, R10_NAN, 0);
        if (R10_IS_INF(a)) return r10_copy(a);
        return r10_copy(b);
    }

    /* Same sign: add magnitudes, keep sign */
    if (a->sign == b->sign)
    {
        result = r10_add_abs(a, b);
        result->sign = a->sign;
        return result;
    }

    /* Different signs: subtract the smaller magnitude from the larger */
    {
        int cmp = r10_cmp_abs(a, b);
        if (cmp > 0)
        {
            result = r10_sub_abs(a, b);
            result->sign = a->sign;
        }
        else if (cmp < 0)
        {
            result = r10_sub_abs(b, a);
            result->sign = b->sign;
        }
        else
        {
            /* Equal magnitudes, different signs → zero */
            result = r10_alloc(0, 0, R10_POS,
                               (a->dscale > b->dscale) ? a->dscale : b->dscale);
        }
    }

    return result;
}

/*
 * r10_sub — a − b.
 *
 * Flip b's sign conceptually and add.  Rather than allocating a copy just
 * to flip one field, we temporarily mutate b, add, then restore.  This is
 * safe because r10_add only reads b.
 */
Radix10Numeric
r10_sub(Radix10Numeric a, Radix10Numeric b)
{
    Radix10Numeric result;
    uint16 orig_sign = b->sign;

    /* Flip sign */
    switch (b->sign)
    {
        case R10_POS:  b->sign = R10_NEG;  break;
        case R10_NEG:  b->sign = R10_POS;  break;
        case R10_PINF: b->sign = R10_NINF; break;
        case R10_NINF: b->sign = R10_PINF; break;
        default:       break;  /* NaN stays NaN */
    }

    result = r10_add(a, b);

    /* Restore — never leave the caller's data modified */
    b->sign = orig_sign;

    return result;
}

/*
 * r10_mul — schoolbook multiplication in base-10^9.
 *
 * Each limb×limb product fits in uint64 (max: (10^9 − 1)^2 ≈ 10^18 < 2^63).
 * We accumulate into a uint64 array, then propagate carries in one pass.
 */
Radix10Numeric
r10_mul(Radix10Numeric a, Radix10Numeric b)
{
    int             res_ndigits, res_weight, res_dscale;
    uint16          res_sign;
    uint64         *accum;
    Radix10Numeric  result;
    int             i, j;

    /* Special values */
    if (R10_IS_SPECIAL(a) || R10_IS_SPECIAL(b))
    {
        if (R10_IS_NAN(a) || R10_IS_NAN(b))
            return r10_alloc(0, 0, R10_NAN, 0);
        if ((R10_IS_INF(a) && b->ndigits == 0) ||
            (R10_IS_INF(b) && a->ndigits == 0))
            return r10_alloc(0, 0, R10_NAN, 0);  /* Inf × 0 */
        res_sign = (a->sign == b->sign ||
                    (R10_IS_PINF(a) && b->sign == R10_POS) ||
                    (R10_IS_PINF(b) && a->sign == R10_POS) ||
                    (R10_IS_NINF(a) && b->sign == R10_NEG) ||
                    (R10_IS_NINF(b) && a->sign == R10_NEG))
                   ? R10_PINF : R10_NINF;
        return r10_alloc(0, 0, res_sign, 0);
    }

    /* Either operand is zero → zero */
    res_dscale = a->dscale + b->dscale;
    if (a->ndigits == 0 || b->ndigits == 0)
        return r10_alloc(0, 0, R10_POS, res_dscale);

    res_ndigits = a->ndigits + b->ndigits;
    res_weight  = a->weight + b->weight + 1;
    res_sign    = (a->sign == b->sign) ? R10_POS : R10_NEG;

    /* Accumulate partial products */
    accum = (uint64 *) palloc0(res_ndigits * sizeof(uint64));
    for (i = 0; i < a->ndigits; i++)
    {
        uint64 ai = a->digits[i];
        if (ai == 0) continue;
        for (j = 0; j < b->ndigits; j++)
            accum[i + j + 1] += ai * (uint64) b->digits[j];
    }

    /* Single carry-propagation pass */
    result = r10_alloc(res_ndigits, res_weight, res_sign, res_dscale);
    {
        uint64 carry = 0;
        for (i = res_ndigits - 1; i >= 0; i--)
        {
            uint64 sum = accum[i] + carry;
            result->digits[i] = (uint32)(sum % R10_NBASE);
            carry = sum / R10_NBASE;
        }
        Assert(carry == 0);
    }

    pfree(accum);
    r10_strip(&result);
    return result;
}

/* ================================================================
 * Delegated operations (v0.1 — native in v0.2)
 *
 * Each function validates special values and edge cases in our type
 * system, then delegates the core computation to PostgreSQL's NUMERIC.
 * ================================================================ */

Radix10Numeric
r10_div(Radix10Numeric a, Radix10Numeric b, int rscale)
{
    Radix10Numeric result;

    if (R10_IS_SPECIAL(a) || R10_IS_SPECIAL(b))
    {
        if (R10_IS_NAN(a) || R10_IS_NAN(b))
            return r10_alloc(0, 0, R10_NAN, 0);
        if (R10_IS_INF(a) && R10_IS_INF(b))
            return r10_alloc(0, 0, R10_NAN, 0);
        if (R10_IS_INF(a))
            return r10_copy(a);
        return r10_alloc(0, 0, R10_POS, rscale);
    }
    if (b->ndigits == 0)
        ereport(ERROR,
                (errcode(ERRCODE_DIVISION_BY_ZERO),
                 errmsg("division by zero")));

    result = r10_delegate2(a, b, numeric_div);
    if (rscale >= 0)
        result = r10_round(result, rscale);
    return result;
}

Radix10Numeric
r10_mod(Radix10Numeric a, Radix10Numeric b)
{
    if (R10_IS_SPECIAL(a) || R10_IS_SPECIAL(b))
        return r10_alloc(0, 0, R10_NAN, 0);
    if (b->ndigits == 0)
        ereport(ERROR,
                (errcode(ERRCODE_DIVISION_BY_ZERO),
                 errmsg("division by zero")));

    return r10_delegate2(a, b, numeric_mod);
}

Radix10Numeric
r10_round(Radix10Numeric a, int scale)
{
    if (R10_IS_SPECIAL(a))
        return r10_copy(a);
    return r10_delegate_scale(a, scale, numeric_round);
}

Radix10Numeric
r10_trunc(Radix10Numeric a, int scale)
{
    if (R10_IS_SPECIAL(a))
        return r10_copy(a);
    return r10_delegate_scale(a, scale, numeric_trunc);
}

Radix10Numeric
r10_power(Radix10Numeric base_val, Radix10Numeric exp_val)
{
    if (R10_IS_NAN(base_val) || R10_IS_NAN(exp_val))
        return r10_alloc(0, 0, R10_NAN, 0);
    return r10_delegate2(base_val, exp_val, numeric_power);
}

Radix10Numeric
r10_sqrt(Radix10Numeric a)
{
    if (R10_IS_SPECIAL(a))
    {
        if (R10_IS_NAN(a))  return r10_alloc(0, 0, R10_NAN, 0);
        if (R10_IS_PINF(a)) return r10_alloc(0, 0, R10_PINF, 0);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
                 errmsg("cannot take square root of a negative number")));
    }
    if (a->sign == R10_NEG && a->ndigits > 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
                 errmsg("cannot take square root of a negative number")));

    return r10_delegate1(a, numeric_sqrt);
}

Radix10Numeric
r10_floor(Radix10Numeric a)
{
    if (R10_IS_SPECIAL(a))
        return r10_copy(a);
    return r10_delegate1(a, numeric_floor);
}

Radix10Numeric
r10_ceil(Radix10Numeric a)
{
    if (R10_IS_SPECIAL(a))
        return r10_copy(a);
    return r10_delegate1(a, numeric_ceil);
}

/* ================================================================
 * Simple unary operations (all native)
 * ================================================================ */

Radix10Numeric
r10_abs(Radix10Numeric a)
{
    Radix10Numeric result = r10_copy(a);
    if (result->sign == R10_NEG)       result->sign = R10_POS;
    else if (R10_IS_NINF(result))      result->sign = R10_PINF;
    return result;
}

Radix10Numeric
r10_negate(Radix10Numeric a)
{
    Radix10Numeric result = r10_copy(a);
    switch (result->sign)
    {
        case R10_POS:  result->sign = R10_NEG;  break;
        case R10_NEG:  result->sign = R10_POS;  break;
        case R10_PINF: result->sign = R10_NINF; break;
        case R10_NINF: result->sign = R10_PINF; break;
        default: break;  /* NaN */
    }
    return result;
}

int
r10_sign(Radix10Numeric a)
{
    if (R10_IS_NAN(a))  return 0;
    if (R10_IS_PINF(a)) return 1;
    if (R10_IS_NINF(a)) return -1;
    if (a->ndigits == 0) return 0;
    return (a->sign == R10_NEG) ? -1 : 1;
}

Radix10Numeric
r10_min2(Radix10Numeric a, Radix10Numeric b)
{
    return r10_copy(r10_cmp(a, b) <= 0 ? a : b);
}

Radix10Numeric
r10_max2(Radix10Numeric a, Radix10Numeric b)
{
    return r10_copy(r10_cmp(a, b) >= 0 ? a : b);
}

/* ================================================================
 * SQL-callable operator wrappers
 *
 * Thin adapters between PG's function-call convention and our
 * internal API.  Each is two lines: unpack args, return result.
 * ================================================================ */

#define R10_UNARY_WRAPPER(sqlname, cfunc) \
    PG_FUNCTION_INFO_V1(sqlname); \
    Datum sqlname(PG_FUNCTION_ARGS) { \
        PG_RETURN_R10NUMERIC_P(cfunc(PG_GETARG_R10NUMERIC_P(0))); \
    }

#define R10_BINARY_WRAPPER(sqlname, cfunc) \
    PG_FUNCTION_INFO_V1(sqlname); \
    Datum sqlname(PG_FUNCTION_ARGS) { \
        PG_RETURN_R10NUMERIC_P(cfunc(PG_GETARG_R10NUMERIC_P(0), \
                                     PG_GETARG_R10NUMERIC_P(1))); \
    }

#define R10_CMP_WRAPPER(sqlname, op) \
    PG_FUNCTION_INFO_V1(sqlname); \
    Datum sqlname(PG_FUNCTION_ARGS) { \
        PG_RETURN_BOOL(r10_cmp(PG_GETARG_R10NUMERIC_P(0), \
                               PG_GETARG_R10NUMERIC_P(1)) op 0); \
    }

/* Arithmetic */
R10_BINARY_WRAPPER(radix10_numeric_add, r10_add)
R10_BINARY_WRAPPER(radix10_numeric_sub, r10_sub)
R10_BINARY_WRAPPER(radix10_numeric_mul, r10_mul)
R10_BINARY_WRAPPER(radix10_numeric_mod, r10_mod)
R10_BINARY_WRAPPER(radix10_numeric_power, r10_power)

/* Unary */
R10_UNARY_WRAPPER(radix10_numeric_abs, r10_abs)
R10_UNARY_WRAPPER(radix10_numeric_uminus, r10_negate)
R10_UNARY_WRAPPER(radix10_numeric_uplus, r10_copy)
R10_UNARY_WRAPPER(radix10_numeric_sqrt_sql, r10_sqrt)
R10_UNARY_WRAPPER(radix10_numeric_floor_sql, r10_floor)
R10_UNARY_WRAPPER(radix10_numeric_ceil_sql, r10_ceil)

/* Division (needs rscale computation) */
PG_FUNCTION_INFO_V1(radix10_numeric_div);
Datum radix10_numeric_div(PG_FUNCTION_ARGS)
{
    Radix10Numeric a = PG_GETARG_R10NUMERIC_P(0);
    Radix10Numeric b = PG_GETARG_R10NUMERIC_P(1);
    int rscale = (a->dscale > b->dscale) ? a->dscale : b->dscale;
    if (rscale < 6) rscale = 6;
    PG_RETURN_R10NUMERIC_P(r10_div(a, b, rscale));
}

/* Round and trunc (need scale argument) */
PG_FUNCTION_INFO_V1(radix10_numeric_round_sql);
Datum radix10_numeric_round_sql(PG_FUNCTION_ARGS)
{
    PG_RETURN_R10NUMERIC_P(r10_round(PG_GETARG_R10NUMERIC_P(0),
                                     PG_GETARG_INT32(1)));
}

PG_FUNCTION_INFO_V1(radix10_numeric_trunc_sql);
Datum radix10_numeric_trunc_sql(PG_FUNCTION_ARGS)
{
    PG_RETURN_R10NUMERIC_P(r10_trunc(PG_GETARG_R10NUMERIC_P(0),
                                     PG_GETARG_INT32(1)));
}

/* Sign (returns int32, not radix10_numeric) */
PG_FUNCTION_INFO_V1(radix10_numeric_sign_sql);
Datum radix10_numeric_sign_sql(PG_FUNCTION_ARGS)
{
    PG_RETURN_INT32(r10_sign(PG_GETARG_R10NUMERIC_P(0)));
}

/* Comparisons */
R10_CMP_WRAPPER(radix10_numeric_eq, ==)
R10_CMP_WRAPPER(radix10_numeric_ne, !=)
R10_CMP_WRAPPER(radix10_numeric_lt, <)
R10_CMP_WRAPPER(radix10_numeric_le, <=)
R10_CMP_WRAPPER(radix10_numeric_gt, >)
R10_CMP_WRAPPER(radix10_numeric_ge, >=)

PG_FUNCTION_INFO_V1(radix10_numeric_cmp);
Datum radix10_numeric_cmp(PG_FUNCTION_ARGS)
{
    PG_RETURN_INT32(r10_cmp(PG_GETARG_R10NUMERIC_P(0),
                            PG_GETARG_R10NUMERIC_P(1)));
}
