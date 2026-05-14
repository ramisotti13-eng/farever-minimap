"""Minimal HashLink 1.15 bytecode parser for Farever's `hlboot.dat`.

Goal: read enough of the bytecode to recover the **class layout** for
any Haxe class compiled into the file. That gives us the EXACT memory
offset of every field, so the runtime memory probe knows where to look
for things like `ent.Hero.absPos`.

This is NOT a general-purpose bytecode tool. It parses only:
  - header + counts
  - int / float constants
  - strings + (v>=5) bytes section + (debug) files section
  - the full types table, with detailed parsing of HOBJ / HSTRUCT
    (class layouts) and length-skipping for all other kinds
  - globals table (type indices only)

Output is a JSON file with one entry per HOBJ/HSTRUCT type, each entry
listing fields with name, type kind, type description, and computed
byte offset within instances of the class.

Usage:
    python hlbc_parse.py <hlboot.dat>  [--out classes.json]
"""
from __future__ import annotations

import json
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

# ---------------------------------------------------------------- enums

HKIND_NAMES = [
    "HVOID", "HUI8", "HUI16", "HI32", "HI64", "HF32", "HF64", "HBOOL",
    "HBYTES", "HDYN", "HFUN", "HOBJ", "HARRAY", "HTYPE", "HREF",
    "HVIRTUAL", "HDYNOBJ", "HABSTRACT", "HENUM", "HNULL", "HMETHOD",
    "HSTRUCT", "HPACKED", "HGUID",
]
HVOID, HUI8, HUI16, HI32, HI64, HF32, HF64, HBOOL = range(8)
HBYTES, HDYN, HFUN, HOBJ, HARRAY, HTYPE, HREF, HVIRTUAL = range(8, 16)
HDYNOBJ, HABSTRACT, HENUM, HNULL, HMETHOD = range(16, 21)
HSTRUCT, HPACKED, HGUID = 21, 22, 23

# Pointer/scalar sizes for x64 platforms
KIND_SIZE = {
    HVOID:     0,
    HUI8:      1,
    HUI16:     2,
    HI32:      4,
    HI64:      8,
    HF32:      4,
    HF64:      8,
    HBOOL:     1,
    HBYTES:    8,
    HDYN:      8,
    HFUN:      8,
    HOBJ:      8,
    HARRAY:    8,
    HTYPE:     8,
    HREF:      8,
    HVIRTUAL:  8,
    HDYNOBJ:   8,
    HABSTRACT: 8,
    HENUM:     8,
    HNULL:     8,
    HMETHOD:   8,
    HSTRUCT:  -1,   # inlined; size computed from contained class
    HPACKED:  -1,   # inlined; size computed from contained struct
    HGUID:    16,
}
KIND_ALIGN = {
    HVOID: 1,
    HUI8: 1, HBOOL: 1,
    HUI16: 2,
    HI32: 4, HF32: 4,
    HI64: 8, HF64: 8, HGUID: 8,
    HBYTES: 8, HDYN: 8, HFUN: 8, HOBJ: 8, HARRAY: 8, HTYPE: 8,
    HREF: 8, HVIRTUAL: 8, HDYNOBJ: 8, HABSTRACT: 8, HENUM: 8, HNULL: 8,
    HMETHOD: 8,
    HSTRUCT: 8, HPACKED: 8,
}


# ---------------------------------------------------------------- reader

class R:
    """Cursor-based reader over the bytecode bytes."""

    __slots__ = ("buf", "i")

    def __init__(self, buf: bytes):
        self.buf = buf
        self.i = 0

    def u8(self) -> int:
        v = self.buf[self.i]
        self.i += 1
        return v

    def u16(self) -> int:
        v = struct.unpack_from("<H", self.buf, self.i)[0]
        self.i += 2
        return v

    def i32(self) -> int:
        v = struct.unpack_from("<i", self.buf, self.i)[0]
        self.i += 4
        return v

    def u32(self) -> int:
        v = struct.unpack_from("<I", self.buf, self.i)[0]
        self.i += 4
        return v

    def f64(self) -> float:
        v = struct.unpack_from("<d", self.buf, self.i)[0]
        self.i += 8
        return v

    def bytes_(self, n: int) -> bytes:
        out = self.buf[self.i : self.i + n]
        self.i += n
        return out

    # HashLink variable-length signed/unsigned index encoding.
    # See hashlink/src/code.c::hl_read_index
    def index(self) -> int:
        b = self.u8()
        if (b & 0x80) == 0:
            return b & 0x7F
        if (b & 0x40) == 0:
            v = self.u8() | ((b & 0x1F) << 8)
            return -v if (b & 0x20) else v
        c = self.u8()
        d = self.u8()
        e = self.u8()
        v = ((b & 0x1F) << 24) | (c << 16) | (d << 8) | e
        return -v if (b & 0x20) else v

    def uindex(self) -> int:
        v = self.index()
        if v < 0:
            raise ValueError(f"negative uindex {v} at offset {self.i}")
        return v


# ---------------------------------------------------------------- types

@dataclass
class TypeFun:
    nargs: int
    args: list[int]
    ret: int


@dataclass
class TypeObj:
    name_idx: int
    super_idx: int            # -1 if no super
    global_idx: int
    fields: list[tuple[int, int]]    # (name_idx, type_idx)
    protos: list[tuple[int, int, int]]  # (name_idx, findex, pindex)
    bindings: list[tuple[int, int]]  # (field_idx, fun_idx)


@dataclass
class TypeEnum:
    name_idx: int
    global_idx: int
    constructs: list[tuple[int, list[int]]]  # [(name_idx, [type_idx, ...]), ...]


@dataclass
class TypeVirtual:
    fields: list[tuple[int, int]]  # (name_idx, type_idx)


@dataclass
class TypeAbstract:
    name_idx: int


@dataclass
class Type:
    kind: int
    payload: object = None     # one of the above, or just a type-index for HREF/HNULL/HPACKED

    @property
    def kind_name(self) -> str:
        return HKIND_NAMES[self.kind] if self.kind < len(HKIND_NAMES) else f"H?{self.kind}"


# ---------------------------------------------------------------- parsing

@dataclass
class Code:
    version: int
    flags: int
    ints: list[int] = field(default_factory=list)
    floats: list[float] = field(default_factory=list)
    strings: list[str] = field(default_factory=list)
    bytes_data: list[bytes] = field(default_factory=list)
    debug_files: list[str] = field(default_factory=list)
    types: list[Type] = field(default_factory=list)
    globals_: list[int] = field(default_factory=list)


def read_strings(r: R, count: int) -> list[str]:
    """Read the strings table: i32 size, then `size` raw bytes,
    then for each of `count` strings a length varint pointing into
    the raw bytes."""
    size = r.i32()
    data = r.bytes_(size)
    strings: list[str] = []
    cursor = 0
    for _ in range(count):
        ln = r.uindex()
        s = data[cursor : cursor + ln]
        strings.append(s.decode("utf-8", errors="replace"))
        cursor += ln + 1
    return strings


def read_bytes_table(r: R, count: int) -> list[bytes]:
    size = r.i32()
    data = r.bytes_(size)
    positions = [r.uindex() for _ in range(count)]
    out: list[bytes] = []
    for i, pos in enumerate(positions):
        end = positions[i + 1] if i + 1 < count else size
        out.append(data[pos:end])
    return out


def read_type(r: R) -> Type:
    kind = r.u8()
    if kind == HFUN or kind == HMETHOD:
        nargs = r.u8()
        args = [r.index() for _ in range(nargs)]
        ret = r.index()
        return Type(kind, TypeFun(nargs, args, ret))
    if kind == HOBJ or kind == HSTRUCT:
        name_idx = r.index()
        super_idx = r.index()
        global_idx = r.uindex()
        nfields = r.uindex()
        nproto = r.uindex()
        nbindings = r.uindex()
        fields = [(r.index(), r.index()) for _ in range(nfields)]
        protos = [(r.index(), r.index(), r.index()) for _ in range(nproto)]
        bindings = [(r.uindex(), r.uindex()) for _ in range(nbindings)]
        return Type(kind, TypeObj(name_idx, super_idx, global_idx,
                                   fields, protos, bindings))
    if kind == HREF or kind == HNULL or kind == HPACKED:
        return Type(kind, r.index())   # type idx of the inner type
    if kind == HVIRTUAL:
        nfields = r.uindex()
        fields = [(r.index(), r.index()) for _ in range(nfields)]
        return Type(kind, TypeVirtual(fields))
    if kind == HABSTRACT:
        return Type(kind, TypeAbstract(r.index()))
    if kind == HENUM:
        name_idx = r.index()
        global_idx = r.uindex()
        nconstructs = r.uindex()
        constructs = []
        for _ in range(nconstructs):
            cname_idx = r.index()
            nparams = r.uindex()
            params = [r.index() for _ in range(nparams)]
            constructs.append((cname_idx, params))
        return Type(kind, TypeEnum(name_idx, 0 if global_idx == 0 else global_idx, constructs))
    # Scalar / pointer types with no payload
    if kind in (HVOID, HUI8, HUI16, HI32, HI64, HF32, HF64, HBOOL,
                HBYTES, HDYN, HARRAY, HTYPE, HDYNOBJ, HGUID):
        return Type(kind, None)
    raise ValueError(f"unhandled type kind {kind} at offset {r.i - 1}")


def parse(buf: bytes) -> Code:
    r = R(buf)
    if r.bytes_(3) != b"HLB":
        raise ValueError("bad magic")
    version = r.u8()
    flags = r.uindex()
    nints = r.uindex()
    nfloats = r.uindex()
    nstrings = r.uindex()
    nbytes = r.uindex() if version >= 5 else 0
    ntypes = r.uindex()
    nglobals = r.uindex()
    nnatives = r.uindex()
    nfunctions = r.uindex()
    nconstants = r.uindex() if version >= 4 else 0
    entrypoint = r.uindex()
    hasdebug = (flags & 1) != 0

    code = Code(version=version, flags=flags)

    # constants tables
    code.ints = [r.i32() for _ in range(nints)]
    code.floats = [r.f64() for _ in range(nfloats)]
    code.strings = read_strings(r, nstrings)
    if version >= 5 and nbytes > 0:
        code.bytes_data = read_bytes_table(r, nbytes)
    if hasdebug:
        ndebugfiles = r.uindex()
        code.debug_files = read_strings(r, ndebugfiles)

    # types — the part we care about
    for _ in range(ntypes):
        code.types.append(read_type(r))

    # globals — type indices
    for _ in range(nglobals):
        code.globals_.append(r.uindex())

    # we don't need natives / functions / constants for class layout

    return code


# ---------------------------------------------------------------- layout

def class_name(code: Code, t: Type) -> str:
    if t.kind in (HOBJ, HSTRUCT):
        return code.strings[t.payload.name_idx]
    if t.kind == HENUM:
        return code.strings[t.payload.name_idx]
    if t.kind == HABSTRACT:
        return code.strings[t.payload.name_idx]
    return t.kind_name


def field_type_desc(code: Code, type_idx: int) -> str:
    t = code.types[type_idx]
    if t.kind in (HOBJ, HSTRUCT, HENUM, HABSTRACT):
        return f"{t.kind_name}:{class_name(code, t)}"
    if t.kind == HREF:
        return f"HREF<{field_type_desc(code, t.payload)}>"
    if t.kind == HNULL:
        return f"HNULL<{field_type_desc(code, t.payload)}>"
    if t.kind == HPACKED:
        return f"HPACKED<{field_type_desc(code, t.payload)}>"
    if t.kind == HVIRTUAL:
        return f"HVIRTUAL[{len(t.payload.fields)} fields]"
    if t.kind in (HFUN, HMETHOD):
        return f"{t.kind_name}({len(t.payload.args)})"
    return t.kind_name


def field_size(code: Code, type_idx: int, _seen=None) -> int:
    """Return size in bytes of a field of this type. For inlined
    HSTRUCT / HPACKED, recursively compute the inner class' size."""
    if _seen is None:
        _seen = set()
    t = code.types[type_idx]
    k = t.kind
    if k != HSTRUCT and k != HPACKED:
        return KIND_SIZE[k]
    if type_idx in _seen:
        return 0  # break cycles
    _seen = _seen | {type_idx}
    if k == HPACKED:
        return field_size(code, t.payload, _seen)
    return class_instance_size(code, type_idx, _seen=_seen)


def field_align(code: Code, type_idx: int) -> int:
    t = code.types[type_idx]
    k = t.kind
    if k in (HSTRUCT, HPACKED):
        return 8
    return KIND_ALIGN.get(k, 8)


def class_layout(code: Code, type_idx: int, _seen=None) -> list[dict]:
    """Compute the layout of an HOBJ / HSTRUCT class as a list of dicts:
        { name, type_idx, type_desc, kind, kind_name, offset, size }

    Includes inherited fields from super classes first."""
    if _seen is None:
        _seen = set()
    if type_idx in _seen:
        return []
    _seen = _seen | {type_idx}
    t = code.types[type_idx]
    if t.kind not in (HOBJ, HSTRUCT):
        raise ValueError(f"type {type_idx} is {t.kind_name}, not an object")

    obj: TypeObj = t.payload
    fields = []
    # Object instances start with a pointer to the hl_type at offset 0.
    # Inherited fields then come next from the super class, then this
    # class's own fields. The super already covers its own header so
    # we only insert ours if no super.
    cursor: int
    if obj.super_idx >= 0:
        super_layout = class_layout(code, obj.super_idx, _seen=_seen)
        fields.extend(super_layout)
        # Resume at the end of the super's last field.
        if super_layout:
            last = super_layout[-1]
            cursor = last["offset"] + last["size"]
        else:
            cursor = 8  # hl_type pointer
    else:
        cursor = 8  # hl_type pointer at offset 0

    for name_idx, type_idx_field in obj.fields:
        sz = field_size(code, type_idx_field)
        al = field_align(code, type_idx_field)
        # Align cursor up
        if al > 1 and (cursor % al) != 0:
            cursor += al - (cursor % al)
        fields.append({
            "name": code.strings[name_idx],
            "type_idx": type_idx_field,
            "type_desc": field_type_desc(code, type_idx_field),
            "kind": code.types[type_idx_field].kind,
            "kind_name": code.types[type_idx_field].kind_name,
            "offset": cursor,
            "size": sz,
        })
        cursor += sz
    return fields


def class_instance_size(code: Code, type_idx: int, _seen=None) -> int:
    layout = class_layout(code, type_idx, _seen=_seen)
    if not layout:
        return 8
    last = layout[-1]
    end = last["offset"] + last["size"]
    # Round to 8-byte alignment for the next allocation
    if end % 8:
        end += 8 - (end % 8)
    return end


# ---------------------------------------------------------------- cli

def find_class_indices(code: Code, name: str) -> list[int]:
    out = []
    for i, t in enumerate(code.types):
        if t.kind in (HOBJ, HSTRUCT) and code.strings[t.payload.name_idx] == name:
            out.append(i)
    return out


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = Path(sys.argv[1])
    out_path = Path("classes.json")
    name_filter: str | None = None
    args = list(sys.argv[2:])
    while args:
        a = args.pop(0)
        if a == "--out" and args:
            out_path = Path(args.pop(0))
        elif a.startswith("--"):
            print(f"unknown flag: {a}")
            return 2
        else:
            name_filter = a

    print(f"reading {path} ({path.stat().st_size / 1024 / 1024:.1f} MB)...", file=sys.stderr)
    buf = path.read_bytes()
    code = parse(buf)
    print(f"  version={code.version}  flags=0x{code.flags:x}  "
          f"{len(code.strings)} strings, {len(code.types)} types, "
          f"{len(code.globals_)} globals", file=sys.stderr)

    if name_filter:
        # Print one class
        hits = find_class_indices(code, name_filter)
        if not hits:
            print(f"!! no class named {name_filter!r}", file=sys.stderr)
            return 1
        for idx in hits:
            layout = class_layout(code, idx)
            size = class_instance_size(code, idx)
            print(f"\n== class {name_filter}  (type #{idx}, instance size = {size} bytes) ==")
            print(f"  {'offset':>6} {'size':>5}  {'kind':<10} field")
            for f in layout:
                print(f"  {f['offset']:>6} {f['size']:>5}  {f['kind_name']:<10} {f['name']}  : {f['type_desc']}")
        return 0

    # Dump every HOBJ/HSTRUCT class to JSON
    classes = {}
    for i, t in enumerate(code.types):
        if t.kind not in (HOBJ, HSTRUCT):
            continue
        nm = code.strings[t.payload.name_idx]
        classes[nm] = {
            "type_idx": i,
            "kind": t.kind_name,
            "super_idx": t.payload.super_idx,
            "super": code.strings[code.types[t.payload.super_idx].payload.name_idx]
                     if t.payload.super_idx >= 0 and
                        code.types[t.payload.super_idx].kind in (HOBJ, HSTRUCT) else None,
            "instance_size": class_instance_size(code, i),
            "fields": class_layout(code, i),
        }
    out_path.write_text(json.dumps(classes, indent=2))
    print(f"  wrote {len(classes)} classes to {out_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
