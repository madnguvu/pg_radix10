-- =============================================================================
-- pg_radix10--1.0.sql
--
-- Radix-10^9 numeric type for PostgreSQL
-- Uses base-10^9 limbs (9 decimal digits per uint32) for 15-23% storage
-- savings over core NUMERIC while maintaining identical SQL semantics.
--
-- Copyright (c) 2026, pg_radix10 Contributors
-- PostgreSQL License (see LICENSE)
-- =============================================================================

-- Complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_radix10" to load this file. \quit

-- =============================================================================
-- SHELL TYPE (forward declaration)
-- =============================================================================

CREATE TYPE radix10_numeric;

-- =============================================================================
-- INPUT / OUTPUT FUNCTIONS
-- =============================================================================

CREATE FUNCTION radix10_numeric_in(cstring, oid, int4)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_in'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION radix10_numeric_out(radix10_numeric)
    RETURNS cstring
    AS 'MODULE_PATHNAME', 'radix10_numeric_out'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- =============================================================================
-- BINARY I/O (replication, COPY BINARY, FDW, pg_dump --format=custom)
-- =============================================================================

CREATE FUNCTION radix10_numeric_send(radix10_numeric)
    RETURNS bytea
    AS 'MODULE_PATHNAME', 'radix10_numeric_send'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION radix10_numeric_recv(internal, oid, int4)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_recv'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- =============================================================================
-- TYPMOD SUPPORT (radix10_numeric(precision, scale))
-- =============================================================================

CREATE FUNCTION radix10_numeric_typmod_in(cstring[])
    RETURNS int4
    AS 'MODULE_PATHNAME', 'radix10_numeric_typmod_in'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION radix10_numeric_typmod_out(int4)
    RETURNS cstring
    AS 'MODULE_PATHNAME', 'radix10_numeric_typmod_out'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- =============================================================================
-- FULL TYPE DEFINITION
-- =============================================================================

CREATE TYPE radix10_numeric (
    internallength = variable,
    input          = radix10_numeric_in,
    output         = radix10_numeric_out,
    send           = radix10_numeric_send,
    receive        = radix10_numeric_recv,
    typmod_in      = radix10_numeric_typmod_in,
    typmod_out     = radix10_numeric_typmod_out,
    alignment      = double,
    storage        = extended,
    category       = 'N',       -- Numeric category (important for implicit casts)
    preferred      = false
);

-- =============================================================================
-- CASTS: NUMERIC ↔ radix10_numeric (seamless, implicit)
-- =============================================================================

CREATE FUNCTION radix10_numeric_from_numeric(numeric)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_from_numeric'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION numeric_from_radix10_numeric(radix10_numeric)
    RETURNS numeric
    AS 'MODULE_PATHNAME', 'numeric_from_radix10_numeric'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE CAST (numeric AS radix10_numeric)
    WITH FUNCTION radix10_numeric_from_numeric(numeric)
    AS IMPLICIT;

CREATE CAST (radix10_numeric AS numeric)
    WITH FUNCTION numeric_from_radix10_numeric(radix10_numeric)
    AS IMPLICIT;

-- Integer casts: direct (no NUMERIC double-hop)
CREATE FUNCTION radix10_numeric_from_int4(integer)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_from_int4'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION radix10_numeric_from_int8(bigint)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_from_int8'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE CAST (integer AS radix10_numeric)
    WITH FUNCTION radix10_numeric_from_int4(integer)
    AS IMPLICIT;

CREATE CAST (bigint AS radix10_numeric)
    WITH FUNCTION radix10_numeric_from_int8(bigint)
    AS IMPLICIT;

CREATE CAST (radix10_numeric AS integer)
    WITH FUNCTION numeric_from_radix10_numeric(radix10_numeric)
    AS ASSIGNMENT;

CREATE CAST (radix10_numeric AS bigint)
    WITH FUNCTION numeric_from_radix10_numeric(radix10_numeric)
    AS ASSIGNMENT;

CREATE CAST (radix10_numeric AS double precision)
    WITH FUNCTION numeric_from_radix10_numeric(radix10_numeric)
    AS IMPLICIT;

-- =============================================================================
-- ARITHMETIC OPERATORS
-- =============================================================================

-- Addition
CREATE FUNCTION radix10_numeric_add(radix10_numeric, radix10_numeric)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_add'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR + (
    leftarg    = radix10_numeric,
    rightarg   = radix10_numeric,
    procedure  = radix10_numeric_add,
    commutator = +
);

-- Subtraction
CREATE FUNCTION radix10_numeric_sub(radix10_numeric, radix10_numeric)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_sub'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR - (
    leftarg    = radix10_numeric,
    rightarg   = radix10_numeric,
    procedure  = radix10_numeric_sub
);

-- Multiplication
CREATE FUNCTION radix10_numeric_mul(radix10_numeric, radix10_numeric)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_mul'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR * (
    leftarg    = radix10_numeric,
    rightarg   = radix10_numeric,
    procedure  = radix10_numeric_mul,
    commutator = *
);

-- Division
CREATE FUNCTION radix10_numeric_div(radix10_numeric, radix10_numeric)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_div'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR / (
    leftarg    = radix10_numeric,
    rightarg   = radix10_numeric,
    procedure  = radix10_numeric_div
);

-- Modulo
CREATE FUNCTION radix10_numeric_mod(radix10_numeric, radix10_numeric)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_mod'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR % (
    leftarg    = radix10_numeric,
    rightarg   = radix10_numeric,
    procedure  = radix10_numeric_mod
);

-- Unary minus
CREATE FUNCTION radix10_numeric_uminus(radix10_numeric)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_uminus'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR - (
    rightarg   = radix10_numeric,
    procedure  = radix10_numeric_uminus
);

-- Unary plus
CREATE FUNCTION radix10_numeric_uplus(radix10_numeric)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_uplus'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR + (
    rightarg   = radix10_numeric,
    procedure  = radix10_numeric_uplus
);

-- Absolute value
CREATE FUNCTION radix10_numeric_abs(radix10_numeric)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_abs'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Round
CREATE FUNCTION r10_round(radix10_numeric, int4)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_round_sql'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Truncate
CREATE FUNCTION r10_trunc(radix10_numeric, int4)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_trunc_sql'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Power ( ^ )
CREATE FUNCTION radix10_numeric_power(radix10_numeric, radix10_numeric)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_power'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR ^ (
    leftarg    = radix10_numeric,
    rightarg   = radix10_numeric,
    procedure  = radix10_numeric_power
);

-- Square root
CREATE FUNCTION r10_sqrt(radix10_numeric)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_sqrt_sql'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Floor
CREATE FUNCTION r10_floor(radix10_numeric)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_floor_sql'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Ceil
CREATE FUNCTION r10_ceil(radix10_numeric)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_ceil_sql'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Sign (-1, 0, +1)
CREATE FUNCTION r10_sign(radix10_numeric)
    RETURNS int4
    AS 'MODULE_PATHNAME', 'radix10_numeric_sign_sql'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- =============================================================================
-- COMPARISON OPERATORS
-- =============================================================================

CREATE FUNCTION radix10_numeric_eq(radix10_numeric, radix10_numeric)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'radix10_numeric_eq'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION radix10_numeric_ne(radix10_numeric, radix10_numeric)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'radix10_numeric_ne'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION radix10_numeric_lt(radix10_numeric, radix10_numeric)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'radix10_numeric_lt'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION radix10_numeric_le(radix10_numeric, radix10_numeric)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'radix10_numeric_le'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION radix10_numeric_gt(radix10_numeric, radix10_numeric)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'radix10_numeric_gt'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION radix10_numeric_ge(radix10_numeric, radix10_numeric)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'radix10_numeric_ge'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION radix10_numeric_cmp(radix10_numeric, radix10_numeric)
    RETURNS int4
    AS 'MODULE_PATHNAME', 'radix10_numeric_cmp'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Equality
CREATE OPERATOR = (
    leftarg    = radix10_numeric,
    rightarg   = radix10_numeric,
    procedure  = radix10_numeric_eq,
    commutator = =,
    negator    = <>,
    restrict   = eqsel,
    join       = eqjoinsel,
    hashes,
    merges
);

-- Not equal
CREATE OPERATOR <> (
    leftarg    = radix10_numeric,
    rightarg   = radix10_numeric,
    procedure  = radix10_numeric_ne,
    commutator = <>,
    negator    = =,
    restrict   = neqsel,
    join       = neqjoinsel
);

-- Less than
CREATE OPERATOR < (
    leftarg    = radix10_numeric,
    rightarg   = radix10_numeric,
    procedure  = radix10_numeric_lt,
    commutator = >,
    negator    = >=,
    restrict   = scalarltsel,
    join       = scalarltjoinsel
);

-- Less than or equal
CREATE OPERATOR <= (
    leftarg    = radix10_numeric,
    rightarg   = radix10_numeric,
    procedure  = radix10_numeric_le,
    commutator = >=,
    negator    = >,
    restrict   = scalarlesel,
    join       = scalarlejoinsel
);

-- Greater than
CREATE OPERATOR > (
    leftarg    = radix10_numeric,
    rightarg   = radix10_numeric,
    procedure  = radix10_numeric_gt,
    commutator = <,
    negator    = <=,
    restrict   = scalargtsel,
    join       = scalargtjoinsel
);

-- Greater than or equal
CREATE OPERATOR >= (
    leftarg    = radix10_numeric,
    rightarg   = radix10_numeric,
    procedure  = radix10_numeric_ge,
    commutator = <=,
    negator    = <,
    restrict   = scalargesel,
    join       = scalargejoinsel
);

-- =============================================================================
-- BTREE OPERATOR CLASS (enables ORDER BY, indexes, merge joins)
-- =============================================================================

CREATE OPERATOR CLASS radix10_numeric_ops
    DEFAULT FOR TYPE radix10_numeric USING btree AS
        OPERATOR 1  <,
        OPERATOR 2  <=,
        OPERATOR 3  =,
        OPERATOR 4  >=,
        OPERATOR 5  >,
        FUNCTION 1  radix10_numeric_cmp(radix10_numeric, radix10_numeric);

-- =============================================================================
-- HASH OPERATOR CLASS (enables hash indexes, hash joins, DISTINCT)
-- =============================================================================

CREATE FUNCTION radix10_numeric_hash(radix10_numeric)
    RETURNS int4
    AS 'MODULE_PATHNAME', 'radix10_numeric_hash'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR CLASS radix10_numeric_hash_ops
    DEFAULT FOR TYPE radix10_numeric USING hash AS
        OPERATOR 1  =,
        FUNCTION 1  radix10_numeric_hash(radix10_numeric);

-- =============================================================================
-- AGGREGATE FUNCTIONS
-- =============================================================================

-- SUM
CREATE FUNCTION radix10_numeric_sum_accum(radix10_numeric, radix10_numeric)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_sum_accum'
    LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE AGGREGATE sum(radix10_numeric) (
    sfunc     = radix10_numeric_sum_accum,
    stype     = radix10_numeric,
    combinefunc = radix10_numeric_add,
    parallel  = safe
);

-- MIN
CREATE FUNCTION radix10_numeric_smaller(radix10_numeric, radix10_numeric)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_smaller'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE AGGREGATE min(radix10_numeric) (
    sfunc     = radix10_numeric_smaller,
    stype     = radix10_numeric,
    combinefunc = radix10_numeric_smaller,
    parallel  = safe,
    sortop    = <
);

-- MAX
CREATE FUNCTION radix10_numeric_larger(radix10_numeric, radix10_numeric)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_larger'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE AGGREGATE max(radix10_numeric) (
    sfunc     = radix10_numeric_larger,
    stype     = radix10_numeric,
    combinefunc = radix10_numeric_larger,
    parallel  = safe,
    sortop    = >
);

-- =============================================================================
-- AVG AGGREGATE — radix10-native implementation (count + sum state)
--
-- Uses an internal state type (bytea-compatible varlena) that holds
-- {count: int64, sum: Radix10NumericData} in a single allocation.
-- The combine function merges two states for parallel aggregation.
-- The final function divides sum / count using radix10 arithmetic.
-- =============================================================================

CREATE FUNCTION radix10_numeric_avg_accum(internal, radix10_numeric)
    RETURNS internal
    AS 'MODULE_PATHNAME', 'radix10_numeric_avg_accum'
    LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION radix10_numeric_avg_combine(internal, internal)
    RETURNS internal
    AS 'MODULE_PATHNAME', 'radix10_numeric_avg_combine'
    LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION radix10_numeric_avg_final(internal)
    RETURNS radix10_numeric
    AS 'MODULE_PATHNAME', 'radix10_numeric_avg_final'
    LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE AGGREGATE avg(radix10_numeric) (
    sfunc       = radix10_numeric_avg_accum,
    stype       = internal,
    finalfunc   = radix10_numeric_avg_final,
    combinefunc = radix10_numeric_avg_combine,
    parallel    = safe
);

-- =============================================================================
-- CONVENIENCE FUNCTIONS
-- =============================================================================

-- Storage size comparison helper
CREATE FUNCTION radix10_numeric_storage_size(radix10_numeric)
    RETURNS int4
    AS $$ SELECT pg_column_size($1) $$
    LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION radix10_numeric_storage_size(radix10_numeric) IS
    'Returns the on-disk storage size in bytes for a radix10_numeric value. '
    'Compare with pg_column_size() on a NUMERIC to measure savings.';

-- Width_bucket (for histogram / binning queries)
CREATE FUNCTION r10_width_bucket(radix10_numeric, radix10_numeric, radix10_numeric, int4)
    RETURNS int4
    AS $$
        SELECT width_bucket($1::numeric, $2::numeric, $3::numeric, $4)
    $$
    LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION r10_width_bucket(radix10_numeric, radix10_numeric, radix10_numeric, int4) IS
    'Assigns a radix10_numeric value to one of N equally-spaced buckets '
    'in the range [lo, hi). Useful for histograms and distribution analysis.';

-- Generate_series support (useful for test data generation)
CREATE FUNCTION r10_generate_series(radix10_numeric, radix10_numeric, radix10_numeric)
    RETURNS SETOF radix10_numeric
    AS $$
        SELECT x::radix10_numeric
        FROM generate_series($1::numeric, $2::numeric, $3::numeric) x
    $$
    LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
    ROWS 100;

COMMENT ON FUNCTION r10_generate_series(radix10_numeric, radix10_numeric, radix10_numeric) IS
    'Generate a series of radix10_numeric values from start to stop with step. '
    'Example: SELECT * FROM r10_generate_series(''1''::radix10_numeric, ''10''::radix10_numeric, ''0.5''::radix10_numeric);';

-- =============================================================================
-- DOCUMENTATION
-- =============================================================================

COMMENT ON TYPE radix10_numeric IS
    'Radix-10^9 numeric: a drop-in NUMERIC alternative using base-10^9 limbs '
    '(9 decimal digits per 4-byte word) for 15-23% storage savings. '
    'Fully compatible with NUMERIC via implicit casts. '
    'Part of the pg_radix10 extension (https://github.com/matt-hudson/pg_radix10).';
