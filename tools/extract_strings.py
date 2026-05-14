"""Extract printable ASCII strings (>=4 chars) from a binary file.

Drop-in replacement for `strings -n 4 <file>`. We use this on hlboot.dat
to surface class names, field names, and string constants without
needing a full HashLink bytecode parser.

Usage:
    python extract_strings.py <input> <output>
"""
from __future__ import annotations

import sys
from pathlib import Path

MIN_LEN = 4
PRINTABLE = set(range(0x20, 0x7F))  # printable ASCII, excludes DEL


def extract(in_path: Path, out_path: Path) -> int:
    data = in_path.read_bytes()
    n = 0
    with out_path.open("w", encoding="utf-8", newline="\n") as out:
        run: list[int] = []
        for b in data:
            if b in PRINTABLE:
                run.append(b)
            else:
                if len(run) >= MIN_LEN:
                    out.write(bytes(run).decode("ascii"))
                    out.write("\n")
                    n += 1
                run.clear()
        if len(run) >= MIN_LEN:
            out.write(bytes(run).decode("ascii"))
            out.write("\n")
            n += 1
    return n


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    count = extract(src, dst)
    size_mb = src.stat().st_size / (1024 * 1024)
    print(f"Extracted {count:,} strings from {src.name} ({size_mb:.1f} MB) -> {dst}")
