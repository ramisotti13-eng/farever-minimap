#pragma once

#include <cstdint>

namespace farever {

// Find the canonical hl_type* for a Haxe class by name.
//
// Anchor procedure:
//   1. Search committed RW heap for the UTF-16 bytes of `class_name`.
//   2. For each hit, find u64 refs to it; that ref is
//      hl_type_obj.name (+16), so obj_ptr = ref - 16.
//   3. For each obj_ptr, find u64 refs to it; that ref is hl_type.obj
//      (+8), so type_ptr = ref - 8.
//   4. Verify u32 @ type_ptr == 11 (HOBJ). Return type_ptr.
//
// The result is stable for the lifetime of the process. With the
// dinput8 proxy distribution we run this BEFORE the world is loaded,
// so the very first class allocations seed the search — we then hook
// hl_alloc_obj and never scan again.
bool type_anchor_find(const wchar_t* class_name, uintptr_t* out_type_ptr);

}  // namespace farever
