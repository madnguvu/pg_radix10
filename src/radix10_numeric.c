/*
 * radix10_numeric.c — Core type management for radix10_numeric
 *
 * Allocation, normalization, deep-copy, and the PG type shell functions
 * (typmod support, binary send/recv, direct integer casts, hash).
 *
 * Copyright (c) 2026, pg_radix10 Contributors
 * PostgreSQL License (see LICENSE)
 */

#include "radix10.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "libpq/pqformat.h"

PG_MODULE_MAGIC;

/* ================================================================
 * Allocation helpers
 * ================================================================ */

Radix10Numeric
r10_alloc(int ndigits, int weight, int sign, int dscale)
{
    Size            sz;
    Radix10Numeric  result;

    sz = R10_ALLOC_SIZE(ndigits);
    result = (Radix10Numeric) palloc0(sz);
    SET_VARSIZE(result, sz);
    result->ndigits = (int16) ndigits;
    result->weight  = (int16) weight;
    result->sign    = (uint16) sign;
    result->dscale  = (int16) dscale;

    return result;
}

void
r10_strip(Radix10Numeric *num)
{
    Radix10Numeric  n = *num;
    int             ndigits = n->ndigits;
    uint32         *digits  = n->digits;
    int             i;

    /* Strip leading zeroes */
    i = 0;
    while (i < ndigits && digits[i] == 0)
        i++;

    if (i > 0)
    {
        int     newnd = ndigits - i;

        memmove(digits, digits + i, newnd * sizeof(uint32));
        n->ndigits = (int16) newnd;
        n->weight -= (int16) i;
        ndigits = newnd;
    }

    /* Strip trailing zeroes */
    while (ndigits > 0 && digits[ndigits - 1] == 0)
        ndigits--;
    n->ndigits = (int16) ndigits;

    /* If all digits are zero, normalize to canonical zero */
    if (ndigits == 0)
    {
        n->sign   = R10_POS;
        n->weight = 0;
    }

    SET_VARSIZE(n, R10_ALLOC_SIZE(ndigits));
}

Radix10Numeric
r10_copy(Radix10Numeric src)
{
    Size            sz = VARSIZE(src);
    Radix10Numeric  dst = (Radix10Numeric) palloc(sz);

    memcpy(dst, src, sz);
    return dst;
}

/* ================================================================
 * Type I/O: text input / output
 * ================================================================ */

PG_FUNCTION_INFO_V1(radix10_numeric_in);
Datum
radix10_numeric_in(PG_FUNCTION_ARGS)
{
    char           *str = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
    Oid             typelem  = PG_GETARG_OID(1);
#endif
    int32           typmod   = PG_GETARG_INT32(2);
    Radix10Numeric  result;

    result = r10_from_cstring(str);

    if (typmod >= 0)
    {
        int precision = ((typmod - VARHDRSZ) >> 16) & 0xFFFF;
        int scale     = (typmod - VARHDRSZ) & 0xFFFF;

        if (scale >= 0)
            result = r10_round(result, scale);

        (void) precision;
    }

    PG_RETURN_R10NUMERIC_P(result);
}

PG_FUNCTION_INFO_V1(radix10_numeric_out);
Datum
radix10_numeric_out(PG_FUNCTION_ARGS)
{
    Radix10Numeric  num = PG_GETARG_R10NUMERIC_P(0);
    char           *str;

    str = r10_to_cstring(num);
    PG_RETURN_CSTRING(str);
}

/* ================================================================
 * Type I/O: binary send / receive
 * ================================================================ */

PG_FUNCTION_INFO_V1(radix10_numeric_send);
Datum
radix10_numeric_send(PG_FUNCTION_ARGS)
{
    Radix10Numeric  num = PG_GETARG_R10NUMERIC_P(0);
    StringInfoData  buf;
    int             i;

    pq_begintypsend(&buf);
    pq_sendint16(&buf, num->ndigits);
    pq_sendint16(&buf, num->weight);
    pq_sendint16(&buf, (int16) num->sign);   /* wire as 2 bytes */
    pq_sendint16(&buf, num->dscale);

    for (i = 0; i < num->ndigits; i++)
        pq_sendint32(&buf, (int32) num->digits[i]);

    PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

PG_FUNCTION_INFO_V1(radix10_numeric_recv);
Datum
radix10_numeric_recv(PG_FUNCTION_ARGS)
{
    StringInfo      buf    = (StringInfo) PG_GETARG_POINTER(0);
    int16           ndigits, weight, dscale;
    uint16          sign;
    Radix10Numeric  result;
    int             i;

    ndigits = pq_getmsgint(buf, 2);
    weight  = pq_getmsgint(buf, 2);
    sign    = (uint16) pq_getmsgint(buf, 2);
    dscale  = pq_getmsgint(buf, 2);

    if (ndigits < 0 || ndigits > R10_MAX_PRECISION / R10_LIMB_DIGITS + 1)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("invalid radix10_numeric: ndigits = %d", ndigits)));

    result = r10_alloc(ndigits, weight, sign, dscale);

    for (i = 0; i < ndigits; i++)
    {
        uint32 d = (uint32) pq_getmsgint(buf, 4);
        if (d >= R10_NBASE)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                     errmsg("invalid radix10_numeric limb value: %u", d)));
        result->digits[i] = d;
    }

    PG_RETURN_R10NUMERIC_P(result);
}

/* ================================================================
 * Casts: NUMERIC ↔ radix10_numeric
 * ================================================================ */

PG_FUNCTION_INFO_V1(radix10_numeric_from_numeric);
Datum
radix10_numeric_from_numeric(PG_FUNCTION_ARGS)
{
    Numeric         src = PG_GETARG_NUMERIC(0);
    Radix10Numeric  result;

    result = r10_from_numeric(src);
    PG_RETURN_R10NUMERIC_P(result);
}

PG_FUNCTION_INFO_V1(numeric_from_radix10_numeric);
Datum
numeric_from_radix10_numeric(PG_FUNCTION_ARGS)
{
    Radix10Numeric  src = PG_GETARG_R10NUMERIC_P(0);
    Numeric         result;

    result = r10_to_numeric(src);
    PG_RETURN_NUMERIC(result);
}

/* ================================================================
 * Direct integer casts (no NUMERIC double-hop)
 * ================================================================ */

PG_FUNCTION_INFO_V1(radix10_numeric_from_int4);
Datum
radix10_numeric_from_int4(PG_FUNCTION_ARGS)
{
    int32           val = PG_GETARG_INT32(0);
    char            buf[16];

    snprintf(buf, sizeof(buf), "%d", val);
    PG_RETURN_R10NUMERIC_P(r10_from_cstring(buf));
}

PG_FUNCTION_INFO_V1(radix10_numeric_from_int8);
Datum
radix10_numeric_from_int8(PG_FUNCTION_ARGS)
{
    int64           val = PG_GETARG_INT64(0);
    char            buf[24];

    snprintf(buf, sizeof(buf), "%lld", (long long) val);
    PG_RETURN_R10NUMERIC_P(r10_from_cstring(buf));
}

/* ================================================================
 * Typmod support (precision, scale)
 * ================================================================ */

PG_FUNCTION_INFO_V1(radix10_numeric_typmod_in);
Datum
radix10_numeric_typmod_in(PG_FUNCTION_ARGS)
{
    ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);
    int32      *tl;
    int         n;
    int32       precision, scale, typmod;

    tl = ArrayGetIntegerTypmods(ta, &n);

    if (n == 1)
    {
        precision = tl[0];
        scale = 0;
    }
    else if (n == 2)
    {
        precision = tl[0];
        scale     = tl[1];
    }
    else
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid type modifier for radix10_numeric"),
                 errhint("Usage: radix10_numeric(precision) or radix10_numeric(precision, scale)")));
        precision = scale = 0;
    }

    if (precision < 1 || precision > R10_MAX_PRECISION)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("radix10_numeric precision %d out of range [1, %d]",
                        precision, R10_MAX_PRECISION)));

    if (scale < 0 || scale > precision)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("radix10_numeric scale %d out of range [0, %d]",
                        scale, precision)));

    typmod = ((precision << 16) | scale) + VARHDRSZ;
    PG_RETURN_INT32(typmod);
}

PG_FUNCTION_INFO_V1(radix10_numeric_typmod_out);
Datum
radix10_numeric_typmod_out(PG_FUNCTION_ARGS)
{
    int32   typmod = PG_GETARG_INT32(0);
    int     precision, scale;
    char   *result;

    precision = ((typmod - VARHDRSZ) >> 16) & 0xFFFF;
    scale     = (typmod - VARHDRSZ) & 0xFFFF;

    if (scale > 0)
        result = psprintf("(%d,%d)", precision, scale);
    else
        result = psprintf("(%d)", precision);

    PG_RETURN_CSTRING(result);
}

/* ================================================================
 * Hash support — normalized limb-based (no string conversion)
 *
 * Equal values must produce equal hashes.  We hash the canonical
 * representation: sign + weight + limb data (already stripped by r10_strip).
 * Special values get fixed sentinels.
 *
 * Uses a stack buffer for small values (≤ 8 limbs covers 72 decimal digits),
 * falling back to palloc only for unusually wide numbers.
 * ================================================================ */

PG_FUNCTION_INFO_V1(radix10_numeric_hash);
Datum
radix10_numeric_hash(PG_FUNCTION_ARGS)
{
    Radix10Numeric  num = PG_GETARG_R10NUMERIC_P(0);

    /* Sentinels for special values */
    if (R10_IS_NAN(num))   PG_RETURN_UINT32(0);
    if (R10_IS_PINF(num))  PG_RETURN_UINT32(1);
    if (R10_IS_NINF(num))  PG_RETURN_UINT32(2);

    {
        /* Stack buffer: 4 bytes header + up to 8 limbs × 4 bytes = 36 bytes */
        char            stack[sizeof(uint16) + sizeof(int16) + 8 * sizeof(uint32)];
        int             hdr_len = sizeof(uint16) + sizeof(int16);
        int             limb_len = num->ndigits * sizeof(uint32);
        int             total = hdr_len + limb_len;
        char           *buf;
        uint32          hash;

        buf = (total <= (int) sizeof(stack)) ? stack : (char *) palloc(total);

        memcpy(buf, &num->sign, sizeof(uint16));
        memcpy(buf + sizeof(uint16), &num->weight, sizeof(int16));
        if (limb_len > 0)
            memcpy(buf + hdr_len, num->digits, limb_len);

        hash = DatumGetUInt32(hash_any((unsigned char *) buf, total));

        if (buf != stack)
            pfree(buf);

        PG_RETURN_UINT32(hash);
    }
}
