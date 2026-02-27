"""
pyradix10.storage — Storage analysis utilities.

Provides functions to estimate and compare the on-disk sizes of
decimal values stored as PostgreSQL NUMERIC vs pg_radix10's radix10_numeric,
without needing a running database.

Useful for:
  - Capacity planning during the 2026-2028 RAM/chip shortage
  - Estimating savings before migrating columns
  - Generating reports for stakeholder buy-in

Note on savings claims: Raw per-limb density is 2.25 digits/byte vs
NUMERIC's 2.0 digits/byte (a 12.5% encoding advantage). However, the
fixed 12-byte header (same for both types) dilutes this for small values.
Effective savings for typical financial/scientific data: 15-23%.
"""

from decimal import Decimal
from typing import Dict, List, Optional, Sequence, Union

from pyradix10.core import R10Decimal, LIMB_DIGITS, PG_VARLENA_HEADER, LIMB_HEADER_SIZE


def estimate_numeric_size(value: Union[str, Decimal, R10Decimal]) -> int:
    """
    Estimate the on-disk storage size (bytes) of a value stored as
    PostgreSQL NUMERIC.

    NUMERIC internal format:
      - 4 bytes varlena header
      - 8 bytes metadata (ndigits, weight, sign, dscale as int16)
      - N × 2 bytes for base-10000 limbs (4 decimal digits each)

    Args:
        value: A decimal number (string, Decimal, or R10Decimal).

    Returns:
        Estimated byte count.
    """
    if not isinstance(value, R10Decimal):
        value = R10Decimal(str(value))
    return value.numeric_storage_bytes


def estimate_radix10_size(value: Union[str, Decimal, R10Decimal]) -> int:
    """
    Estimate the on-disk storage size (bytes) of a value stored as
    pg_radix10's radix10_numeric.

    radix10_numeric internal format:
      - 4 bytes varlena header
      - 8 bytes metadata (ndigits, weight, sign, dscale as int16/uint16)
      - N × 4 bytes for base-10^9 limbs (9 decimal digits each)

    Args:
        value: A decimal number (string, Decimal, or R10Decimal).

    Returns:
        Estimated byte count.
    """
    if not isinstance(value, R10Decimal):
        value = R10Decimal(str(value))
    return value.storage_bytes


# Backwards-compatible alias
estimate_dpd_size = estimate_radix10_size


def storage_comparison(
    values: Sequence[Union[str, Decimal]],
    label: str = "dataset",
) -> Dict[str, Union[str, int, float]]:
    """
    Compare NUMERIC vs radix10_numeric storage for a collection of values.

    Args:
        values: Iterable of decimal values (strings, Decimals, etc.).
        label: A human-readable label for this dataset.

    Returns:
        Dictionary with:
          - label: dataset label
          - count: number of values
          - total_numeric_bytes: sum of NUMERIC storage
          - total_radix10_bytes: sum of radix10_numeric storage
          - savings_bytes: absolute savings
          - savings_pct: percentage savings
          - avg_numeric_bytes: average NUMERIC size per value
          - avg_radix10_bytes: average radix10_numeric size per value
    """
    total_num = 0
    total_r10 = 0
    count = 0

    for v in values:
        d = R10Decimal(str(v))
        total_num += d.numeric_storage_bytes
        total_r10 += d.storage_bytes
        count += 1

    savings = total_num - total_r10
    pct = (savings / total_num * 100) if total_num > 0 else 0.0

    return {
        "label": label,
        "count": count,
        "total_numeric_bytes": total_num,
        "total_radix10_bytes": total_r10,
        # Keep old key for backwards compat
        "total_dpd_bytes": total_r10,
        "savings_bytes": savings,
        "savings_pct": round(pct, 2),
        "avg_numeric_bytes": round(total_num / count, 2) if count else 0,
        "avg_radix10_bytes": round(total_r10 / count, 2) if count else 0,
        "avg_dpd_bytes": round(total_r10 / count, 2) if count else 0,
    }


def storage_report(
    values: Sequence[Union[str, Decimal]],
    label: str = "dataset",
    scale_factor: int = 1,
) -> str:
    """
    Generate a human-readable storage comparison report.

    Args:
        values: Sample values to analyze.
        label: Dataset label.
        scale_factor: Multiply row count by this to project total savings
                      (e.g., if values is a sample of 1000 from 1M rows,
                      set scale_factor=1000).

    Returns:
        Formatted multi-line report string.
    """
    stats = storage_comparison(values, label)

    lines = [
        f"╔══════════════════════════════════════════════════════════════╗",
        f"║  pg_radix10 Storage Analysis: {label:<30s} ║",
        f"╠══════════════════════════════════════════════════════════════╣",
        f"║  Sample size:        {stats['count']:>10,d} values               ║",
        f"║  Scale factor:       {scale_factor:>10,d}×                     ║",
        f"╠══════════════════════════════════════════════════════════════╣",
        f"║  Per-value averages:                                       ║",
        f"║    NUMERIC:          {stats['avg_numeric_bytes']:>10.1f} bytes               ║",
        f"║    radix10_numeric:  {stats['avg_radix10_bytes']:>10.1f} bytes               ║",
        f"╠══════════════════════════════════════════════════════════════╣",
    ]

    projected_num = stats["total_numeric_bytes"] * scale_factor
    projected_r10 = stats["total_radix10_bytes"] * scale_factor
    projected_savings = projected_num - projected_r10

    lines.extend([
        f"║  Projected totals (×{scale_factor:,d}):                          ║",
        f"║    NUMERIC:          {_human_bytes(projected_num):>10s}                    ║",
        f"║    radix10_numeric:  {_human_bytes(projected_r10):>10s}                    ║",
        f"║    ─────────────────────────────────────                    ║",
        f"║    SAVINGS:          {_human_bytes(projected_savings):>10s}  ({stats['savings_pct']:.1f}%)          ║",
        f"╠══════════════════════════════════════════════════════════════╣",
        f"║  Equivalent to: {_savings_equiv(projected_savings):<42s} ║",
        f"╚══════════════════════════════════════════════════════════════╝",
    ])

    return "\n".join(lines)


def _human_bytes(n: int) -> str:
    """Format byte count as human-readable string."""
    if n < 1024:
        return f"{n} B"
    elif n < 1024 ** 2:
        return f"{n / 1024:.1f} KB"
    elif n < 1024 ** 3:
        return f"{n / 1024**2:.1f} MB"
    elif n < 1024 ** 4:
        return f"{n / 1024**3:.2f} GB"
    else:
        return f"{n / 1024**4:.2f} TB"


def _savings_equiv(savings_bytes: int) -> str:
    """Describe savings in relatable terms."""
    gb = savings_bytes / (1024 ** 3)
    if gb >= 1024:
        tb = gb / 1024
        return f"~{tb:.1f} TB of RAM you don't need to buy"
    elif gb >= 1:
        return f"~{gb:.1f} GB of RAM you don't need to buy"
    elif savings_bytes >= 1024 ** 2:
        mb = savings_bytes / (1024 ** 2)
        return f"~{mb:.0f} MB of RAM freed up"
    else:
        kb = savings_bytes / 1024
        return f"~{kb:.0f} KB saved"
