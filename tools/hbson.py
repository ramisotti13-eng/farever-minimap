"""Reader for Farever's HBSON prefab container.

Since the 2026-07 game builds, the level prefabs inside `res.map.pak`
(`Level/World/<World>.dat/gameplayData/*.prefab`) are no longer plain
JSON text; they are a binary "HBSON" encoding of the same tree. This
module decodes them back into the equivalent Python structures so the
existing extractors keep working.

Format (reverse-engineered against the 2026-07-17 build, 829/829 tile
prefabs of W1_Siagarta decode with zero trailing bytes):

    header   "HBSON" + u8 version (0)
    value    <tag:u8> <payload>

    tag  payload                 meaning
    ---- ----------------------- -------------------------------------
    0x00 -                       zero / absent number
    0x01 i8                      small int
    0x02 i32                     int
    0x03 f64                     float
    0x04 -                       false
    0x05 -                       true
    0x06 -                       null
    0x07 -                       empty object {}
    0x08 u8 count                object, count * (key string, value)
    0x09 i32 count               object with > 255 fields (by symmetry
                                 with 0x0d; not seen in the wild yet)
    0x0a string                  string value
    0x0b -                       empty array []
    0x0c u8 count                array, count * value
    0x0d i32 count               array with > 255 elements

    string  u32 LE. Bit 0x40000000 -> inline, length = v & 0x3fffffff,
            UTF-8 bytes follow, and the string is appended to the
            document's string table. Bit 0x80000000 -> inline as above
            but NOT added to the table (used for one-shot values such
            as asset paths). Neither bit set -> the value is an index
            into the string table.

The 0x40-vs-0x80 distinction matters: caching the 0x80 strings shifts
every later back-reference and silently turns object keys into asset
paths.
"""
from __future__ import annotations

import struct

MAGIC = b"HBSON"


class HbsonError(ValueError):
    pass


class _Reader:
    __slots__ = ("b", "p")

    def __init__(self, b: bytes) -> None:
        self.b = b
        self.p = 0

    def u8(self) -> int:
        v = self.b[self.p]
        self.p += 1
        return v

    def i8(self) -> int:
        v = struct.unpack_from("<b", self.b, self.p)[0]
        self.p += 1
        return v

    def i32(self) -> int:
        v = struct.unpack_from("<i", self.b, self.p)[0]
        self.p += 4
        return v

    def u32(self) -> int:
        v = struct.unpack_from("<I", self.b, self.p)[0]
        self.p += 4
        return v

    def f64(self) -> float:
        v = struct.unpack_from("<d", self.b, self.p)[0]
        self.p += 8
        return v

    def raw(self, n: int) -> bytes:
        v = self.b[self.p:self.p + n]
        if len(v) != n:
            raise HbsonError(f"truncated read of {n} at 0x{self.p:x}")
        self.p += n
        return v


def _read_str(r: _Reader, tbl: list[str]) -> str:
    v = r.u32()
    if v & 0xC0000000:
        s = r.raw(v & 0x3FFFFFFF).decode("utf-8")
        if v & 0x40000000:
            tbl.append(s)
        return s
    if 0 <= v < len(tbl):
        return tbl[v]
    raise HbsonError(f"string ref {v} out of range ({len(tbl)}) at 0x{r.p - 4:x}")


def _read_val(r: _Reader, tbl: list[str]):
    at = r.p
    t = r.u8()
    if t == 0x00:
        return 0
    if t == 0x01:
        return r.i8()
    if t == 0x02:
        return r.i32()
    if t == 0x03:
        return r.f64()
    if t == 0x04:
        return False
    if t == 0x05:
        return True
    if t == 0x06:
        return None
    if t == 0x07:
        return {}
    if t == 0x08 or t == 0x09:
        n = r.u8() if t == 0x08 else r.i32()
        o = {}
        for _ in range(n):
            k = _read_str(r, tbl)
            o[k] = _read_val(r, tbl)
        return o
    if t == 0x0A:
        return _read_str(r, tbl)
    if t == 0x0B:
        return []
    if t == 0x0C or t == 0x0D:
        n = r.u8() if t == 0x0C else r.i32()
        return [_read_val(r, tbl) for _ in range(n)]
    raise HbsonError(f"unknown tag 0x{t:02x} at 0x{at:x}")


def is_hbson(raw: bytes) -> bool:
    return raw[:5] == MAGIC


def loads(raw: bytes):
    """Decode one HBSON document. Raises HbsonError on a bad stream."""
    r = _Reader(raw)
    if r.raw(5) != MAGIC:
        raise HbsonError("not an HBSON document")
    r.u8()                       # version (0 on every build seen)
    v = _read_val(r, [])
    if r.p != len(raw):
        raise HbsonError(f"trailing bytes: consumed {r.p} of {len(raw)}")
    return v


def loads_any(raw: bytes):
    """Decode a prefab payload that may be either HBSON or plain JSON."""
    if is_hbson(raw):
        return loads(raw)
    import json
    return json.loads(raw.decode("utf-8"))
