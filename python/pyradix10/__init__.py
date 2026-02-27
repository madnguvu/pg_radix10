"""
pyradix10 — Radix-10^9 numeric library for Python + PostgreSQL

Provides:
  - R10Decimal: a thin wrapper around Python's decimal.Decimal with
    base-10^9 limb packing/unpacking for efficient binary transfer
    to/from pg_radix10 (the PostgreSQL extension).
  - psycopg3 type adapters for seamless radix10_numeric ↔ R10Decimal I/O.
  - Storage analysis utilities for comparing NUMERIC vs radix10_numeric.
  - Benchmark suite demonstrating 15-23% storage savings.

Usage:
    from pyradix10 import R10Decimal, register_adapters

    # Register with psycopg3
    import psycopg
    conn = psycopg.connect("dbname=mydb")
    register_adapters(conn)

    # Now radix10_numeric columns auto-convert to R10Decimal
    cur = conn.execute("SELECT amount FROM ledger")
    for row in cur:
        print(type(row[0]))  # <class 'pyradix10.R10Decimal'>
"""

from pyradix10.core import R10Decimal
from pyradix10.adapter import register_adapters
from pyradix10.storage import (
    estimate_numeric_size,
    estimate_radix10_size,
    estimate_dpd_size,
    storage_comparison,
    storage_report,
)

__version__ = "0.1.0"
__all__ = [
    "R10Decimal",
    "register_adapters",
    "estimate_numeric_size",
    "estimate_radix10_size",
    "estimate_dpd_size",
    "storage_comparison",
    "storage_report",
]
