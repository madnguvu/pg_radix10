-- =============================================================================
-- pg_radix10 regression test
-- =============================================================================

-- Setup
CREATE EXTENSION IF NOT EXISTS pg_radix10;

-- =============================================================================
-- 1. Basic I/O
-- =============================================================================

SELECT '0'::radix10_numeric;
SELECT '1'::radix10_numeric;
SELECT '-1'::radix10_numeric;
SELECT '123456789.123456789'::radix10_numeric;
SELECT '-99999999999999999.9999'::radix10_numeric;
SELECT '0.000000001'::radix10_numeric;
SELECT 'NaN'::radix10_numeric;
SELECT 'Infinity'::radix10_numeric;
SELECT '-Infinity'::radix10_numeric;

-- Very large number (tests multiple limbs)
SELECT '123456789012345678901234567890.123456789012345678901234567890'::radix10_numeric;

-- =============================================================================
-- 2. Cast round-trip: NUMERIC ↔ radix10_numeric
-- =============================================================================

SELECT 42.5::numeric::radix10_numeric;
SELECT '12345.6789'::radix10_numeric::numeric;

-- Round-trip should be lossless
SELECT val, val::radix10_numeric::numeric = val AS round_trip_ok
FROM (VALUES
    (0::numeric),
    (1::numeric),
    (-1::numeric),
    (123456789.123456789::numeric),
    (0.000000001::numeric)
) AS t(val);

-- =============================================================================
-- 3. Arithmetic
-- =============================================================================

-- Addition
SELECT '100.50'::radix10_numeric + '200.25'::radix10_numeric;
SELECT '999999999'::radix10_numeric + '1'::radix10_numeric;

-- Subtraction
SELECT '500.00'::radix10_numeric - '123.45'::radix10_numeric;
SELECT '0'::radix10_numeric - '42'::radix10_numeric;

-- Multiplication
SELECT '12345.67'::radix10_numeric * '89.01'::radix10_numeric;
SELECT '0.001'::radix10_numeric * '1000'::radix10_numeric;

-- Division
SELECT '100'::radix10_numeric / '3'::radix10_numeric;
SELECT '1'::radix10_numeric / '7'::radix10_numeric;

-- Modulo
SELECT '10'::radix10_numeric % '3'::radix10_numeric;

-- Unary operators
SELECT -('42.5'::radix10_numeric);
SELECT +('42.5'::radix10_numeric);
SELECT radix10_numeric_abs('-99.99'::radix10_numeric);

-- =============================================================================
-- 4. Comparisons
-- =============================================================================

SELECT '1'::radix10_numeric = '1.0'::radix10_numeric;
SELECT '1'::radix10_numeric <> '2'::radix10_numeric;
SELECT '1'::radix10_numeric < '2'::radix10_numeric;
SELECT '2'::radix10_numeric <= '2'::radix10_numeric;
SELECT '3'::radix10_numeric > '2'::radix10_numeric;
SELECT '2'::radix10_numeric >= '2'::radix10_numeric;

-- ORDER BY
SELECT val FROM (VALUES
    ('3.14'::radix10_numeric),
    ('2.71'::radix10_numeric),
    ('1.41'::radix10_numeric),
    ('1.73'::radix10_numeric),
    ('0.00'::radix10_numeric),
    ('-1.00'::radix10_numeric)
) AS t(val)
ORDER BY val;

-- =============================================================================
-- 5. Aggregates
-- =============================================================================

CREATE TEMP TABLE r10_test_data (id serial, amount radix10_numeric);
INSERT INTO r10_test_data (amount) VALUES
    ('100.50'), ('200.25'), ('300.75'), ('400.00'), ('500.50');

SELECT sum(amount) FROM r10_test_data;
SELECT min(amount) FROM r10_test_data;
SELECT max(amount) FROM r10_test_data;

-- =============================================================================
-- 6. Table storage and indexing
-- =============================================================================

CREATE TEMP TABLE r10_indexed (
    id serial PRIMARY KEY,
    val radix10_numeric
);

CREATE INDEX ON r10_indexed USING btree (val);

INSERT INTO r10_indexed (val)
SELECT (random() * 1000000)::numeric::radix10_numeric
FROM generate_series(1, 1000);

-- Index scan
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT * FROM r10_indexed WHERE val = '42'::radix10_numeric;
RESET enable_seqscan;

-- =============================================================================
-- 7. Storage size comparison (the whole point!)
-- =============================================================================

-- Create matching NUMERIC and radix10_numeric tables
CREATE TEMP TABLE size_test_numeric (val numeric(38,10));
CREATE TEMP TABLE size_test_r10    (val radix10_numeric);

-- Insert identical data
INSERT INTO size_test_numeric (val)
SELECT (generate_series * 12345.6789012345)::numeric(38,10)
FROM generate_series(1, 10000);

INSERT INTO size_test_r10 (val)
SELECT (generate_series * 12345.6789012345)::numeric(38,10)::radix10_numeric
FROM generate_series(1, 10000);

-- Compare sizes
SELECT
    'numeric' AS type,
    pg_size_pretty(pg_total_relation_size('size_test_numeric')) AS total_size,
    pg_total_relation_size('size_test_numeric') AS bytes
UNION ALL
SELECT
    'radix10_numeric',
    pg_size_pretty(pg_total_relation_size('size_test_r10')),
    pg_total_relation_size('size_test_r10');

-- Per-value comparison
SELECT
    avg(pg_column_size(n.val)) AS avg_numeric_bytes,
    avg(pg_column_size(d.val)) AS avg_r10_bytes,
    round(100.0 * (1 - avg(pg_column_size(d.val))::numeric / avg(pg_column_size(n.val))::numeric), 1) AS pct_savings
FROM size_test_numeric n, size_test_r10 d
WHERE n.ctid = d.ctid;

-- =============================================================================
-- 8. Special values
-- =============================================================================

SELECT 'NaN'::radix10_numeric + '1'::radix10_numeric;
SELECT 'Infinity'::radix10_numeric + '1'::radix10_numeric;
SELECT '-Infinity'::radix10_numeric + 'Infinity'::radix10_numeric;

-- Division by zero
DO $$
BEGIN
    PERFORM '1'::radix10_numeric / '0'::radix10_numeric;
    RAISE EXCEPTION 'should have raised division_by_zero';
EXCEPTION WHEN division_by_zero THEN
    RAISE NOTICE 'division by zero caught correctly';
END;
$$;

-- =============================================================================
-- 9. Round / Truncate
-- =============================================================================

SELECT r10_round('123.456789'::radix10_numeric, 2);
SELECT r10_round('123.455'::radix10_numeric, 2);
SELECT r10_trunc('123.999'::radix10_numeric, 0);

-- =============================================================================
-- 10. Power / sqrt / floor / ceil / sign
-- =============================================================================

-- Power operator
SELECT '2'::radix10_numeric ^ '10'::radix10_numeric;
SELECT '10'::radix10_numeric ^ '3'::radix10_numeric;
SELECT '2.5'::radix10_numeric ^ '2'::radix10_numeric;

-- Square root
SELECT r10_sqrt('144'::radix10_numeric);
SELECT r10_sqrt('2'::radix10_numeric);

-- Floor and ceil
SELECT r10_floor('3.7'::radix10_numeric);
SELECT r10_floor('-3.7'::radix10_numeric);
SELECT r10_ceil('3.2'::radix10_numeric);
SELECT r10_ceil('-3.2'::radix10_numeric);

-- Sign
SELECT r10_sign('42'::radix10_numeric);
SELECT r10_sign('-42'::radix10_numeric);
SELECT r10_sign('0'::radix10_numeric);

-- =============================================================================
-- 11. AVG aggregate (native radix10 implementation)
-- =============================================================================

CREATE TEMP TABLE r10_avg_test (val radix10_numeric);
INSERT INTO r10_avg_test VALUES ('10'), ('20'), ('30');
SELECT avg(val) FROM r10_avg_test;

-- AVG with fractional results
INSERT INTO r10_avg_test VALUES ('7'), ('13');
SELECT avg(val) FROM r10_avg_test;

-- AVG with NULLs (should be skipped)
INSERT INTO r10_avg_test VALUES (NULL);
SELECT avg(val) FROM r10_avg_test;

DROP TABLE r10_avg_test;

-- =============================================================================
-- 12. Scientific notation parsing
-- =============================================================================

SELECT '1.5e3'::radix10_numeric;
SELECT '2.5E-4'::radix10_numeric;
SELECT '1e10'::radix10_numeric;
SELECT '-3.14e0'::radix10_numeric;
SELECT '6.022e23'::radix10_numeric;

-- =============================================================================
-- 13. Convenience functions
-- =============================================================================

-- Width bucket
SELECT r10_width_bucket('3.5'::radix10_numeric, '0'::radix10_numeric, '10'::radix10_numeric, 5);

-- Generate series
SELECT * FROM r10_generate_series('1'::radix10_numeric, '5'::radix10_numeric, '1'::radix10_numeric);

-- =============================================================================
-- 14. Hash index support
-- =============================================================================

CREATE TEMP TABLE r10_hash_test (val radix10_numeric);
CREATE INDEX ON r10_hash_test USING hash (val);

INSERT INTO r10_hash_test VALUES ('42'), ('100.5'), ('-99');

SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT * FROM r10_hash_test WHERE val = '42'::radix10_numeric;
RESET enable_seqscan;

DROP TABLE r10_hash_test;

-- =============================================================================
-- 15. Edge cases: very small fractional values
-- =============================================================================

SELECT '0.000000000000000001'::radix10_numeric;
SELECT '0.000000000000000001'::radix10_numeric + '0.000000000000000001'::radix10_numeric;
SELECT '0.000000000000000001'::radix10_numeric * '1000000000000000000'::radix10_numeric;

-- =============================================================================
-- 16. Mixed arithmetic with NUMERIC (implicit casts)
-- =============================================================================

SELECT '100'::radix10_numeric + 42::numeric;
SELECT 42::numeric + '100'::radix10_numeric;
SELECT '100'::radix10_numeric * 3::numeric;

-- =============================================================================
-- 17. Direct integer casts (no NUMERIC double-hop)
-- =============================================================================

SELECT 42::integer::radix10_numeric;
SELECT 9223372036854775807::bigint::radix10_numeric;
SELECT (-2147483648)::integer::radix10_numeric;

-- =============================================================================
-- Cleanup
-- =============================================================================

DROP TABLE IF EXISTS r10_test_data;
DROP TABLE IF EXISTS r10_indexed;
DROP TABLE IF EXISTS size_test_numeric;
DROP TABLE IF EXISTS size_test_r10;

-- Extension stays installed for further testing
SELECT 'pg_radix10 regression tests passed!' AS result;
