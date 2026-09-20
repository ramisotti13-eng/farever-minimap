// Rare/sparkling-mob proximity alert. See mob_watch.h.
//
// Mirrors entity_state's alloc-watcher + per-tick rescan structure, but reads
// run on the hl_pump worker (HL-GC-invisible), the same safe thread the rest of
// the v0.6 state readers use. Foe fields (ent.GameObject / ent.Unit, offsets
// from tools/classes_v017.json):
//   +8    removed   HBOOL
//   +32   __uid     HI64
//   +152  posx      HF64  (posy +160, posz +168)
//   +544  dying     HBOOL
//   +592  kind      HOBJ:String   (ent.Unit)

#include "mob_watch.h"
#include "hero_state.h"
#include "hl_hook.h"
#include "mem_scan.h"
#include "user_data.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace farever {
namespace {

constexpr std::size_t OFF_GO_REMOVED = 8;     // unchanged
constexpr std::size_t OFF_GO_UID     = 32;    // unchanged
constexpr std::size_t OFF_GO_POSX    = 176;   // x,y,z contiguous f64 (was 152) [v018 +16]
constexpr std::size_t OFF_GO_DYING   = 560;   // was 552 [v023 +8]
constexpr std::size_t OFF_UNIT_KIND  = 600;  // ent.Unit.kind was 592 [v023 +8]
constexpr std::size_t OFF_STR_BYTES  = 8;
constexpr std::size_t OFF_STR_LEN    = 16;

constexpr std::size_t kMaxTerms   = 16;
constexpr std::size_t kMaxTermLen = 31;
constexpr double      kDefaultRange = 100.0;   // world units (~ meters)

// --- tracked foes (alloc watcher feeds this) --------------------------------
struct Tracked {
    std::uintptr_t ptr;
    std::uintptr_t expected_type;   // ent.Foe hl_type captured at alloc time
    int            retries;
    int            passes;          // completed scan passes, see the settle rule
    std::uint32_t  kind_hash;       // kind seen last pass, 0 = none yet
};

// FNV-1a over the kind string. Only ever compared against itself one pass
// later, so collision quality does not matter; it just has to change when the
// string changes.
std::uint32_t hash_kind(const char* s) {
    std::uint32_t h = 2166136261u;
    for (; *s; ++s) {
        h ^= static_cast<std::uint8_t>(*s);
        h *= 16777619u;
    }
    return h ? h : 1u;   // never 0, that means "nothing seen yet"
}
std::mutex            g_track_mu;
std::vector<Tracked>  g_tracked;

// --- config (Options panel writes, worker reads) ----------------------------
std::mutex                 g_cfg_mu;
std::atomic<bool>          g_enabled{false};
std::atomic<bool>          g_sparkling{false};  // #78: built-in "Sparkling" boss match
std::atomic<double>        g_range{kDefaultRange};
std::vector<std::string>   g_terms;             // display form (as typed)

// --- published snapshot (worker writes, render reads) -----------------------
std::mutex          g_pub_mu;
MobWatchSnapshot    g_published{};

// rising-edge tracking for the beep: uids currently in range last tick
std::unordered_set<std::int64_t> g_alerted;
std::atomic<bool>                g_beeping{false};

// SEH safety for the worker scan (mirrors heal_tick): a fault reading a foe
// that got freed/reused despite the type-anchor must never take down the
// hl_pump worker. Auto-disable after repeated trips.
std::atomic<bool>                g_tick_ok{true};
std::atomic<int>                 g_seh_fails{0};

// diagnostic: log the first ~80 distinct foe kinds seen in range so users
// learn the exact strings to watch for.
std::unordered_set<std::string>  g_seen_kinds;

std::wstring config_path() { return user_data_path(L"mob_watch.txt"); }

std::string to_lower(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

// Unit kinds are code identifiers ("Boar_Z2W", "Sprout_Rice_Z2W_Plantivore_2",
// "R1KoboldBoss_Sparkling"): a letter, then letters, digits or underscores.
// This is the second belt behind the settle rule in the scan loop. It rejects
// the engine strings that surface when a recycled GC block is read before its
// new owner has initialised it ("cascade 0", "Value(output.color,null)",
// "ToDTexture merge 2268 and 2268", "Sword_Guard_Basic_01.prefab", "c", "7").
// It deliberately does NOT claim to catch all of them: a plain lowercase word
// like "emissive" or "Lake" is a valid identifier shape and only the settle
// rule keeps that one out.
bool kind_is_identifier(const char* s) {
    if (!s) return false;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')))
        return false;
    std::size_t n = 0;
    for (; s[n]; ++n) {
        const char c = s[n];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return n >= 2;
}

bool read_kind_ascii(std::uintptr_t str_ptr, char* out, std::size_t cap) {
    out[0] = 0;
    if (!str_ptr || !mem_is_userland(str_ptr)) return false;
    std::uint64_t bytes_u64 = 0;
    std::int32_t  length    = 0;
    if (!mem_read_u64(str_ptr + OFF_STR_BYTES, &bytes_u64)) return false;
    if (!mem_is_userland(static_cast<std::uintptr_t>(bytes_u64))) return false;
    if (!mem_read_i32(str_ptr + OFF_STR_LEN, &length)) return false;
    if (length <= 0 || length > 63) return false;
    std::uint8_t buf[160];
    std::size_t  nb = static_cast<std::size_t>(length) * 2;
    if (nb > sizeof(buf)) return false;
    if (!mem_read_bytes(static_cast<std::uintptr_t>(bytes_u64), buf, nb))
        return false;
    std::size_t w = 0;
    for (int i = 0; i < length && w + 1 < cap; ++i) {
        std::uint16_t c = static_cast<std::uint16_t>(buf[i * 2]) |
                          (static_cast<std::uint16_t>(buf[i * 2 + 1]) << 8);
        if (c >= 128) return false;   // internal kind ids are ASCII
        out[w++] = static_cast<char>(c);
    }
    out[w] = 0;
    return w > 0;
}

void save_config_locked() {
    std::ofstream f(config_path(), std::ios::binary | std::ios::trunc);
    if (!f) { logf("mob_watch: cannot write config"); return; }
    f << "enabled "  << (g_enabled.load()   ? 1 : 0) << "\n";
    f << "sparkling " << (g_sparkling.load() ? 1 : 0) << "\n";
    f << "range "    << g_range.load() << "\n";
    for (const auto& t : g_terms) f << "term " << t << "\n";
}

void load_config() {
    std::ifstream f(config_path(), std::ios::binary);
    if (!f) return;
    std::lock_guard<std::mutex> lk(g_cfg_mu);
    g_terms.clear();
    std::string line;
    while (std::getline(f, line)) {
        // strip trailing CR (file may be CRLF)
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.rfind("enabled ", 0) == 0) {
            g_enabled.store(std::atoi(line.c_str() + 8) != 0);
        } else if (line.rfind("sparkling ", 0) == 0) {
            g_sparkling.store(std::atoi(line.c_str() + 10) != 0);
        } else if (line.rfind("range ", 0) == 0) {
            double r = std::atof(line.c_str() + 6);
            if (r >= 10.0 && r <= 1000.0) g_range.store(r);
        } else if (line.rfind("term ", 0) == 0) {
            std::string t = line.substr(5);
            if (!t.empty() && t.size() <= kMaxTermLen &&
                g_terms.size() < kMaxTerms)
                g_terms.push_back(t);
        }
    }
    logf("mob_watch: config loaded (enabled=%d sparkling=%d range=%.0f terms=%zu)",
         g_enabled.load() ? 1 : 0, g_sparkling.load() ? 1 : 0,
         g_range.load(), g_terms.size());
}

void fire_beep() {
    bool expected = false;
    if (!g_beeping.compare_exchange_strong(expected, true)) return;
    // Fire-and-forget so the 40 Hz worker tick never blocks on Beep().
    std::thread([] {
        Beep(988, 110);
        Beep(1319, 150);
        g_beeping.store(false);
    }).detach();
}

void on_foe_alloc(std::uintptr_t obj) {
    if (!obj) return;
    std::lock_guard<std::mutex> lk(g_track_mu);
    Tracked t{};
    t.ptr           = obj;
    t.expected_type = hl_hook_get_type(L"ent.Foe");
    g_tracked.push_back(t);
    if (g_tracked.size() > 8192) {
        g_tracked.erase(g_tracked.begin(),
                        g_tracked.begin() + (g_tracked.size() - 8192));
    }
}

}  // namespace

void mob_watch_start() {
    load_config();
    hl_hook_register(L"ent.Foe", on_foe_alloc);
    logf("mob_watch: watcher registered on ent.Foe");
}

static void mob_watch_tick_body() {
    // Cheap exit when off or nothing to match - still drains the tracked queue
    // implicitly on the next enabled tick (entries are re-validated then). The
    // built-in "Sparkling bosses" toggle (#78) keeps the scan running too.
    const bool sparkling = g_sparkling.load(std::memory_order_acquire);
    if (!g_enabled.load(std::memory_order_acquire) && !sparkling) {
        std::lock_guard<std::mutex> lk(g_pub_mu);
        g_published.enabled = false;
        g_published.count   = 0;
        return;
    }

    // Snapshot the config for this tick.
    std::vector<std::string> terms_lc;
    {
        std::lock_guard<std::mutex> lk(g_cfg_mu);
        terms_lc.reserve(g_terms.size());
        for (const auto& t : g_terms) terms_lc.push_back(to_lower(t));
    }
    const double range  = g_range.load();
    const double range2 = range * range;

    HeroSnapshot h = hero_state_read();
    if (!h.locked) {
        std::lock_guard<std::mutex> lk(g_pub_mu);
        g_published.enabled = true;
        g_published.count   = 0;
        g_alerted.clear();
        return;
    }
    // Note: we still scan + log in-range foe kinds even with NO watch terms, so
    // the user can switch the alert on, walk around, and read the exact names
    // out of the log before deciding what to watch. Empty terms simply produce
    // no matches (no banner / beep / marker).

    std::vector<Tracked> work;
    {
        std::lock_guard<std::mutex> lk(g_track_mu);
        work.swap(g_tracked);
    }

    std::vector<Tracked> retry;
    retry.reserve(work.size());
    MobHit hits[16];
    int    hit_count = 0;
    std::unordered_set<std::int64_t> seen_this_tick;

    for (Tracked t : work) {
        if (!mem_is_userland(t.ptr)) continue;

        // Type-anchor against the captured ent.Foe type to dodge GC slot reuse.
        if (t.expected_type) {
            std::uint64_t actual = 0;
            if (!mem_read_u64(t.ptr, &actual)) continue;
            if (static_cast<std::uintptr_t>(actual) != t.expected_type) continue;
        } else {
            // Type wasn't cached at alloc time. Learn it now and re-check this
            // object on the NEXT pass; reading it unvalidated here was the same
            // class of mistake as the settle bug below. If the type never gets
            // cached the retry cap drops the entry, which is the safe outcome.
            t.expected_type = hl_hook_get_type(L"ent.Foe");
            if (++t.retries < 60) retry.push_back(t);
            continue;
        }

        std::uint8_t removed = 0;
        if (!mem_read_u8(t.ptr + OFF_GO_REMOVED, &removed)) {
            if (++t.retries < 60) retry.push_back(t);
            continue;
        }
        if (removed) continue;   // gone, drop

        // Foe is alive; keep tracking it for next tick. Hold the slot so the
        // kind seen this pass can be carried into the next one (see below).
        ++t.passes;
        const std::size_t ri = retry.size();
        retry.push_back(t);

        // Settle rule. The alloc watcher hands us the object the instant
        // hl_alloc_obj returns, which is BEFORE the constructor has populated
        // it. hl_alloc_obj writes the type pointer first, so the type anchor
        // above cannot tell a fresh object from a settled one: `kind` still
        // holds the pointer the PREVIOUS occupant of that GC block left there,
        // and dereferencing it yields a perfectly valid but completely
        // unrelated engine string. That is how "cascade 0", "emissive" and
        // "ToDTexture merge 2268 and 2268" ended up logged as foe kinds and
        // could have tripped the rare-mob alert. Give every foe one full scan
        // pass before reading anything off it.
        if (t.passes < 2) continue;

        std::uint8_t dying = 0;
        if (mem_read_u8(t.ptr + OFF_GO_DYING, &dying) && dying) continue;

        double pos[3] = {0, 0, 0};
        if (!mem_read_bytes(t.ptr + OFF_GO_POSX, pos, sizeof(pos))) continue;
        double dx = pos[0] - h.x;
        double dy = pos[1] - h.y;
        double d2 = dx * dx + dy * dy;
        if (d2 > range2) continue;

        std::uint64_t kind_u64 = 0;
        if (!mem_read_u64(t.ptr + OFF_UNIT_KIND, &kind_u64)) continue;
        char kind[64];
        if (!read_kind_ascii(static_cast<std::uintptr_t>(kind_u64),
                             kind, sizeof(kind)))
            continue;
        // Second belt behind the settle rule: a slot recycled mid-read can
        // still hand back a string that is not a unit kind at all.
        if (!kind_is_identifier(kind)) continue;

        // Third belt, and the one that does not depend on timing. One settle
        // pass is only 25 ms at the 40 Hz worker tick, and during an alloc
        // burst (a region streaming in, frames running long) that is not always
        // enough: "DualSwords", which is an ItemTypeKind / ModelKind value and
        // not a unit kind at all, still got through with just the settle rule.
        // So require the SAME kind on two consecutive passes. A stale pointer
        // read before the constructor ran cannot survive that, because the
        // moment the constructor writes `kind` the value changes and the match
        // fails. Costs one extra tick of latency on a genuine new foe.
        const std::uint32_t kh = hash_kind(kind);
        const std::uint32_t prev = t.kind_hash;
        retry[ri].kind_hash = kh;
        if (prev != kh) continue;

        // Diagnostic: surface distinct in-range kinds so users know what to type.
        {
            std::string k(kind);
            if (g_seen_kinds.size() < 80 && g_seen_kinds.insert(k).second)
                logf("mob_watch: in-range foe kind '%s'", kind);
        }

        std::string kind_lc = to_lower(kind);
        const char* matched = nullptr;
        for (std::size_t i = 0; i < terms_lc.size(); ++i) {
            if (kind_lc.find(terms_lc[i]) != std::string::npos) {
                // Report the term in its display form.
                std::lock_guard<std::mutex> lk(g_cfg_mu);
                matched = (i < g_terms.size()) ? g_terms[i].c_str() : "";
                break;
            }
        }
        // #78: built-in match for sparkling boss variants (kinds like
        // "R1KoboldBoss_Sparkling"), independent of the user's watch terms.
        if (!matched && sparkling && kind_lc.find("sparkling") != std::string::npos)
            matched = "Sparkling";
        if (!matched) continue;

        std::int64_t uid = 0;
        {
            std::uint64_t u = 0;
            if (mem_read_u64(t.ptr + OFF_GO_UID, &u)) uid = static_cast<std::int64_t>(u);
        }
        seen_this_tick.insert(uid);

        if (hit_count < 16) {
            MobHit& m = hits[hit_count++];
            m.uid  = uid;
            std::strncpy(m.kind, kind, sizeof(m.kind) - 1);
            m.kind[sizeof(m.kind) - 1] = 0;
            std::strncpy(m.term, matched, sizeof(m.term) - 1);
            m.term[sizeof(m.term) - 1] = 0;
            m.x = pos[0]; m.y = pos[1]; m.z = pos[2];
            m.dist = (d2 > 0.0) ? std::sqrt(d2) : 0.0;
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_track_mu);
        // g_tracked now holds only foes allocated WHILE we scanned. Keep the
        // still-alive survivors first, then those new ones, and swap back in.
        if (!g_tracked.empty())
            retry.insert(retry.end(), g_tracked.begin(), g_tracked.end());
        g_tracked.swap(retry);
        if (g_tracked.size() > 8192)
            g_tracked.resize(8192);
    }

    // Rising edge: beep once for any matched uid that wasn't matched last tick.
    bool new_entry = false;
    for (const auto& uid : seen_this_tick) {
        if (g_alerted.find(uid) == g_alerted.end()) { new_entry = true; break; }
    }
    g_alerted.swap(seen_this_tick);
    if (new_entry) fire_beep();

    {
        std::lock_guard<std::mutex> lk(g_pub_mu);
        g_published.enabled = true;
        g_published.count   = hit_count;
        for (int i = 0; i < hit_count; ++i) g_published.hits[i] = hits[i];
    }
}

void mob_watch_tick() {
    if (!g_tick_ok.load(std::memory_order_acquire)) return;
    __try {
        mob_watch_tick_body();
        g_seh_fails.store(0, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        int f = g_seh_fails.fetch_add(1) + 1;
        logf("mob_watch: SEH trip #%d (code 0x%08lx)", f, GetExceptionCode());
        if (f >= 5) {
            g_tick_ok.store(false, std::memory_order_release);
            logf("mob_watch: auto-disabled after %d SEH trips", f);
        }
    }
}

const MobWatchSnapshot& mob_watch_read() {
    static thread_local MobWatchSnapshot s_local;
    std::lock_guard<std::mutex> lk(g_pub_mu);
    s_local = g_published;
    return s_local;
}

bool   mob_watch_enabled()        { return g_enabled.load(std::memory_order_acquire); }
bool   mob_watch_sparkling()      { return g_sparkling.load(std::memory_order_acquire); }
double mob_watch_range()          { return g_range.load(); }

void mob_watch_set_enabled(bool on) {
    g_enabled.store(on, std::memory_order_release);
    std::lock_guard<std::mutex> lk(g_cfg_mu);
    save_config_locked();
}

void mob_watch_set_sparkling(bool on) {
    g_sparkling.store(on, std::memory_order_release);
    std::lock_guard<std::mutex> lk(g_cfg_mu);
    save_config_locked();
}

void mob_watch_set_range(double r) {
    if (r < 10.0)   r = 10.0;
    if (r > 1000.0) r = 1000.0;
    g_range.store(r);
    std::lock_guard<std::mutex> lk(g_cfg_mu);
    save_config_locked();
}

std::vector<std::string> mob_watch_terms() {
    std::lock_guard<std::mutex> lk(g_cfg_mu);
    return g_terms;
}

void mob_watch_add_term(const char* term) {
    if (!term || !*term) return;
    std::string t(term);
    // trim surrounding whitespace
    while (!t.empty() && std::isspace((unsigned char)t.front())) t.erase(t.begin());
    while (!t.empty() && std::isspace((unsigned char)t.back()))  t.pop_back();
    if (t.empty() || t.size() > kMaxTermLen) return;
    std::lock_guard<std::mutex> lk(g_cfg_mu);
    if (g_terms.size() >= kMaxTerms) return;
    for (const auto& e : g_terms) if (e == t) return;   // dedupe
    g_terms.push_back(t);
    save_config_locked();
}

void mob_watch_remove_term(const char* term) {
    if (!term) return;
    std::lock_guard<std::mutex> lk(g_cfg_mu);
    for (auto it = g_terms.begin(); it != g_terms.end(); ++it) {
        if (*it == term) { g_terms.erase(it); save_config_locked(); return; }
    }
}

}  // namespace farever
