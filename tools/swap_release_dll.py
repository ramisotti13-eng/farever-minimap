#!/usr/bin/env python3
"""Build a new release zip from the previous one by renaming the top
folder to the new version and swapping ONLY dinput8.dll. Everything else
(data/, etc.) is carried over byte-for-byte. Forward-slash separators are
enforced (#45 Proton). Run from the repo root.

Usage: python tools/swap_release_dll.py <old_ver> <new_ver> <new_dll_path>
"""
import hashlib
import sys
import zipfile

old_ver, new_ver, new_dll = sys.argv[1], sys.argv[2], sys.argv[3]
src_zip = f"farever-minimap-dps-{old_ver}.zip"
dst_zip = f"farever-minimap-dps-{new_ver}.zip"

with open(new_dll, "rb") as f:
    dll_bytes = f.read()
dll_sha = hashlib.sha256(dll_bytes).hexdigest()

zin = zipfile.ZipFile(src_zip, "r")
src_tops = sorted({n.split("/")[0] for n in zin.namelist() if n.strip()})
print("src top folder(s):", src_tops)

swapped = 0
with zipfile.ZipFile(dst_zip, "w", zipfile.ZIP_DEFLATED) as zout:
    for item in zin.infolist():
        name = item.filename
        newname = name.replace(
            f"farever-minimap-dps-{old_ver}",
            f"farever-minimap-dps-{new_ver}", 1).replace("\\", "/")
        if name.endswith("dinput8.dll"):
            data = dll_bytes
            swapped += 1
        else:
            data = zin.read(name)
        zi = zipfile.ZipInfo(newname, date_time=item.date_time)
        zi.compress_type = zipfile.ZIP_DEFLATED
        zi.external_attr = item.external_attr
        zout.writestr(zi, data)
zin.close()

# ---- verify ----
zc = zipfile.ZipFile(dst_zip)
names = zc.namelist()
backslash = [n for n in names if "\\" in n]
dst_tops = sorted({n.split("/")[0] for n in names if n.strip()})
zip_dll = [n for n in names if n.endswith("dinput8.dll")]
zip_dll_sha = hashlib.sha256(zc.read(zip_dll[0])).hexdigest() if zip_dll else "<none>"

print(f"dst zip          : {dst_zip}")
print(f"entries          : {len(names)}")
print(f"dst top folder(s): {dst_tops}")
print(f"backslash entries: {len(backslash)}")
print(f"dll entries      : {len(zip_dll)} (swapped {swapped})")
print(f"new dll sha256   : {dll_sha}")
print(f"zip dll sha256   : {zip_dll_sha}")
ok = (len(backslash) == 0 and len(dst_tops) == 1
      and dst_tops[0] == f"farever-minimap-dps-{new_ver}"
      and len(zip_dll) == 1 and zip_dll_sha == dll_sha)
print("VERIFY:", "OK" if ok else "FAILED")
sys.exit(0 if ok else 1)
