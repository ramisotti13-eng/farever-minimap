"""Minimal parser for the Haxe `haxe.Serializer` text format.

Only the subset we encounter in Heaps' `.mogshape` files is supported —
nulls, bools, ints, floats, strings, arrays, class instances with
named fields, and the reference codes for both strings (R<n>) and
objects (r<n>).

This is NOT a complete implementation. We can extend it as new codes
show up in Farever data.

Reference: <https://github.com/HaxeFoundation/haxe/blob/development/std/haxe/Unserializer.hx>

Usage as a module:
    from hxserializer import unserialize
    obj = unserialize(open("Z1_Region.mogshape").read())

CLI:
    python hxserializer.py <file>  -> pretty-prints the parsed object
"""
from __future__ import annotations

import json
import sys
from dataclasses import dataclass, field
from pathlib import Path


class HxClass:
    __slots__ = ("name", "fields")

    def __init__(self, name: str) -> None:
        self.name = name
        self.fields: dict[str, object] = {}

    def __repr__(self) -> str:
        return f"HxClass({self.name!r}, {self.fields!r})"

    def to_jsonable(self):
        return {"__class__": self.name, **{k: _jsonable(v) for k, v in self.fields.items()}}


def _jsonable(v):
    if isinstance(v, HxClass):
        return v.to_jsonable()
    if isinstance(v, list):
        return [_jsonable(x) for x in v]
    if isinstance(v, dict):
        return {k: _jsonable(x) for k, x in v.items()}
    return v


@dataclass
class _Parser:
    s: str
    i: int = 0
    string_cache: list[str] = field(default_factory=list)
    object_cache: list[object] = field(default_factory=list)

    # ---- low-level helpers ------------------------------------------------

    def peek(self) -> str:
        return self.s[self.i]

    def eat(self) -> str:
        c = self.s[self.i]
        self.i += 1
        return c

    def read_int(self) -> int:
        start = self.i
        if self.peek() in "+-":
            self.i += 1
        while self.i < len(self.s) and self.s[self.i].isdigit():
            self.i += 1
        return int(self.s[start : self.i])

    def read_float(self) -> float:
        start = self.i
        # haxe writes 'd' followed by an optionally signed decimal number,
        # possibly with 'e' exponent
        if self.peek() in "+-":
            self.i += 1
        ok = "0123456789.eE+-"
        while self.i < len(self.s) and self.s[self.i] in ok:
            self.i += 1
        return float(self.s[start : self.i])

    def read_string_literal(self) -> str:
        # format: y<len>:<utf8 bytes encoded? actually url-encoded percents>
        length = self.read_int()
        assert self.peek() == ":", f"expected ':' at {self.i}, got {self.peek()!r}"
        self.i += 1
        # haxe.Serializer percent-encodes any byte > 0x7F. For our use the
        # data are plain ASCII class/field names, so just slice.
        raw = self.s[self.i : self.i + length]
        self.i += length
        # decode the URL-percent escapes if present
        if "%" in raw:
            try:
                from urllib.parse import unquote
                raw = unquote(raw)
            except Exception:
                pass
        self.string_cache.append(raw)
        return raw

    # ---- main dispatch ----------------------------------------------------

    def value(self):
        c = self.eat()
        if c == "n":
            return None
        if c == "z":
            return 0
        if c == "i":
            return self.read_int()
        if c == "d":
            return self.read_float()
        if c == "t":
            return True
        if c == "f":
            return False
        if c == "k":
            return float("nan")
        if c == "m":
            return float("-inf")
        if c == "p":
            return float("inf")
        if c == "y":
            return self.read_string_literal()
        if c == "R":
            idx = self.read_int()
            return self.string_cache[idx]
        if c == "r":
            idx = self.read_int()
            return self.object_cache[idx]
        if c == "a":
            return self.read_array()
        if c == "l":
            return self.read_list()
        if c == "c":
            return self.read_class()
        raise ValueError(f"unhandled type code {c!r} at offset {self.i - 1}")

    def read_array(self) -> list:
        arr: list = []
        self.object_cache.append(arr)
        while self.i < len(self.s) and self.peek() != "h":
            if self.peek() == "u":
                self.i += 1
                n_null = self.read_int()
                arr.extend([None] * n_null)
            else:
                arr.append(self.value())
        assert self.peek() == "h", f"expected 'h' at {self.i}"
        self.i += 1
        return arr

    def read_list(self) -> list:
        # same wire format as array per Serializer.hx
        return self.read_array()

    def read_class(self) -> HxClass:
        name = self.value()
        if not isinstance(name, str):
            raise ValueError(f"class name must be string, got {name!r}")
        inst = HxClass(name)
        self.object_cache.append(inst)
        while self.peek() != "g":
            key = self.value()
            if not isinstance(key, str):
                raise ValueError(f"field key must be string, got {key!r}")
            val = self.value()
            inst.fields[key] = val
        self.i += 1  # consume 'g'
        return inst


def unserialize(s: str):
    p = _Parser(s)
    return p.value()


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    src = Path(sys.argv[1])
    text = src.read_text(encoding="latin-1")  # raw byte-as-char
    obj = unserialize(text)
    print(json.dumps(_jsonable(obj), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
