"""
pyradix10.adapter — psycopg3 type adapters for radix10_numeric ↔ R10Decimal.

After calling `register_adapters(conn)`, any radix10_numeric column in
query results will automatically be deserialized to R10Decimal, and
R10Decimal parameters will be serialized using the packed binary format.

Usage:
    import psycopg
    from pyradix10 import register_adapters

    conn = psycopg.connect("dbname=mydb")
    register_adapters(conn)

    # radix10_numeric values now round-trip as R10Decimal
    conn.execute("CREATE TABLE t (val radix10_numeric)")
    conn.execute("INSERT INTO t VALUES (%s)", [R10Decimal("123.45")])
    row = conn.execute("SELECT val FROM t").fetchone()
    assert isinstance(row[0], R10Decimal)
"""

from __future__ import annotations

import struct
from decimal import Decimal
from typing import Optional, Union

try:
    import psycopg
    from psycopg import sql
    from psycopg.adapt import Dumper, Loader
    from psycopg.pq import Format
    from psycopg.types import TypeInfo

    HAS_PSYCOPG = True
except ImportError:
    HAS_PSYCOPG = False

from pyradix10.core import R10Decimal


def register_adapters(
    conn_or_ctx: "Union[psycopg.Connection, psycopg.AsyncConnection, psycopg.adapt.AdaptersMap]",
    schema: str = "public",
) -> None:
    """
    Register R10Decimal loaders and dumpers on a psycopg3 connection.

    This queries the database for the radix10_numeric type OID and registers:
      - A text-mode Loader (for simple queries)
      - A binary-mode Loader (for binary COPY and prepared statements)
      - A text-mode Dumper (for parameterized queries)
      - A binary-mode Dumper (for binary COPY)

    Args:
        conn_or_ctx: A psycopg Connection, AsyncConnection, or AdaptersMap.
        schema: The schema where pg_radix10 is installed (default: "public").

    Raises:
        RuntimeError: If psycopg is not installed.
        RuntimeError: If the pg_radix10 extension is not installed in the target database.
    """
    if not HAS_PSYCOPG:
        raise RuntimeError(
            "psycopg3 is required for adapter registration. "
            "Install it with: pip install 'psycopg[binary]>=3.1'"
        )

    # Resolve the type OID
    info = TypeInfo.fetch(conn_or_ctx, "radix10_numeric")
    if info is None:
        raise RuntimeError(
            "Type 'radix10_numeric' not found. "
            "Make sure pg_radix10 is installed: CREATE EXTENSION pg_radix10;"
        )

    # Register the type info first
    info.register(conn_or_ctx)

    # Get the adapters map
    if hasattr(conn_or_ctx, "adapters"):
        adapters = conn_or_ctx.adapters
    else:
        adapters = conn_or_ctx

    # Register loaders (DB → Python)
    adapters.register_loader(info.oid, R10TextLoader)
    adapters.register_loader(info.oid, R10BinaryLoader)

    # Register dumpers (Python → DB)
    adapters.register_dumper(R10Decimal, R10TextDumper)
    adapters.register_dumper(R10Decimal, R10BinaryDumper)

    # Also handle plain Decimal → radix10_numeric (for convenience)
    adapters.register_dumper(Decimal, R10TextDumper)


# ============================================================
# Loaders: DB → Python
# ============================================================

class R10TextLoader(Loader):
    """Load radix10_numeric text representation into R10Decimal."""
    format = Format.TEXT

    def load(self, data: bytes) -> R10Decimal:
        return R10Decimal(data.decode("utf-8"))


class R10BinaryLoader(Loader):
    """Load radix10_numeric binary representation into R10Decimal."""
    format = Format.BINARY

    def load(self, data: bytes) -> R10Decimal:
        return R10Decimal.from_packed_bytes(data)


# ============================================================
# Dumpers: Python → DB
# ============================================================

class R10TextDumper(Dumper):
    """Dump R10Decimal (or Decimal) as text for radix10_numeric."""
    format = Format.TEXT

    def dump(self, obj: Union[R10Decimal, Decimal]) -> bytes:
        return str(obj).encode("utf-8")

    def get_key(self, obj, format_):
        # Always use this dumper for R10Decimal and Decimal
        return type(obj)

    def upgrade(self, obj, format_):
        return self


class R10BinaryDumper(Dumper):
    """Dump R10Decimal as binary (packed limbs) for radix10_numeric."""
    format = Format.BINARY

    def dump(self, obj: Union[R10Decimal, Decimal]) -> bytes:
        if not isinstance(obj, R10Decimal):
            obj = R10Decimal(str(obj))
        return obj.to_packed_bytes()

    def get_key(self, obj, format_):
        return type(obj)

    def upgrade(self, obj, format_):
        return self
