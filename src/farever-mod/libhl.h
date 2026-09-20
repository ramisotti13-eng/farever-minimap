#pragma once

#include <cstdint>

namespace farever {

// Resolved exports from libhl.dll. Populated by libhl_wait_and_resolve.
// All function pointers stay valid until the process exits - libhl is
// never unloaded.
struct LibHL {
    // The classic Boehm-style allocator. Called by the HashLink VM to
    // create every heap object: vdynamic* hl_alloc_obj(hl_type* t).
    // Hooking this gives us a callback for every object birth, which
    // is the foundation of the alloc-based event source.
    void* hl_alloc_obj      = nullptr;

    // Diagnostic exports we may want later - not hooked, just resolved.
    void* hl_alloc_dynamic  = nullptr;
    void* hl_alloc_dynobj   = nullptr;
    void* hl_alloc_array    = nullptr;
    void* hl_gc_dump_memory = nullptr;

    // Thread registration for the GC. Any thread that reads HashLink
    // heap pointers must be registered or the Boehm-style collector
    // can race with it (stop-the-world signalling is per registered
    // thread, and unregistered threads' stacks/registers aren't
    // scanned as roots). Signatures:
    //   void hl_register_thread(void* stack_top);
    //   void hl_unregister_thread();
    //   void hl_blocking(bool b);   // true=we're sleeping/I/O bound, GC can advance without us
    void* hl_register_thread   = nullptr;
    void* hl_unregister_thread = nullptr;
    void* hl_blocking          = nullptr;

    // Dynamic field accessors - used by the skill-icon resolver to walk
    // BaseSkill.inf.gfx (HVIRTUAL chain in HashLink terms). Signatures:
    //   void*  hl_dyn_getp(vdynamic* d, int hashed_name, hl_type* result_type);
    //   int    hl_dyn_geti(vdynamic* d, int hashed_name, hl_type* result_type);
    //   double hl_dyn_getd(vdynamic* d, int hashed_name);
    //   int    hl_hash_utf8(const char* utf8_name);
    //   int    hl_hash_gen(uchar* name, bool cache);  // not used today
    void* hl_dyn_getp = nullptr;
    void* hl_dyn_geti = nullptr;
    void* hl_dyn_getd = nullptr;
    void* hl_hash_utf8 = nullptr;

    // Lower-level field read. Direct access to an HOBJ's field table,
    // no dispatch through HVIRTUAL/HDYN coercion paths, no GC
    // allocation. Signature:
    //   vdynamic* hl_obj_field_fetch(vdynamic* obj, int hashed_name);
    // Returns NULL if obj is not HOBJ or field is not found. Companion
    // uses this in their worker-thread reads instead of hl_dyn_getp -
    // probably because the dispatch in dyn_getp can collide with
    // in-flight hxbit deserialization in ways the direct fetch can't.
    void* hl_obj_field_fetch = nullptr;

    // hxbit replicated entities are referenced by 32-bit UIDs (e.g.
    // ent.Hero.autoTarget). NetworkSerializer.refs is a hl_int64_map
    // whose values are the Serializable objects. HashLink exports its
    // int64-map ops as the `hl_hi64*` family (NOT `hl_int64_map_*`).
    // Signature:
    //   vdynamic *hl_hi64get(hl_hi64 *m, int64 key);
    void* hl_hi64get = nullptr;

    // Built-in type singletons exported by libhl. We need at least
    //   hlt_bytes, hlt_i32, hlt_dyn
    // to use as result_type when fetching fields.
    void* hlt_bytes = nullptr;
    void* hlt_i32   = nullptr;
    void* hlt_dyn   = nullptr;

    void* libhl_base        = nullptr;   // HMODULE
};

// Block on a background thread until libhl.dll is loaded into the
// process, then GetProcAddress every entry above. Returns once at
// least hl_alloc_obj is resolved, or never (if libhl never loads -
// which shouldn't happen in a Farever process).
bool libhl_wait_and_resolve(LibHL* out);

}  // namespace farever
