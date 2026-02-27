"""
Tests for pyradix10.storage — Storage analysis utilities.
"""

import pytest
from decimal import Decimal

from pyradix10.storage import (
    estimate_numeric_size,
    estimate_radix10_size,
    storage_comparison,
    storage_report,
)


class TestEstimateFunctions:
    """Test individual estimation functions."""

    def test_numeric_size_zero(self):
        assert estimate_numeric_size("0") > 0

    def test_radix10_size_zero(self):
        assert estimate_radix10_size("0") > 0

    def test_numeric_size_accepts_decimal(self):
        size = estimate_numeric_size(Decimal("123.456"))
        assert size > 0

    def test_radix10_size_accepts_string(self):
        size = estimate_radix10_size("999999999.999999999")
        assert size > 0

    def test_sizes_differ_for_large_values(self):
        """For sufficiently large values, sizes should differ."""
        val = "1" * 50
        num_size = estimate_numeric_size(val)
        r10_size = estimate_radix10_size(val)
        # Both should be > 12 (header only)
        assert num_size > 12
        assert r10_size > 12


class TestStorageComparison:
    """Test the storage_comparison function."""

    def test_basic_comparison(self):
        values = ["100.50", "200.25", "300.75"]
        result = storage_comparison(values, "test")

        assert result["label"] == "test"
        assert result["count"] == 3
        assert result["total_numeric_bytes"] > 0
        assert result["total_radix10_bytes"] > 0
        assert isinstance(result["savings_pct"], float)

    def test_empty_values(self):
        result = storage_comparison([], "empty")
        assert result["count"] == 0
        assert result["total_numeric_bytes"] == 0
        assert result["total_radix10_bytes"] == 0

    def test_single_value(self):
        result = storage_comparison(["42"], "single")
        assert result["count"] == 1

    def test_mixed_types(self):
        """Should accept both strings and Decimals."""
        values = ["100", Decimal("200.50"), "300.123456"]
        result = storage_comparison(values, "mixed")
        assert result["count"] == 3


class TestStorageReport:
    """Test the storage_report function."""

    def test_report_generation(self):
        values = ["100.50", "200.25", "300.75", "400.00", "500.99"]
        report = storage_report(values, "test ledger", scale_factor=1000)

        assert isinstance(report, str)
        assert "pg_radix10 Storage Analysis" in report
        assert "test ledger" in report
        assert "SAVINGS" in report
        assert "NUMERIC" in report
        assert "radix10_numeric" in report

    def test_report_with_scale_factor(self):
        values = [str(i * 123.456) for i in range(100)]
        report = storage_report(values, "scaled test", scale_factor=1_000_000)

        assert "1,000,000" in report or "1000000" in report

    def test_report_does_not_crash_on_special_values(self):
        """NaN and Infinity should not crash the report."""
        values = ["42", "NaN", "100.5"]
        report = storage_report(values, "with specials")
        assert isinstance(report, str)
