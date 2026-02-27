/*
 * radix10_agg.c — Aggregate functions for radix10_numeric
 *
 * Implements: SUM, AVG (with custom state type), MIN, MAX, COUNT
 *
 * AVG uses a radix10-native two-part internal state {count, sum} stored
 * as a single varlena (R10AvgStateData).  The final function divides
 * sum / count using radix10 arithmetic → no NUMERIC fallback needed.
 *
 * Copyright (c) 2026, pg_radix10 Contributors
 * PostgreSQL License (see LICENSE)
 */

#include "radix10.h"

/* ================================================================
 * SUM aggregate
 *
 * State: a radix10_numeric accumulator (starts NULL → first value).
 * ================================================================ */

PG_FUNCTION_INFO_V1(radix10_numeric_sum_accum);
Datum
radix10_numeric_sum_accum(PG_FUNCTION_ARGS)
{
    Radix10Numeric  state;
    Radix10Numeric  val = PG_GETARG_R10NUMERIC_P(1);

    if (PG_ARGISNULL(0))
    {
        /* First call — initialize state with val */
        PG_RETURN_R10NUMERIC_P(r10_copy(val));
    }

    state = PG_GETARG_R10NUMERIC_P(0);
    PG_RETURN_R10NUMERIC_P(r10_add(state, val));
}

/* ================================================================
 * AVG aggregate — native radix10 implementation
 *
 * We store the running count and running sum together as a custom
 * internal type (R10AvgStateData / bytea in SQL).  This gives us:
 *   - Exact decimal accumulation (no floating-point drift)
 *   - No NUMERIC conversion overhead during accumulation
 *   - Correct parallel-safe semantics via combine function
 * ================================================================ */

/*
 * Helper: build an initial AVG state from the first input value.
 */
static R10AvgState
make_avg_state(Radix10Numeric first_val)
{
    Size        sum_size = VARSIZE(first_val);
    Size        total;
    R10AvgState state;

    /* Embed the full Radix10NumericData */
    total = offsetof(R10AvgStateData, sum_data) + sum_size;
    state = (R10AvgState) palloc0(total);
    SET_VARSIZE(state, total);
    state->count = 1;
    memcpy(&state->sum_data, first_val, sum_size);

    return state;
}

/*
 * Helper: extract the running sum from an AVG state.
 */
static Radix10Numeric
avg_state_sum(R10AvgState state)
{
    return &state->sum_data;
}

/*
 * radix10_numeric_avg_accum — transition function for AVG.
 *
 * Accumulates (count, sum) in a single varlena state.
 */
PG_FUNCTION_INFO_V1(radix10_numeric_avg_accum);
Datum
radix10_numeric_avg_accum(PG_FUNCTION_ARGS)
{
    Radix10Numeric  val;
    R10AvgState     state;
    R10AvgState     new_state;
    Radix10Numeric  old_sum;
    Radix10Numeric  new_sum;
    Size            new_sum_size;
    Size            total;

    /* Skip NULLs */
    if (PG_ARGISNULL(1))
    {
        if (PG_ARGISNULL(0))
            PG_RETURN_NULL();
        PG_RETURN_POINTER(PG_GETARG_POINTER(0));
    }

    val = PG_GETARG_R10NUMERIC_P(1);

    if (PG_ARGISNULL(0))
    {
        /* First non-NULL value */
        PG_RETURN_POINTER(make_avg_state(val));
    }

    state = (R10AvgState) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));

    /* Compute new sum */
    old_sum = avg_state_sum(state);
    new_sum = r10_add(old_sum, val);
    new_sum_size = VARSIZE(new_sum);

    /* Build new state */
    total = offsetof(R10AvgStateData, sum_data) + new_sum_size;
    new_state = (R10AvgState) palloc(total);
    SET_VARSIZE(new_state, total);
    new_state->count = state->count + 1;
    memcpy(&new_state->sum_data, new_sum, new_sum_size);

    PG_RETURN_POINTER(new_state);
}

/*
 * radix10_numeric_avg_combine — combine two AVG states (for parallel aggregation).
 */
PG_FUNCTION_INFO_V1(radix10_numeric_avg_combine);
Datum
radix10_numeric_avg_combine(PG_FUNCTION_ARGS)
{
    R10AvgState     s1, s2;
    Radix10Numeric  sum1, sum2, combined_sum;
    R10AvgState     result;
    Size            sum_size, total;

    if (PG_ARGISNULL(0) && PG_ARGISNULL(1))
        PG_RETURN_NULL();
    if (PG_ARGISNULL(0))
        PG_RETURN_POINTER(PG_GETARG_POINTER(1));
    if (PG_ARGISNULL(1))
        PG_RETURN_POINTER(PG_GETARG_POINTER(0));

    s1 = (R10AvgState) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
    s2 = (R10AvgState) PG_DETOAST_DATUM(PG_GETARG_DATUM(1));

    sum1 = avg_state_sum(s1);
    sum2 = avg_state_sum(s2);
    combined_sum = r10_add(sum1, sum2);

    sum_size = VARSIZE(combined_sum);
    total = offsetof(R10AvgStateData, sum_data) + sum_size;
    result = (R10AvgState) palloc(total);
    SET_VARSIZE(result, total);
    result->count = s1->count + s2->count;
    memcpy(&result->sum_data, combined_sum, sum_size);

    PG_RETURN_POINTER(result);
}

/*
 * radix10_numeric_avg_final — final function for AVG: returns sum / count.
 */
PG_FUNCTION_INFO_V1(radix10_numeric_avg_final);
Datum
radix10_numeric_avg_final(PG_FUNCTION_ARGS)
{
    R10AvgState     state;
    Radix10Numeric  sum_val;
    Radix10Numeric  count_r10;
    Radix10Numeric  result;
    char            count_str[32];

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    state = (R10AvgState) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));

    if (state->count == 0)
        PG_RETURN_NULL();

    sum_val = avg_state_sum(state);

    /* Convert count to Radix10Numeric for division */
    snprintf(count_str, sizeof(count_str), "%lld", (long long) state->count);
    count_r10 = r10_from_cstring(count_str);

    /* Divide sum / count with reasonable precision */
    {
        int rscale = sum_val->dscale + 6;  /* extra precision for division */
        if (rscale < 6) rscale = 6;
        result = r10_div(sum_val, count_r10, rscale);
    }

    PG_RETURN_R10NUMERIC_P(result);
}

/* ================================================================
 * MIN / MAX aggregates
 * ================================================================ */

PG_FUNCTION_INFO_V1(radix10_numeric_smaller);
Datum
radix10_numeric_smaller(PG_FUNCTION_ARGS)
{
    Radix10Numeric a = PG_GETARG_R10NUMERIC_P(0);
    Radix10Numeric b = PG_GETARG_R10NUMERIC_P(1);

    PG_RETURN_R10NUMERIC_P(r10_cmp(a, b) <= 0 ? r10_copy(a) : r10_copy(b));
}

PG_FUNCTION_INFO_V1(radix10_numeric_larger);
Datum
radix10_numeric_larger(PG_FUNCTION_ARGS)
{
    Radix10Numeric a = PG_GETARG_R10NUMERIC_P(0);
    Radix10Numeric b = PG_GETARG_R10NUMERIC_P(1);

    PG_RETURN_R10NUMERIC_P(r10_cmp(a, b) >= 0 ? r10_copy(a) : r10_copy(b));
}
