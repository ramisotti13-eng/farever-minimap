#pragma once

#include <cstdint>

namespace dpsmeter {

// Find the canonical hl_type* for a Haxe class by name.
//
// Anchor procedure (mirrors tools/find_type_by_name.py):
//   1. Search committed RW heap for the UTF-16 bytes of `class_name`.
//   2. For each hit (a name string), find u64 refs to it; that ref is
//      hl_type_obj.name (+16), so obj_ptr = ref - 16.
//   3. For each obj_ptr, find u64 refs to it; that ref is hl_type.obj
//      (+8), so type_ptr = ref - 8.
//   4. Verify u32 @ type_ptr == 11 (HOBJ kind). Return type_ptr.
//
// Cost: one-shot at init. ~hundreds of MB scanned linearly, expect
// 0.5–2 s on a modern machine. Caller should run this on a worker
// thread; don't block render/main.
//
// Returns true and stores the canonical hl_type* in `out_type_ptr` on
// success. Returns false if no canonical type was found (class not
// resident — usually means the world hasn't been entered yet).
bool type_anchor_find(const wchar_t* class_name, uintptr_t* out_type_ptr);

}  // namespace dpsmeter
