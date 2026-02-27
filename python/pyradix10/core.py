"""
pyradix10.core — R10Decimal: a Decimal subclass with base-10^9 limb packing.

Extends Python's `decimal.Decimal` with methods to convert to and from the
same uint32 limb representation used by the pg_radix10 PostgreSQL extension:

  to_limbs() / from_limbs()      — structural decomposition
  to_packed_bytes() / from_packed_bytes() — binary wire format
  storage_bytes / numeric_storage_bytes  — size estimation

The limb decomposition uses integer arithmetic (divmod), not string slicing.
"""

import struct
from decimal import Decimal, InvalidOperation
from typing import List, Tuple

# Must match src/radix10.h
NBASE = 1_000_000_000  # 10^9
LIMB_DIGITS = 9

# Sign constants (must match R10_POS/NEG/NAN/PINF/NINF in radix10.h)
R10_POS  = 0x0000
R10_NEG  = 0x4000
R10_NAN  = 0xC000
R10_PINF = 0xD000
R10_NINF = 0xF000

# Storage overhead: 4-byte varlena header + 8-byte metadata = 12 bytes
PG_VARLENA_HEADER = 4
LIMB_HEADER_SIZE = 8  # ndigits(2) + weight(2) + sign(2) + dscale(2)


def _int_to_limbs(n: int) -> List[int]:
    """Decompose a non-negative integer into base-10^9 limbs, MSD first."""
    if n == 0:
        return []
    limbs = []
    while n > 0:
        n, limb = divmod(n, NBASE)
        limbs.append(limb)
    limbs.reverse()
    return limbs


class R10Decimal(Decimal):
    """
    Drop-in Decimal replacement with base-10^9 limb packing.

    All standard Decimal arithmetic works unchanged.  Additional methods
    expose the packed representation for PostgreSQL wire protocol I/O
    and storage analysis.
    """

    def __new__(cls, value="0"):
        return super().__new__(cls, value)

    # ------------------------------------------------------------------
    # Limb decomposition
    # ------------------------------------------------------------------

    def to_limbs(self) -> Tuple[int, List[int], int, int]:
        """
        Decompose into (sign, limbs, weight, dscale).

        sign:   R10_POS | R10_NEG | R10_NAN | R10_PINF | R10_NINF
        limbs:  list of uint32 values (0..999_999_999), MSD first
        weight: base-10^9 exponent of the first limb
        dscale: display scale (digits after decimal point)
        """
        # Special values
        if self.is_nan():
            return (R10_NAN, [], 0, 0)
        if self.is_infinite():
            return (R10_PINF if self > 0 else R10_NINF, [], 0, 0)

        sign_flag = R10_NEG if self.is_signed() else R10_POS
        dec_sign, dec_digits, dec_exp = self.as_tuple()

        # Zero (possibly with scale)
        if not dec_digits or all(d == 0 for d in dec_digits):
            dscale = max(0, -dec_exp) if dec_exp < 0 else 0
            return (R10_POS, [], 0, dscale)

        # Reconstruct the unscaled integer and compute dscale
        #   value = ±coefficient × 10^exponent
        coefficient = int("".join(str(d) for d in dec_digits))

        if dec_exp >= 0:
            # Pure integer (possibly with trailing zeros from exponent)
            int_part = coefficient * (10 ** dec_exp)
            frac_part = 0
            dscale = 0
        else:
            # Has fractional digits
            dscale = -dec_exp
            power = 10 ** dscale
            # coefficient = int_part * power + frac_part
            int_part, frac_part = divmod(coefficient, power)

        # Decompose integer part into limbs
        int_limbs = _int_to_limbs(int_part)

        # Decompose fractional part into limbs
        # Fractional limbs: ceil(dscale / 9) limbs, each representing
        # 9 decimal digits of the fraction, left-aligned.
        frac_limbs = []
        if dscale > 0:
            n_frac_limbs = (dscale + LIMB_DIGITS - 1) // LIMB_DIGITS
            # Scale frac_part to fill exactly n_frac_limbs × 9 digits
            padded_digits = n_frac_limbs * LIMB_DIGITS
            frac_scaled = frac_part * (10 ** (padded_digits - dscale))
            frac_limbs = _int_to_limbs(frac_scaled)
            # Left-pad with zero limbs if needed
            while len(frac_limbs) < n_frac_limbs:
                frac_limbs.insert(0, 0)

        # Combine
        limbs = int_limbs + frac_limbs
        weight = len(int_limbs) - 1 if int_limbs else -1

        # Strip leading zero limbs (adjusting weight)
        while limbs and limbs[0] == 0:
            limbs.pop(0)
            weight -= 1

        # Strip trailing zero limbs
        while limbs and limbs[-1] == 0:
            limbs.pop()

        return (sign_flag, limbs, weight, dscale)

    @classmethod
    def from_limbs(cls, sign: int, limbs: List[int], weight: int,
                   dscale: int) -> "R10Decimal":
        """Reconstruct an R10Decimal from base-10^9 limbs (inverse of to_limbs)."""
        if sign == R10_NAN:  return cls("NaN")
        if sign == R10_PINF: return cls("Infinity")
        if sign == R10_NINF: return cls("-Infinity")

        if not limbs:
            return cls("0." + "0" * dscale) if dscale > 0 else cls("0")

        # Number of integer limbs = weight + 1
        n_int_limbs = weight + 1

        # Reconstruct integer part from limbs [0 .. n_int_limbs-1]
        int_val = 0
        for i in range(max(0, n_int_limbs)):
            if i < len(limbs):
                int_val = int_val * NBASE + limbs[i]
            else:
                int_val = int_val * NBASE  # trailing virtual zero limbs

        # Reconstruct fractional part from remaining limbs
        frac_val = 0
        frac_limb_count = 0
        start = max(0, n_int_limbs)
        for i in range(start, len(limbs)):
            frac_val = frac_val * NBASE + limbs[i]
            frac_limb_count += 1

        # Build the string
        int_str = str(int_val) if int_val > 0 or n_int_limbs > 0 else "0"

        if dscale > 0 and frac_limb_count > 0:
            # frac_val represents frac_limb_count × 9 digits, left-to-right
            frac_total_digits = frac_limb_count * LIMB_DIGITS
            frac_str = str(frac_val).zfill(frac_total_digits)
            # Trim or pad to exactly dscale
            frac_str = frac_str[:dscale].ljust(dscale, "0")
            result_str = f"{int_str}.{frac_str}"
        elif dscale > 0:
            result_str = f"{int_str}.{'0' * dscale}"
        else:
            result_str = int_str

        if sign == R10_NEG:
            result_str = "-" + result_str

        return cls(result_str)

    # ------------------------------------------------------------------
    # Binary serialization (pg_radix10 wire format)
    # ------------------------------------------------------------------

    def to_packed_bytes(self) -> bytes:
        """
        Serialize to pg_radix10's binary send/recv format.

        Format: [ndigits:i16][weight:i16][sign:u16][dscale:i16][limbs: n × u32]
        """
        sign, limbs, weight, dscale = self.to_limbs()
        header = struct.pack("!hhHh", len(limbs), weight, sign, dscale)
        body = struct.pack(f"!{len(limbs)}I", *limbs) if limbs else b""
        return header + body

    @classmethod
    def from_packed_bytes(cls, data: bytes) -> "R10Decimal":
        """Deserialize from pg_radix10's binary wire format."""
        ndigits, weight, sign, dscale = struct.unpack_from("!hhHh", data, 0)
        limbs = [struct.unpack_from("!I", data, 8 + i * 4)[0]
                 for i in range(ndigits)]
        return cls.from_limbs(sign, limbs, weight, dscale)

    # ------------------------------------------------------------------
    # Storage analysis
    # ------------------------------------------------------------------

    @property
    def storage_bytes(self) -> int:
        """Estimated on-disk size in pg_radix10 format (including varlena header)."""
        _, limbs, _, _ = self.to_limbs()
        return PG_VARLENA_HEADER + LIMB_HEADER_SIZE + len(limbs) * 4

    @property
    def numeric_storage_bytes(self) -> int:
        """
        Estimated on-disk size as core PostgreSQL NUMERIC.

        NUMERIC: base-10000, 4 digits per 2-byte limb, same 12-byte header.
        """
        if self.is_nan() or self.is_infinite():
            return 12

        _, dec_digits, dec_exp = self.as_tuple()
        if not dec_digits or dec_digits == (0,):
            return 12

        digit_count = len(dec_digits)
        if dec_exp > 0:
            digit_count += dec_exp

        num_limbs = (digit_count + 3) // 4
        return PG_VARLENA_HEADER + 8 + num_limbs * 2

    @property
    def savings_pct(self) -> float:
        """Storage savings: (1 − r10_size / numeric_size) × 100."""
        num_size = self.numeric_storage_bytes
        if num_size == 0:
            return 0.0
        return (1.0 - self.storage_bytes / num_size) * 100.0

    @property
    def is_special(self) -> bool:
        """True if NaN, +Inf, or −Inf."""
        return self.is_nan() or self.is_infinite()

    @property
    def limb_count(self) -> int:
        """Number of base-10^9 limbs needed."""
        _, limbs, _, _ = self.to_limbs()
        return len(limbs)

    @property
    def digit_count(self) -> int:
        """Total significant decimal digits."""
        if self.is_special:
            return 0
        _, digits, _ = self.as_tuple()
        return len(digits) if digits else 0

    @property
    def density_ratio(self) -> float:
        """Ratio of radix10 storage to NUMERIC storage (< 1.0 = denser)."""
        num_sz = self.numeric_storage_bytes
        if num_sz == 0:
            return 1.0
        return self.storage_bytes / num_sz

    def __repr__(self) -> str:
        return f"R10Decimal('{super().__str__()}')"
