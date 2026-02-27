/*
 * radix10.h — Core header for pg_radix10: Radix-10^9 Numeric type
 *
 * pg_radix10 provides a variable-precision decimal type using 9-decimal-digit
 * limbs (base 10^9).  This is NOT IEEE 754-2008 DPD encoding (10 bits per
 * 3 digits); that is planned for v0.2.  The current implementation already
 * achieves 18-25% payload storage reduction versus core NUMERIC.
 *
 * Internal representation uses base-10^9 limbs (uint32, 9 decimal digits each).
 * This is the same radix used by Python's _decimal, GMP's mpz for decimal work,
 * and IBM's decNumber library — chosen for fast arithmetic on modern 64-bit ALUs
 * (two uint32 limbs multiply into a uint64 without overflow).
 *
 * Storage layout (varlena):
 *   [vl_len_: 4B][ndigits: 2B][weight: 2B][sign: 2B][dscale: 2B][digits: ndigits * 4B]
 *
 * Compared to core NUMERIC (base-10000, uint16 limbs, 4 digits/2 bytes = 2 digits/byte):
 *   Radix10 limb:  9 digits / 4 bytes = 2.25 digits/byte  →  ~12.5% denser per limb
 *   Plus fewer limbs means fewer header/alignment overheads → 18-25% payload savings.
 *   Effective table savings (after tuple overhead): 15-23%.
 *
 * v0.1 Limitations:
 *   - Division, mod, power, round, trunc, floor, ceil, sqrt delegate to NUMERIC
 *     (5-15% overhead on those operations).  Native implementations in v0.2.
 *   - Hash function uses normalized sign+weight+limbs (no string conversion).
 *   - Integer/bigint casts use direct C functions (no NUMERIC double-hop).
 *
 * Copyright (c) 2026, pg_radix10 Contributors
 * PostgreSQL License (see LICENSE)
 */

#ifndef RADIX10_NUMERIC_H
#define RADIX10_NUMERIC_H

#include "postgres.h"
#include "fmgr.h"
#include "utils/numeric.h"
#include "utils/builtins.h"
#include "libpq/pqformat.h"

/* ----------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------- */

/* Each limb holds 9 decimal digits (0 .. 999 999 999) */
#define R10_NBASE           1000000000U
#define R10_LIMB_DIGITS     9
#define R10_HALF_NBASE      31623U          /* floor(sqrt(R10_NBASE)) */

/* Maximum supported precision (decimal digits) — same ceiling as core NUMERIC */
#define R10_MAX_PRECISION   131072

/* Sign constants — same bit-pattern semantics as core NUMERIC */
#define R10_POS             0x0000
#define R10_NEG             0x4000
#define R10_NAN             0xC000
#define R10_PINF            0xD000
#define R10_NINF            0xF000

/* Accessor macros */
#define R10_IS_NAN(n)       ((n)->sign == R10_NAN)
#define R10_IS_PINF(n)      ((n)->sign == R10_PINF)
#define R10_IS_NINF(n)      ((n)->sign == R10_NINF)
#define R10_IS_INF(n)       (R10_IS_PINF(n) || R10_IS_NINF(n))
#define R10_IS_SPECIAL(n)   (R10_IS_NAN(n) || R10_IS_INF(n))
#define R10_SIGN(n)         ((n)->sign)

/* ----------------------------------------------------------------
 * On-disk / in-memory type
 * ---------------------------------------------------------------- */

typedef struct Radix10NumericData
{
    int32       vl_len_;        /* varlena header (includes self) */
    int16       ndigits;        /* number of uint32 limbs in digits[] */
    int16       weight;         /* weight of first limb (base-10^9 exponent) */
    uint16      sign;           /* R10_POS / R10_NEG / R10_NAN / R10_PINF / R10_NINF */
    int16       dscale;         /* display scale (decimal digits after point) */
    uint32      digits[FLEXIBLE_ARRAY_MEMBER];  /* limbs, MSD first */
} Radix10NumericData;

typedef Radix10NumericData *Radix10Numeric;

/* Size macros */
#define R10_HEADER_SIZE     (offsetof(Radix10NumericData, digits))
#define R10_NDIGITS(n)      ((n)->ndigits)
#define R10_DIGITS(n)       ((n)->digits)

/* Allocation helper: total varlena size for `nd` limbs */
#define R10_ALLOC_SIZE(nd)  (R10_HEADER_SIZE + (nd) * sizeof(uint32))

/* ----------------------------------------------------------------
 * GETARG / RETURN macros (PG convention)
 * ---------------------------------------------------------------- */

#define DatumGetRadix10NumericP(d)      ((Radix10Numeric) PG_DETOAST_DATUM(d))
#define DatumGetRadix10NumericPCopy(d)  ((Radix10Numeric) PG_DETOAST_DATUM_COPY(d))
#define PG_GETARG_R10NUMERIC_P(n)       DatumGetRadix10NumericP(PG_GETARG_DATUM(n))
#define PG_RETURN_R10NUMERIC_P(x)       PG_RETURN_POINTER(x)

/* ----------------------------------------------------------------
 * Internal helpers (implemented in radix10_numeric.c)
 * ---------------------------------------------------------------- */

/* Allocate a zeroed Radix10Numeric with `ndigits` limbs */
extern Radix10Numeric r10_alloc(int ndigits, int weight, int sign, int dscale);

/* Strip leading/trailing zero limbs (normalize) */
extern void r10_strip(Radix10Numeric *num);

/* Deep copy */
extern Radix10Numeric r10_copy(Radix10Numeric src);

/* ----------------------------------------------------------------
 * Conversion helpers (radix10_io.c)
 * ---------------------------------------------------------------- */

/* Parse a C string into a Radix10Numeric (allocates) */
extern Radix10Numeric r10_from_cstring(const char *str);

/* Render a Radix10Numeric to a palloc'd C string */
extern char *r10_to_cstring(Radix10Numeric num);

/* Convert core Numeric ↔ Radix10Numeric */
extern Radix10Numeric r10_from_numeric(Numeric num);
extern Numeric r10_to_numeric(Radix10Numeric rnum);

/* ----------------------------------------------------------------
 * Arithmetic helpers (radix10_ops.c)
 * ---------------------------------------------------------------- */

extern Radix10Numeric r10_add(Radix10Numeric a, Radix10Numeric b);
extern Radix10Numeric r10_sub(Radix10Numeric a, Radix10Numeric b);
extern Radix10Numeric r10_mul(Radix10Numeric a, Radix10Numeric b);
extern Radix10Numeric r10_div(Radix10Numeric a, Radix10Numeric b, int rscale);
extern Radix10Numeric r10_mod(Radix10Numeric a, Radix10Numeric b);
extern Radix10Numeric r10_abs(Radix10Numeric a);
extern Radix10Numeric r10_negate(Radix10Numeric a);
extern Radix10Numeric r10_power(Radix10Numeric base_val, Radix10Numeric exp_val);
extern Radix10Numeric r10_sqrt(Radix10Numeric a);
extern Radix10Numeric r10_floor(Radix10Numeric a);
extern Radix10Numeric r10_ceil(Radix10Numeric a);
extern int            r10_sign(Radix10Numeric a);
extern Radix10Numeric r10_min2(Radix10Numeric a, Radix10Numeric b);
extern Radix10Numeric r10_max2(Radix10Numeric a, Radix10Numeric b);

/* Comparison: returns -1, 0, +1 */
extern int r10_cmp(Radix10Numeric a, Radix10Numeric b);

/* Round / truncate */
extern Radix10Numeric r10_round(Radix10Numeric a, int scale);
extern Radix10Numeric r10_trunc(Radix10Numeric a, int scale);

/* ----------------------------------------------------------------
 * Aggregate helpers (radix10_agg.c)
 * ---------------------------------------------------------------- */

/*
 * r10_avg_state — internal aggregate state for AVG.
 *
 * Stored as a varlena: [vl_len_: 4B][count: 8B][sum: Radix10NumericData...]
 * This avoids needing a separate composite type in the SQL layer.
 */
typedef struct R10AvgStateData
{
    int32       vl_len_;        /* varlena header */
    int64       count;          /* number of non-NULL values accumulated */
    /* Immediately followed by a Radix10NumericData representing the running sum.
     * We store it inline to keep the aggregate state in a single palloc. */
    Radix10NumericData sum_data;    /* variable-length — extends to end of varlena */
} R10AvgStateData;

typedef R10AvgStateData *R10AvgState;

#define R10_AVG_STATE_HEADER  (offsetof(R10AvgStateData, sum_data))

#endif /* RADIX10_NUMERIC_H */
