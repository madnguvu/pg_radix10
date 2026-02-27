# pg_radix10 — Radix-10⁹ Numeric Storage for PostgreSQL
# ================================================================
# Stores NUMERIC-compatible values using 9-digit (base 10^9) limbs
# in uint32 words, delivering 15–23% effective storage savings over
# core NUMERIC.
#
# Build:   make
# Install: make install   (or: sudo make install)
# Test:    make installcheck
# Clean:   make clean

MODULE_big = pg_radix10
OBJS = src/radix10_numeric.o src/radix10_io.o src/radix10_ops.o src/radix10_agg.o

EXTENSION = pg_radix10
DATA = sql/pg_radix10--1.0.sql

REGRESS = pg_radix10_test

# Allow overriding pg_config path for cross-compilation or custom installs
PG_CONFIG ?= pg_config

# Extra compiler flags for safety & performance
PG_CFLAGS += -Wall -Wextra -Werror=implicit-function-declaration -O2

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
