"""Walk a Heaps .pak file and list every entry.

Format (from upstream Heaps' hxd.fmt.pak.Reader.hx):

  Header:
    "PAK"           3 bytes
    version         u8
    headerSize      i32 LE      // size of the file-tree section
    dataSize        i32 LE
    <file tree>     headerSize bytes
    "DATA"          4 bytes
    <raw blob>

  File entry (recursive):
    nameLen         u8
    name            nameLen bytes (utf-8)
    flags           u8           // bit 0 = directory, bit 1 = double-precision data position
    if flags & 1 (directory):
      childCount    i32 LE
      children      childCount * file entry
    else (file):
      dataPosition  i32 LE       (or f64 LE if flags & 2)
      dataSize      i32 LE
      checksum      i32 LE

Output is a TSV: path<TAB>size<TAB>dataPosition (file leaves only),
sorted lexicographically.

Usage:
    python pak_walk.py <input.pak> [<output.tsv>]
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path


class _R:
    """Tiny binary reader over a bytes object with a moving cursor."""

    __slots__ = ("buf", "i")

    def __init__(self, buf: bytes, i: int = 0) -> None:
        self.buf = buf
        self.i = i

    def take(self, n: int) -> bytes:
        out = self.buf[self.i : self.i + n]
        if len(out) != n:
            raise EOFError(f"want {n} bytes at {self.i}, file is {len(self.buf)} bytes")
        self.i += n
        return out

    def u8(self) -> int:
        return self.take(1)[0]

    def i32(self) -> int:
        return struct.unpack("<i", self.take(4))[0]

    def f64(self) -> float:
        return struct.unpack("<d", self.take(8))[0]

    def s(self, n: int) -> str:
        return self.take(n).decode("utf-8", errors="replace")


def read_entry(r: _R, parent_path: str, out: list[tuple[str, int, int]]) -> None:
    name_len = r.u8()
    name = r.s(name_len)
    flags = r.u8()
    path = f"{parent_path}/{name}" if parent_path else name

    if flags & 1:  # directory
        n = r.i32()
        for _ in range(n):
            read_entry(r, path, out)
    else:
        if flags & 2:
            data_pos = int(r.f64())
        else:
            data_pos = r.i32()
        data_size = r.i32()
        _checksum = r.i32()  # noqa: F841 (we don't use it)
        out.append((path, data_size, data_pos))


def walk(pak_path: Path) -> tuple[list[tuple[str, int, int]], dict]:
    buf = pak_path.read_bytes()
    r = _R(buf)

    magic = r.s(3)
    if magic != "PAK":
        raise ValueError(f"bad magic: {magic!r}")
    version = r.u8()
    header_size = r.i32()
    data_size = r.i32()

    entries: list[tuple[str, int, int]] = []
    # Root is a single (implicit?) directory entry — try reading the
    # first file entry directly. If that fails, the format may wrap the
    # root in something else.
    read_entry(r, "", entries)

    meta = {
        "magic": magic,
        "version": version,
        "header_size": header_size,
        "data_size": data_size,
        "tree_bytes_consumed": r.i - 12,
        "file_total_bytes": len(buf),
        "entry_count": len(entries),
    }
    return entries, meta


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    pak = Path(sys.argv[1])
    out_path = Path(sys.argv[2]) if len(sys.argv) >= 3 else None

    entries, meta = walk(pak)

    print(f"# {pak.name}", file=sys.stderr)
    for k, v in meta.items():
        print(f"#   {k} = {v}", file=sys.stderr)

    entries.sort()
    lines = (f"{p}\t{sz}\t{pos}" for p, sz, pos in entries)
    if out_path:
        out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"# wrote {len(entries):,} entries -> {out_path}", file=sys.stderr)
    else:
        for line in lines:
            print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
