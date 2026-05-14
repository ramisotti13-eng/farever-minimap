"""Live memory probe for Farever (HashLink VM process).

Goal: attach to the running game, locate the in-process anchor strings
we identified in M0 (`client.PlayerController`, `ent.Hero`, …), and
from there walk pointer chains to find the live player instance and
read its world position.

Status: Phase 1 — process attach + string scanning works fully offline
once the game is running. Phase 2 (pointer-walking + reading position)
is stubbed; needs validation against a live process.

REQUIRES the game to be running and (likely) this script to run as
Administrator so pymem can OpenProcess with PROCESS_VM_READ.

Usage:
    # 1) launch Farever, get to the main menu or in-game
    # 2) run as Administrator:
    python probe.py find       # scan + report addresses
    python probe.py modules    # list loaded modules
    python probe.py dump <hex_addr> [<len>]
    python probe.py refs <hex_addr>      # find pointers to that addr
    python probe.py hero                 # try to locate ent.Hero singleton (WIP)
    python probe.py loop                 # tail player position every 100 ms (WIP)
"""
from __future__ import annotations

import struct
import sys
import time
from dataclasses import dataclass

import pymem
import pymem.process
import pymem.ressources.structure as pmstruct
from pymem import Pymem

PROCESS_NAME = "Farever.exe"

# Strings we want to locate. These all appeared as ASCII in hlboot.dat
# and should be present in the live VM's string pool. Some have
# alternates so we try multiple anchors per concept.
ANCHORS: list[str] = [
    # singletons / hot paths
    "client.PlayerController",
    "client.GameCamera",
    "world.World",
    "ent.Hero",
    "st.Player",
    "ui.Hud",
    # debug source paths preserved in bytecode (unique-ish, less likely
    # to collide with random data)
    "src/client/PlayerController.hx",
    "src/client/GameCamera.hx",
    "src/world/World.hx",
    "src/ui/Hud.hx",
]


@dataclass
class Module:
    name: str
    base: int
    size: int
    end: int


def attach() -> tuple[Pymem, list[Module]]:
    pm = Pymem(PROCESS_NAME)
    mods: list[Module] = []
    for m in pymem.process.enum_process_module(pm.process_handle):
        name = m.name
        if isinstance(name, bytes):
            name = name.decode(errors="replace")
        mods.append(
            Module(
                name=name,
                base=m.lpBaseOfDll,
                size=m.SizeOfImage,
                end=m.lpBaseOfDll + m.SizeOfImage,
            )
        )
    mods.sort(key=lambda x: x.base)
    return pm, mods


# ----------------------------------------------------------------------
# Memory iteration: VirtualQuery walk over committed regions
# ----------------------------------------------------------------------


def iter_committed_regions(pm: Pymem):
    """Yield (base, size, protect) for every committed, readable region.

    pymem doesn't expose VirtualQueryEx directly; we use it via the
    underlying Windows API on the process handle.
    """
    import ctypes
    from ctypes import wintypes

    class MBI(ctypes.Structure):
        _fields_ = [
            ("BaseAddress", ctypes.c_void_p),
            ("AllocationBase", ctypes.c_void_p),
            ("AllocationProtect", wintypes.DWORD),
            ("__alignment1", wintypes.DWORD),
            ("RegionSize", ctypes.c_size_t),
            ("State", wintypes.DWORD),
            ("Protect", wintypes.DWORD),
            ("Type", wintypes.DWORD),
            ("__alignment2", wintypes.DWORD),
        ]

    VirtualQueryEx = ctypes.windll.kernel32.VirtualQueryEx
    VirtualQueryEx.argtypes = [
        wintypes.HANDLE,
        ctypes.c_void_p,
        ctypes.POINTER(MBI),
        ctypes.c_size_t,
    ]
    VirtualQueryEx.restype = ctypes.c_size_t

    MEM_COMMIT = 0x1000
    PAGE_NOACCESS = 0x01
    PAGE_GUARD = 0x100

    mbi = MBI()
    addr = 0
    max_addr = 0x7FFFFFFEFFFF  # user-mode limit on x64
    while addr < max_addr:
        ok = VirtualQueryEx(pm.process_handle, addr, ctypes.byref(mbi), ctypes.sizeof(mbi))
        if not ok:
            addr += 0x1000
            continue
        size = mbi.RegionSize
        if mbi.State == MEM_COMMIT and not (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)):
            yield mbi.BaseAddress or addr, size, mbi.Protect
        addr += size or 0x1000


def scan_region(pm: Pymem, base: int, size: int, needles: list[bytes]) -> dict[bytes, list[int]]:
    """Read region and return {needle: [absolute_addresses]} for each
    occurrence found."""
    hits: dict[bytes, list[int]] = {n: [] for n in needles}
    # cap region read size to avoid huge allocations
    LIMIT = 64 * 1024 * 1024
    off = 0
    while off < size:
        chunk_size = min(LIMIT, size - off)
        try:
            chunk = pm.read_bytes(base + off, chunk_size)
        except Exception:
            return hits  # skip unreadable region
        for n in needles:
            start = 0
            while True:
                k = chunk.find(n, start)
                if k < 0:
                    break
                hits[n].append(base + off + k)
                start = k + 1
        off += chunk_size
    return hits


# ----------------------------------------------------------------------
# Commands
# ----------------------------------------------------------------------


def cmd_modules() -> int:
    pm, mods = attach()
    print(f"PID {pm.process_id}")
    print(f"{'base':<18} {'size':>12}  name")
    for m in mods:
        print(f"0x{m.base:016x} {m.size:>12,}  {m.name}")
    return 0


def cmd_find(only: str | None = None) -> int:
    """Scan for each anchor string in *both* ASCII and UTF-16 LE forms.

    HashLink bytecode keeps two copies: a `strings[]` ASCII table for
    debug + bytecode constants, and a `ustrings[]` UTF-16 table for
    runtime class names (`hl_type_obj.name`). The UTF-16 hits are the
    ones we follow to type descriptors and live instances.
    """
    pm, mods = attach()

    anchors = ANCHORS if not only else [a for a in ANCHORS if only in a]

    # Build needle pairs (label, bytes); each anchor gets two variants.
    needles: list[tuple[str, str, bytes]] = []
    for a in anchors:
        needles.append((a, "asc", a.encode("ascii") + b"\x00"))
        needles.append((a, "u16", a.encode("utf-16-le") + b"\x00\x00"))
    pat_bytes = [n[2] for n in needles]

    all_hits: dict[tuple[str, str], list[int]] = {(a, k): [] for a, k, _ in needles}
    n_regions = 0
    t0 = time.time()
    print(f"scanning... pid={pm.process_id}", file=sys.stderr)
    for base, size, _prot in iter_committed_regions(pm):
        n_regions += 1
        r = scan_region(pm, base, size, pat_bytes)
        for (a, k, pat), addrs in zip(needles, r.values()):
            all_hits[(a, k)].extend(addrs)
    dt = time.time() - t0
    print(f"# scanned {n_regions} regions in {dt:.1f}s", file=sys.stderr)

    for (a, k), addrs in all_hits.items():
        if not addrs:
            continue
        print(f"\n[{a}] ({k})  {len(addrs)} hit(s)")
        for addr in addrs[:8]:
            mod = next((m for m in mods if m.base <= addr < m.end), None)
            tag = f"  in {mod.name}+0x{addr-mod.base:x}" if mod else ""
            print(f"  0x{addr:016x}{tag}")
        if len(addrs) > 8:
            print(f"  ... and {len(addrs)-8} more")
    return 0


def cmd_dump(addr_hex: str, length_hex: str = "0x80") -> int:
    pm, _ = attach()
    addr = int(addr_hex, 0)
    length = int(length_hex, 0)
    data = pm.read_bytes(addr, length)
    width = 16
    for i in range(0, len(data), width):
        chunk = data[i : i + width]
        hex_part = " ".join(f"{b:02x}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print(f"  0x{addr+i:016x}  {hex_part:<{width*3}}  {ascii_part}")
    return 0


def cmd_refs(target_hex: str) -> int:
    """Find 8-byte little-endian pointers to a given address in any
    committed region."""
    pm, mods = attach()
    target = int(target_hex, 0)
    needle = target.to_bytes(8, "little")
    refs: list[int] = []
    for base, size, _ in iter_committed_regions(pm):
        # only scan readable, RW-like regions where data structures live
        try:
            buf = pm.read_bytes(base, min(size, 64 * 1024 * 1024))
        except Exception:
            continue
        start = 0
        while True:
            k = buf.find(needle, start)
            if k < 0:
                break
            refs.append(base + k)
            start = k + 1
        # paginate for large regions — skip for now
    print(f"# {len(refs)} refs to 0x{target:x}")
    for a in refs[:32]:
        mod = next((m for m in mods if m.base <= a < m.end), None)
        tag = f"  in {mod.name}+0x{a-mod.base:x}" if mod else ""
        print(f"  0x{a:016x}{tag}")
    return 0


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cmd = sys.argv[1]
    try:
        if cmd == "modules":
            return cmd_modules()
        if cmd == "find":
            return cmd_find(sys.argv[2] if len(sys.argv) >= 3 else None)
        if cmd == "dump":
            return cmd_dump(*sys.argv[2:])
        if cmd == "refs":
            return cmd_refs(*sys.argv[2:])
        if cmd == "hero":
            print("not implemented yet — needs anchor offsets from cmd find first")
            return 1
        if cmd == "loop":
            print("not implemented yet — needs hero offset known first")
            return 1
        print(f"unknown command: {cmd}")
        return 2
    except pymem.exception.ProcessNotFound:
        print(f"!! {PROCESS_NAME} not running. Launch the game first.", file=sys.stderr)
        return 3
    except pymem.exception.CouldNotOpenProcess:
        print("!! could not open process — try running as Administrator.", file=sys.stderr)
        return 4


if __name__ == "__main__":
    sys.exit(main())
