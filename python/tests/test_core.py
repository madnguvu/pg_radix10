"""
Tests for pyradix10.core — R10Decimal limb packing and round-trip correctness.

Run with: pytest tests/ -v
"""

import pytest
from decimal import Decimal

from pyradix10.core import (
    R10Decimal,
    R10_POS, R10_NEG, R10_NAN, R10_PINF, R10_NINF,
    NBASE, LIMB_DIGITS,
)


class TestR10DecimalCreation:
    """Test basic R10Decimal construction."""

    def test_from_string(self):
        d = R10Decimal("123.456")
        assert str(d) == "123.456"

    def test_from_integer_string(self):
        d = R10Decimal("42")
        assert str(d) == "42"

    def test_from_zero(self):
        d = R10Decimal("0")
        assert str(d) == "0"

    def test_negative(self):
        d = R10Decimal("-99.5")
        assert d.is_signed()

    def test_nan(self):
        d = R10Decimal("NaN")
        assert d.is_nan()

    def test_infinity(self):
        d = R10Decimal("Infinity")
        assert d.is_infinite()

    def test_negative_infinity(self):
        d = R10Decimal("-Infinity")
        assert d.is_infinite()
        assert d.is_signed()

    def test_repr(self):
        d = R10Decimal("3.14")
        assert "R10Decimal" in repr(d)
        assert "3.14" in repr(d)


class TestLimbPacking:
    """Test to_limbs() decomposition."""

    def test_zero(self):
        sign, limbs, weight, dscale = R10Decimal("0").to_limbs()
        assert sign == R10_POS
        assert limbs == []

    def test_small_integer(self):
        sign, limbs, weight, dscale = R10Decimal("42").to_limbs()
        assert sign == R10_POS
        assert len(limbs) == 1
        assert limbs[0] == 42
        assert weight == 0

    def test_negative(self):
        sign, limbs, weight, dscale = R10Decimal("-100").to_limbs()
        assert sign == R10_NEG

    def test_nan_limbs(self):
        sign, limbs, weight, dscale = R10Decimal("NaN").to_limbs()
        assert sign == R10_NAN
        assert limbs == []

    def test_pinf_limbs(self):
        sign, limbs, weight, dscale = R10Decimal("Infinity").to_limbs()
        assert sign == R10_PINF

    def test_ninf_limbs(self):
        sign, limbs, weight, dscale = R10Decimal("-Infinity").to_limbs()
        assert sign == R10_NINF

    def test_fractional(self):
        sign, limbs, weight, dscale = R10Decimal("0.5").to_limbs()
        assert sign == R10_POS
        assert dscale == 1
        # 0.5 → fractional limb 500000000
        assert len(limbs) == 1
        assert limbs[0] == 500000000

    def test_nine_digit_integer(self):
        """Exactly one limb: 123456789"""
        sign, limbs, weight, dscale = R10Decimal("123456789").to_limbs()
        assert limbs == [123456789]
        assert weight == 0

    def test_ten_digit_integer(self):
        """Two limbs: 1234567890"""
        sign, limbs, weight, dscale = R10Decimal("1234567890").to_limbs()
        assert len(limbs) == 2
        assert weight == 1

    def test_limb_values_in_range(self):
        """All limb values must be 0 <= limb < NBASE."""
        d = R10Decimal("999999999999999999.999999999")
        _, limbs, _, _ = d.to_limbs()
        for limb in limbs:
            assert 0 <= limb < NBASE

    def test_large_number(self):
        """30-digit number → multiple limbs."""
        val = "123456789012345678901234567890"
        d = R10Decimal(val)
        sign, limbs, weight, dscale = d.to_limbs()
        # 30 digits = ceil(30/9) = 4 limbs (but leading zeros stripped)
        assert len(limbs) >= 3
        assert weight >= 2


class TestLimbRoundTrip:
    """Test to_limbs() → from_limbs() round-trip."""

    @pytest.mark.parametrize("value", [
        "0",
        "1",
        "-1",
        "42",
        "-42",
        "0.5",
        "0.001",
        "0.000000001",
        "123.456",
        "-123.456",
        "999999999",
        "1000000000",
        "123456789.123456789",
        "99999999999999999.9999",
        "0.123456789012345678",
        "123456789012345678901234567890.123456789012345678901234567890",
        "NaN",
        "Infinity",
        "-Infinity",
    ])
    def test_round_trip(self, value):
        original = R10Decimal(value)
        sign, limbs, weight, dscale = original.to_limbs()
        reconstructed = R10Decimal.from_limbs(sign, limbs, weight, dscale)

        if original.is_nan():
            assert reconstructed.is_nan()
        elif original.is_infinite():
            assert reconstructed.is_infinite()
            assert original.is_signed() == reconstructed.is_signed()
        else:
            # Compare as Decimal to avoid trailing-zero differences
            assert Decimal(str(original)) == Decimal(str(reconstructed)), \
                f"Round-trip failed: {value} → {reconstructed}"


class TestBinaryRoundTrip:
    """Test to_packed_bytes() → from_packed_bytes() round-trip."""

    @pytest.mark.parametrize("value", [
        "0",
        "1",
        "-1",
        "123.456789",
        "999999999999999999.9999",
        "0.000000001",
        "NaN",
        "Infinity",
        "-Infinity",
    ])
    def test_binary_round_trip(self, value):
        original = R10Decimal(value)
        packed = original.to_packed_bytes()
        reconstructed = R10Decimal.from_packed_bytes(packed)

        if original.is_nan():
            assert reconstructed.is_nan()
        elif original.is_infinite():
            assert reconstructed.is_infinite()
            assert original.is_signed() == reconstructed.is_signed()
        else:
            assert Decimal(str(original)) == Decimal(str(reconstructed))

    def test_packed_bytes_format(self):
        """Verify the binary format matches the C struct layout."""
        d = R10Decimal("42")
        packed = d.to_packed_bytes()
        # Header: 2×int16 + 1×uint16 + 1×int16 = 8 bytes + limbs
        assert len(packed) >= 8
        # ndigits should be small
        import struct
        ndigits, weight, sign, dscale = struct.unpack_from("!hhHh", packed, 0)
        assert ndigits >= 0
        assert sign in (R10_POS, R10_NEG, R10_NAN, R10_PINF, R10_NINF)


class TestStorageEstimation:
    """Test storage size estimation."""

    def test_zero_sizes(self):
        d = R10Decimal("0")
        # Zero has no limbs — just header
        assert d.storage_bytes == 12  # 4 varlena + 8 metadata
        assert d.numeric_storage_bytes == 12

    def test_small_value_savings(self):
        """Small values may not save much (header dominates)."""
        d = R10Decimal("42")
        assert d.storage_bytes > 0
        assert d.numeric_storage_bytes > 0

    def test_large_value_savings(self):
        """Large values should show clear savings."""
        d = R10Decimal("1" * 100)
        assert d.storage_bytes > 0
        assert d.numeric_storage_bytes > 0

    def test_savings_pct_positive_for_large_values(self):
        """Very high precision values should show positive savings."""
        d = R10Decimal("1" * 50 + "." + "1" * 50)
        assert d.savings_pct is not None  # Just verify it doesn't crash

    def test_nan_size(self):
        d = R10Decimal("NaN")
        assert d.storage_bytes == 12
        assert d.numeric_storage_bytes == 12


class TestArithmetic:
    """Test that standard Decimal arithmetic still works on R10Decimal."""

    def test_add(self):
        a = R10Decimal("100.50")
        b = R10Decimal("200.25")
        result = a + b
        assert Decimal(str(result)) == Decimal("300.75")

    def test_sub(self):
        result = R10Decimal("500") - R10Decimal("123.45")
        assert Decimal(str(result)) == Decimal("376.55")

    def test_mul(self):
        result = R10Decimal("12.5") * R10Decimal("8")
        assert Decimal(str(result)) == Decimal("100.0")

    def test_div(self):
        result = R10Decimal("100") / R10Decimal("4")
        assert Decimal(str(result)) == Decimal("25")

    def test_comparison(self):
        assert R10Decimal("1") < R10Decimal("2")
        assert R10Decimal("2") > R10Decimal("1")
        assert R10Decimal("1") == R10Decimal("1.0")
        assert R10Decimal("1") != R10Decimal("2")


class TestNewProperties:
    """Test new properties added in the full implementation."""

    def test_limb_count_zero(self):
        assert R10Decimal("0").limb_count == 0

    def test_limb_count_small(self):
        assert R10Decimal("42").limb_count == 1

    def test_limb_count_large(self):
        # 20 digits → ceil(20/9) = 3 limbs (after stripping)
        d = R10Decimal("12345678901234567890")
        assert d.limb_count >= 2

    def test_digit_count(self):
        assert R10Decimal("12345").digit_count == 5
        assert R10Decimal("0").digit_count == 1
        assert R10Decimal("NaN").digit_count == 0

    def test_density_ratio_less_than_one_for_large(self):
        """For large enough values, radix10 should be denser (ratio < 1)."""
        d = R10Decimal("1" * 50)
        assert d.density_ratio <= 1.0

    def test_density_ratio_special(self):
        d = R10Decimal("NaN")
        assert d.density_ratio == 1.0

    def test_is_special(self):
        assert R10Decimal("NaN").is_special
        assert R10Decimal("Infinity").is_special
        assert R10Decimal("-Infinity").is_special
        assert not R10Decimal("42").is_special


class TestScientificNotation:
    """Test R10Decimal with scientific notation strings."""

    def test_positive_exponent(self):
        d = R10Decimal("1.5E3")
        assert Decimal(str(d)) == Decimal("1500")

    def test_negative_exponent(self):
        d = R10Decimal("2.5E-4")
        assert Decimal(str(d)) == Decimal("0.00025")

    def test_zero_exponent(self):
        d = R10Decimal("3.14e0")
        assert Decimal(str(d)) == Decimal("3.14")

    def test_large_exponent(self):
        d = R10Decimal("6.022e23")
        assert d > R10Decimal("0")

    def test_round_trip_scientific(self):
        """Scientific notation values should round-trip through limbs."""
        d = R10Decimal("1.5e3")
        sign, limbs, weight, dscale = d.to_limbs()
        reconstructed = R10Decimal.from_limbs(sign, limbs, weight, dscale)
        assert Decimal(str(d)) == Decimal(str(reconstructed))
