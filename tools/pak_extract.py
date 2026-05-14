"""Extract one or more files from a Heaps .pak by path.

Companion to pak_walk.py — once you know a file's path (and the walker
has confirmed its dataPosition / dataSize), this pulls the raw bytes
out and writes them to disk.

Important: the dataPosition recorded in each tree entry is an offset
into the file's data section, which begins right after the 4-byte
"DATA" marker that follows the header tree. We compute the absolute
file offset as:

    data_section_start = 12 + header_size + 4
    absolute_offset    = data_section_start + entry.dataPosition

Usage:
    python pak_extract.py <input.pak> <out_dir> <path_glob> [<path_glob> ...]

The path_glob is a simple `fnmatch` pattern against the slash-separated
entry path inside the pak. Globs that match many files are fine.
"""
from __future__ import annotations

import fnmatch
import struct
import sys
from pathlib import Path

from pak_walk import _R, read_entry  # type: ignore[import-not-found]


def open_pak(pak_path: Path):
    buf = pak_path.read_bytes()
    r = _R(buf)
    if r.s(3) != "PAK":
        raise ValueError("bad magic")
    _version = r.u8()
    header_size = r.i32()
    _data_size = r.i32()

    entries: list[tuple[str, int, int]] = []
    read_entry(r, "", entries)

    # The `header_size` field stores the absolute offset at which the
    # data section starts. The 4-byte "DATA" marker is immediately
    # before that, at offset `header_size - 4`.
    data_marker_pos = header_size - 4
    if buf[data_marker_pos : data_marker_pos + 4] != b"DATA":
        raise ValueError(
            f"expected DATA marker at offset {data_marker_pos}, got "
            f"{buf[data_marker_pos:data_marker_pos+4]!r}"
        )
    data_section_start = header_size
    return buf, entries, data_section_start


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    pak = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    patterns = sys.argv[3:]

    buf, entries, data_section_start = open_pak(pak)

    out_dir.mkdir(parents=True, exist_ok=True)
    n_written = 0
    for path, size, pos in entries:
        if not any(fnmatch.fnmatchcase(path, p) for p in patterns):
            continue
        abs_off = data_section_start + pos
        data = buf[abs_off : abs_off + size]
        if len(data) != size:
            print(f"!! short read for {path}: got {len(data)} of {size}", file=sys.stderr)
            continue
        dst = out_dir / path
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(data)
        n_written += 1
        print(f"  wrote {path} ({size:,} bytes)")
    print(f"# extracted {n_written} files -> {out_dir}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
