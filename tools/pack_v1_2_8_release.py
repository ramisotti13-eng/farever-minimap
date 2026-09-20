"""Build the v1.2.8 release zip from the v1.2.7 release zip.

v1.2.8 on top of v1.2.7:
  - offset migration for the 2026-09-11 game build (v0.2.4.29918). Every class
    deriving from st.DBState gained a `dbStatePlayer` field, so st.Loadout /
    st.Equipment / st.Inventory / st.Item (+Weapon/Gear/Armor) and
    st.player.Progress shift +8 from that point on. 13 constants migrated.
  - is_gear_item() matched the "st.item." namespace PREFIX and assumed
    everything in it carries level/upgradeLevel. st.item.Mastery reports its
    `mastery` field as the level and st.item.Recipe has neither (the object
    ends at afxUIDs), so inventory()/equipment() returned junk for those.
  - mob_watch read ent.Unit.kind off foes the alloc watcher handed over before
    their constructor had run, logging engine strings as monster names and
    potentially tripping the rare-mob alert.

THE WHITELIST SEMANTIC CHANGED, and that is the important part of this release.
`verified_builds.json` had been growing cumulatively and carried 10 hashes
spanning FOUR different memory layouts. It therefore meant "builds we have ever
seen", not "builds this DLL can actually read", and the version gate is a plain
hash match with no notion of layout. A v1.2.8 DLL would have injected happily
into a v023 client and read garbage silently, because that hash was still
listed. From this release the file lists only the builds whose layout matches
the offsets compiled into the DLL shipped beside it, so anyone on a different
build gets the honest "build not recognised" prompt instead of wrong numbers.

Swap vs the v1.2.7 zip:
  - dinput8.dll                 -> the v1.2.8 RelWithDebInfo build
  - data/verified_builds.json   -> release/data/verified_builds.json,
                                   trimmed to the single matching build

Nothing added, nothing removed. POI data, atlases, icons and the plugin guide
are untouched: no API changed and no asset moved. Top folder renamed,
forward-slash separators enforced (#45 Proton). Run from the repo root.
"""
import hashlib
import os
import zipfile

OLD_VER = "1.2.7"
NEW_VER = "1.2.8"
NEW_DLL = str(Path(__file__).resolve().parents[1] /
              "build" / "farever-mod" / "RelWithDebInfo" / "dinput8.dll")
NEW_VB  = str(Path(__file__).resolve().parents[1] /
              "release" / "data" / "verified_builds.json")

# hlboot.dat of the 2026-09-11 build (v0.2.4.29918). The ONLY build whose
# layout matches the offsets in this DLL.
NEW_HASH = "7d0c189415ad6832b11da0fafda29eaaa2a41f93330c4489942820b495e1df89"
EXPECT_HASHES = 1

# The build the user smoke-tested. Refuse to package anything else.
TESTED_DLL_SHA = os.environ.get("FAREVER_TESTED_DLL_SHA", "").strip().lower()

src_zip = f"farever-minimap-dps-{OLD_VER}.zip"
dst_zip = f"farever-minimap-dps-{NEW_VER}.zip"

with open(NEW_DLL, "rb") as f:
    dll_bytes = f.read()
dll_sha = hashlib.sha256(dll_bytes).hexdigest()
if TESTED_DLL_SHA and dll_sha != TESTED_DLL_SHA:
    print(f"!! REFUSING: built dll {dll_sha}")
    print(f"!!           tested    {TESTED_DLL_SHA}")
    print("!! Never ship a binary that was not smoke-tested.")
    raise SystemExit(2)

with open(NEW_VB, "rb") as f:
    vb_bytes = f.read()

old_top = f"farever-minimap-dps-{OLD_VER}"
new_top = f"farever-minimap-dps-{NEW_VER}"

zin = zipfile.ZipFile(src_zip, "r")
src_names = zin.namelist()
print("src zip          :", src_zip)
print("src top folder(s):", sorted({n.split("/")[0] for n in src_names if n.strip()}))
print("src entries      :", len(src_names))

swapped_dll = swapped_vb = 0
with zipfile.ZipFile(dst_zip, "w", zipfile.ZIP_DEFLATED) as zout:
    for item in zin.infolist():
        name = item.filename
        newname = name.replace(old_top, new_top, 1).replace("\\", "/")
        if name.endswith("dinput8.dll"):
            data = dll_bytes
            swapped_dll += 1
        elif name.endswith("data/verified_builds.json"):
            data = vb_bytes
            swapped_vb += 1
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
zip_vb = [n for n in names if n.endswith("verified_builds.json")]
zip_dll_sha = hashlib.sha256(zc.read(zip_dll[0])).hexdigest() if zip_dll else "<none>"
vb_text = zc.read(zip_vb[0]).decode("utf-8", "replace") if zip_vb else ""
zip_sha = hashlib.sha256(open(dst_zip, "rb").read()).hexdigest()

print()
print("dst zip          :", dst_zip)
print("dst top folder(s):", dst_tops)
print("dst entries      :", len(names))
print("swapped dll      :", swapped_dll)
print("swapped vb       :", swapped_vb)
print("backslash entries:", len(backslash))
print("dll sha256       :", zip_dll_sha)
print("zip sha256       :", zip_sha)
print("hashes in vb     :", vb_text.count('"hash"'))
print("new hash present :", NEW_HASH in vb_text)

problems = []
if len(names) != len(src_names):
    problems.append(f"entry count changed: {len(src_names)} -> {len(names)}")
if backslash:
    problems.append(f"{len(backslash)} backslash entries (breaks Proton, #45)")
if dst_tops != [new_top]:
    problems.append(f"top folder(s) = {dst_tops}, expected ['{new_top}']")
if swapped_dll != 1:
    problems.append(f"swapped {swapped_dll} dll(s), expected 1")
if swapped_vb != 1:
    problems.append(f"swapped {swapped_vb} verified_builds.json, expected 1")
if zip_dll_sha != dll_sha:
    problems.append("dll in zip does not match the built dll")
if vb_text.count('"hash"') != EXPECT_HASHES:
    problems.append(f"{vb_text.count(chr(34) + 'hash' + chr(34))} hashes in "
                    f"verified_builds.json, expected {EXPECT_HASHES}")
if NEW_HASH not in vb_text:
    problems.append("the current game build hash is missing from the whitelist")

print()
if problems:
    print("PROBLEMS:")
    for p in problems:
        print("   ", p)
    raise SystemExit(1)
print("OK")
