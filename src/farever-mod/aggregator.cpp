// DamageEvent → SkillRow aggregator. Single-threaded; everything runs
// on the render thread alongside damage_tick. No combat-state tracking
// in this build - fight starts at the first damage event and runs
// until F9. (Hero pointer tracking via the alloc-hook landed in a
// follow-up commit; once we read Hero.combat_id we can auto-segment.)

#include "aggregator.h"
#include "damage.h"
#include "heal.h"
#include "hero_state.h"
#include "log.h"
#include "plugins.h"
#include "user_data.h"
#include "waypoints_json.h"   // wpjson::Parser for fight_history.json

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <unordered_map>

namespace farever {
namespace {

struct Fight {
    bool   have_first_damage = false;
    DWORD  first_damage_tick = 0;
    DWORD  last_damage_tick  = 0;
    std::unordered_map<std::string, SkillRow> rows;
    std::unordered_map<std::string, SkillRow> heal_rows;   // #57
    std::unordered_map<std::string, SkillRow> shield_rows; // #70
};

// We're out of combat once *both* signals say so for this long:
//   - no damage event for kDamageIdleMs
//   - Hero.isInCombat is false (or no hero lock)
// Either signal alone keeps the fight alive. That covers blocking /
// dodging without dealing damage, AoE pulls with kill-then-kite, etc.
constexpr DWORD kDamageIdleMs       = 5000;
constexpr DWORD kOutOfCombatGraceMs = 2000;

Fight                     g_fight;
AggSnapshot               g_snapshot{};
std::deque<FightLogEntry> g_history;   // newest at front
int                       g_next_fight_id = 1;
DWORD                     g_out_of_combat_since = 0;  // 0 = currently in combat (or never observed yet)

// Perf: publish() only rebuilds the expensive parts of g_snapshot when they
// actually change. Live rows (+ totals) change only when an event is recorded;
// the fight-history block changes only on seal/reset. The cheap scalars
// (elapsed/idle/dps/hps/in_combat) are refreshed every frame regardless.
bool                      g_rows_dirty    = true;
bool                      g_history_dirty = true;

// ---- fight_history.json persistence -----------------------------------

std::wstring fight_history_path() {
    return user_data_path(L"fight_history.json");
}

void write_row_json(std::ofstream& f, const SkillRow& r, bool last) {
    // Escape " and \ in the skill name.
    std::string nm;
    for (const char* c = r.skill; *c; ++c) {
        if (*c == '"' || *c == '\\') nm.push_back('\\');
        nm.push_back(*c);
    }
    char buf[320];
    std::snprintf(buf, sizeof(buf),
        "        { \"skill\": \"%s\", \"hit_count\": %d, "
        "\"total\": %g, \"max_hit\": %g, \"crit_count\": %d }%s\n",
        nm.c_str(), r.hit_count,
        r.total, r.max_hit, r.crit_count,
        last ? "" : ",");
    f << buf;
}

void save_fight_history() {
    std::ofstream f(fight_history_path());
    if (!f) { logf("aggregator: failed to open fight_history.json for write"); return; }
    f << "{\n  \"fights\": [\n";
    for (std::size_t i = 0; i < g_history.size(); ++i) {
        const FightLogEntry& fe = g_history[i];
        // Escape top_skill.
        std::string ts;
        for (const char* c = fe.top_skill; *c; ++c) {
            if (*c == '"' || *c == '\\') ts.push_back('\\');
            ts.push_back(*c);
        }
        f << "    {\n";
        char hdr[768];
        std::snprintf(hdr, sizeof(hdr),
            "      \"id\": %d, \"ended_unix_ms\": %lld,\n"
            "      \"duration_sec\": %g, \"total_damage\": %g, \"dps\": %g,\n"
            "      \"total_heal\": %g, \"hps\": %g, \"hit_count\": %d,\n"
            "      \"total_shield\": %g, \"sps\": %g,\n"
            "      \"top_skill\": \"%s\",\n",
            fe.id, (long long)fe.ended_unix_ms,
            fe.duration_sec, fe.total_damage, fe.dps,
            fe.total_heal, fe.hps, fe.hit_count,
            fe.total_shield, fe.sps,
            ts.c_str());
        f << hdr;
        f << "      \"rows\": [\n";
        for (std::size_t j = 0; j < fe.row_count; ++j)
            write_row_json(f, fe.rows[j], j + 1 == fe.row_count);
        f << "      ],\n";
        f << "      \"heal_rows\": [\n";
        for (std::size_t j = 0; j < fe.heal_row_count; ++j)
            write_row_json(f, fe.heal_rows[j], j + 1 == fe.heal_row_count);
        f << "      ],\n";
        f << "      \"shield_rows\": [\n";
        for (std::size_t j = 0; j < fe.shield_row_count; ++j)
            write_row_json(f, fe.shield_rows[j], j + 1 == fe.shield_row_count);
        f << "      ]\n    }";
        if (i + 1 < g_history.size()) f << ",";
        f << "\n";
    }
    f << "  ]\n}\n";
}

bool parse_row(wpjson::Parser& ps, SkillRow& r) {
    if (!ps.consume('{')) return false;
    r = SkillRow{};
    while (!ps.peek('}')) {
        std::string k;
        if (!ps.parse_string(k)) return false;
        if (!ps.consume(':'))    return false;
        if      (k == "skill") {
            std::string v;
            if (!ps.parse_string(v)) return false;
            wpjson::copy_into(r.skill, sizeof(r.skill), v);
        } else if (k == "hit_count") {
            double v; if (ps.parse_number(v)) r.hit_count = (std::int32_t)v;
            else ps.skip_value();
        } else if (k == "total") {
            double v; if (ps.parse_number(v)) r.total = v; else ps.skip_value();
        } else if (k == "max_hit") {
            double v; if (ps.parse_number(v)) r.max_hit = v; else ps.skip_value();
        } else if (k == "crit_count") {
            double v; if (ps.parse_number(v)) r.crit_count = (std::int32_t)v;
            else ps.skip_value();
        } else {
            ps.skip_value();
        }
        if (!ps.consume(',')) break;
    }
    return ps.consume('}');
}

bool parse_row_array(wpjson::Parser& ps, SkillRow* rows, std::size_t& count) {
    count = 0;
    if (!ps.consume('[')) return false;
    while (!ps.peek(']')) {
        if (count >= kMaxRows) {
            ps.skip_value();
        } else {
            if (!parse_row(ps, rows[count])) return false;
            ++count;
        }
        if (!ps.consume(',')) break;
    }
    return ps.consume(']');
}

void load_fight_history() {
    std::ifstream f(fight_history_path());
    if (!f) {
        logf("aggregator: no fight_history.json yet, starting fresh");
        return;
    }
    std::string text((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    wpjson::Parser ps{text.data(), text.data() + text.size()};
    if (!ps.consume('{')) return;
    bool found = false;
    while (!ps.peek('}')) {
        std::string k;
        if (!ps.parse_string(k)) return;
        if (!ps.consume(':'))    return;
        if (k == "fights") { found = true; break; }
        ps.skip_value();
        if (!ps.consume(',')) return;
    }
    if (!found || !ps.consume('[')) return;

    std::size_t loaded = 0;
    while (!ps.peek(']')) {
        if (!ps.consume('{')) break;
        FightLogEntry e{};
        bool ok = true;
        while (!ps.peek('}')) {
            std::string k;
            if (!ps.parse_string(k)) { ok = false; break; }
            if (!ps.consume(':'))    { ok = false; break; }
            if      (k == "id") {
                double v; if (ps.parse_number(v)) e.id = (int)v;
                else ps.skip_value();
            } else if (k == "ended_unix_ms") {
                double v; if (ps.parse_number(v)) e.ended_unix_ms = (std::int64_t)v;
                else ps.skip_value();
            } else if (k == "duration_sec") {
                double v; if (ps.parse_number(v)) e.duration_sec = v;
                else ps.skip_value();
            } else if (k == "total_damage") {
                double v; if (ps.parse_number(v)) e.total_damage = v;
                else ps.skip_value();
            } else if (k == "dps") {
                double v; if (ps.parse_number(v)) e.dps = v;
                else ps.skip_value();
            } else if (k == "total_heal") {
                double v; if (ps.parse_number(v)) e.total_heal = v;
                else ps.skip_value();
            } else if (k == "hps") {
                double v; if (ps.parse_number(v)) e.hps = v;
                else ps.skip_value();
            } else if (k == "total_shield") {
                double v; if (ps.parse_number(v)) e.total_shield = v;
                else ps.skip_value();
            } else if (k == "sps") {
                double v; if (ps.parse_number(v)) e.sps = v;
                else ps.skip_value();
            } else if (k == "hit_count") {
                double v; if (ps.parse_number(v)) e.hit_count = (std::int32_t)v;
                else ps.skip_value();
            } else if (k == "top_skill") {
                std::string v;
                if (ps.parse_string(v))
                    wpjson::copy_into(e.top_skill, sizeof(e.top_skill), v);
                else ps.skip_value();
            } else if (k == "rows") {
                if (!parse_row_array(ps, e.rows, e.row_count)) { ok = false; break; }
            } else if (k == "heal_rows") {
                if (!parse_row_array(ps, e.heal_rows, e.heal_row_count)) { ok = false; break; }
            } else if (k == "shield_rows") {
                if (!parse_row_array(ps, e.shield_rows, e.shield_row_count)) { ok = false; break; }
            } else {
                ps.skip_value();
            }
            if (!ps.consume(',')) break;
        }
        if (!ok || !ps.consume('}')) break;
        g_history.push_back(e);
        if (e.id >= g_next_fight_id) g_next_fight_id = e.id + 1;
        ++loaded;
        if (g_history.size() >= kFightHistoryMax) break;
        if (!ps.consume(',')) break;
    }
    if (loaded > 0) {
        g_history_dirty = true;
        logf("aggregator: loaded %zu fight(s) from fight_history.json", loaded);
    }
}

std::int64_t now_unix_ms() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    std::int64_t t = (static_cast<std::int64_t>(ft.dwHighDateTime) << 32) |
                      ft.dwLowDateTime;
    // FILETIME is 100-ns ticks since 1601-01-01; convert to ms since
    // Unix epoch (1970-01-01).
    return (t - 116444736000000000LL) / 10000LL;
}

// Build a FightLogEntry from the active fight and push it to the
// history ring. No-op if the fight had no events. We snapshot every
// SkillRow into the entry so the history-detail view can replay the
// full table later without re-running the aggregator.
void seal_active_fight(double elapsed_sec) {
    if (!g_fight.have_first_damage || g_fight.rows.empty()) return;

    FightLogEntry e{};
    e.id            = g_next_fight_id++;
    e.ended_unix_ms = now_unix_ms();
    e.duration_sec  = elapsed_sec > 0.001 ? elapsed_sec : 0.001;
    e.total_damage  = 0.0;
    e.hit_count     = 0;

    // Pull every skill row out, sort by total desc, copy into the
    // entry. SkillRow array is fixed-size so no allocations here.
    SkillRow temp[kMaxRows];
    std::size_t n = 0;
    for (const auto& kv : g_fight.rows) {
        if (n >= kMaxRows) break;
        temp[n++] = kv.second;
    }
    std::sort(temp, temp + n, [](const SkillRow& a, const SkillRow& b) {
        return a.total > b.total;
    });
    e.row_count = n;
    for (std::size_t i = 0; i < n; ++i) {
        e.rows[i]       = temp[i];
        e.total_damage += temp[i].total;
        e.hit_count    += temp[i].hit_count;
    }
    e.dps = e.total_damage / e.duration_sec;

    // #57 follow-up: also archive per-skill heal rows so the Fight-detail
    // window can switch its breakdown to HEAL, like the live meter does.
    SkillRow heal_temp[kMaxRows];
    std::size_t hn = 0;
    for (const auto& kv : g_fight.heal_rows) {
        if (hn >= kMaxRows) break;
        heal_temp[hn++] = kv.second;
    }
    std::sort(heal_temp, heal_temp + hn,
              [](const SkillRow& a, const SkillRow& b) {
                  return a.total > b.total;
              });
    e.heal_row_count = hn;
    double seal_heal = 0.0;
    for (std::size_t i = 0; i < hn; ++i) {
        e.heal_rows[i] = heal_temp[i];
        seal_heal     += heal_temp[i].total;
    }
    e.total_heal = seal_heal;
    e.hps        = seal_heal / e.duration_sec;

    // #70: per-status shield rows, same archival as heal.
    SkillRow shield_temp[kMaxRows];
    std::size_t sn = 0;
    for (const auto& kv : g_fight.shield_rows) {
        if (sn >= kMaxRows) break;
        shield_temp[sn++] = kv.second;
    }
    std::sort(shield_temp, shield_temp + sn,
              [](const SkillRow& a, const SkillRow& b) {
                  return a.total > b.total;
              });
    e.shield_row_count = sn;
    double seal_shield = 0.0;
    for (std::size_t i = 0; i < sn; ++i) {
        e.shield_rows[i] = shield_temp[i];
        seal_shield     += shield_temp[i].total;
    }
    e.total_shield = seal_shield;
    e.sps          = seal_shield / e.duration_sec;

    if (n > 0) std::strncpy(e.top_skill, temp[0].skill,
                            sizeof(e.top_skill) - 1);

    g_history.push_front(e);
    if (g_history.size() > kFightHistoryMax) g_history.pop_back();
    g_history_dirty = true;

    logf("aggregator: sealed fight #%d (%.1fs, %.0f dmg / %.0f heal, "
         "%.1f DPS / %.1f HPS, top=%s, %zu skills)",
         e.id, e.duration_sec, e.total_damage, e.total_heal, e.dps, e.hps,
         e.top_skill, e.row_count);

    save_fight_history();

    plugins_emit_fight_end(e.id, e.duration_sec, e.total_damage,
                           e.dps, e.top_skill);
}

void clear_active_fight() {
    g_fight.rows.clear();
    g_fight.heal_rows.clear();
    g_fight.shield_rows.clear();
    g_fight.have_first_damage = false;
    g_fight.first_damage_tick = 0;
    g_fight.last_damage_tick  = 0;
    g_out_of_combat_since     = 0;
    g_rows_dirty              = true;   // snapshot rows must clear next publish
}

// Base-attack chain collapse: "DS_Base_Attack_1/2/3/4" → "DS_Base_Attack".
// Only triggers on names containing "Base_Attack" to leave genuine combo
// skills (e.g. DS_Bladeleaf_Skill1) alone.
void normalise_skill(char* skill) {
    if (!std::strstr(skill, "Base_Attack")) return;
    std::size_t len = std::strlen(skill);
    while (len > 0 && skill[len - 1] >= '0' && skill[len - 1] <= '9') {
        skill[--len] = 0;
    }
    if (len > 0 && skill[len - 1] == '_') skill[--len] = 0;
}

void record(const DamageEvent& ev) {
    char skill[64];
    std::memcpy(skill, ev.skill, sizeof(skill));
    skill[sizeof(skill) - 1] = 0;
    normalise_skill(skill);

    DWORD now = GetTickCount();
    if (!g_fight.have_first_damage) {
        g_fight.have_first_damage = true;
        g_fight.first_damage_tick = now;
        logf("aggregator: combat START (%s)", skill);
        plugins_emit_fight_start(g_next_fight_id);
    }
    g_fight.last_damage_tick = now;

    plugins_emit_damage_dealt(skill, ev.damage,
                              ev.is_crit != 0, ev.is_kill != 0,
                              ev.target_name, ev.blocked);

    // One DamageEvent == one impact. ev.hit_count is the tick ordinal
    // (channels increment it 1, 2, 3 ...) so we never sum it; we just
    // count each event as a single hit.
    std::string key(skill);
    auto it = g_fight.rows.find(key);
    if (it == g_fight.rows.end()) {
        SkillRow r{};
        std::strncpy(r.skill, skill, sizeof(r.skill) - 1);
        r.hit_count  = 1;
        r.total      = ev.damage;
        r.max_hit    = ev.damage;
        r.crit_count = ev.is_crit ? 1 : 0;
        g_fight.rows.emplace(std::move(key), r);
    } else {
        SkillRow& r = it->second;
        r.hit_count += 1;
        r.total     += ev.damage;
        if (ev.damage > r.max_hit) r.max_hit = ev.damage;
        if (ev.is_crit) r.crit_count += 1;
    }
    g_rows_dirty = true;
}

void record_heal(const HealEvent& ev) {
    char skill[64];
    std::memcpy(skill, ev.skill, sizeof(skill));
    skill[sizeof(skill) - 1] = 0;
    normalise_skill(skill);

    // Plugins see every heal we deal, in or out of combat (#70). This stays
    // ahead of the in-combat gate below so the raw event stream is complete.
    plugins_emit_heal_dealt(skill, ev.heal, ev.is_crit != 0, ev.target_name);

    // #74: passive heal-over-time effects (e.g. Flame of Angol) tick every
    // ~2s, also while idle out of combat. Letting those open or sustain a
    // fight kept last_damage_tick fresh faster than the kDamageIdleMs window,
    // so the damage-idle timer never expired and the meter never sealed or
    // reset. Heals only feed the meter while the hero is actually in combat;
    // a real fight is in_combat=true so in-combat heals still count and still
    // sustain it.
    HeroSnapshot hs = hero_state_read();
    if (!(hs.locked && hs.in_combat)) return;

    DWORD now = GetTickCount();
    if (!g_fight.have_first_damage) {
        g_fight.have_first_damage = true;
        g_fight.first_damage_tick = now;
        logf("aggregator: combat START (heal %s)", skill);
        plugins_emit_fight_start(g_next_fight_id);
    }
    g_fight.last_damage_tick = now;   // in-combat heals sustain the fight too

    std::string key(skill);
    auto it = g_fight.heal_rows.find(key);
    if (it == g_fight.heal_rows.end()) {
        SkillRow r{};
        std::strncpy(r.skill, skill, sizeof(r.skill) - 1);
        r.hit_count  = 1;
        r.total      = ev.heal;
        r.max_hit    = ev.heal;
        r.crit_count = ev.is_crit ? 1 : 0;
        g_fight.heal_rows.emplace(std::move(key), r);
    } else {
        SkillRow& r = it->second;
        r.hit_count += 1;
        r.total     += ev.heal;
        if (ev.heal > r.max_hit) r.max_hit = ev.heal;
        if (ev.is_crit) r.crit_count += 1;
    }
    g_rows_dirty = true;
}

// #70: fold one shield application of `amount`, attributed to status `kind`,
// into the active fight's shield rows. Caller decides whether it counts (fight
// active + pre-pull / mid-fight rules below); this just records.
void record_shield_row(const char* kind_in, double amount) {
    char kind[64];
    std::strncpy(kind, (kind_in && *kind_in) ? kind_in : "Shield",
                 sizeof(kind) - 1);
    kind[sizeof(kind) - 1] = 0;

    std::string key(kind);
    auto it = g_fight.shield_rows.find(key);
    if (it == g_fight.shield_rows.end()) {
        SkillRow r{};
        std::strncpy(r.skill, kind, sizeof(r.skill) - 1);
        r.hit_count  = 1;
        r.total      = amount;
        r.max_hit    = amount;
        r.crit_count = 0;
        g_fight.shield_rows.emplace(std::move(key), r);
    } else {
        SkillRow& r = it->second;
        r.hit_count += 1;
        r.total     += amount;
        if (amount > r.max_hit) r.max_hit = amount;
    }
    g_rows_dirty = true;
}

// Perf: mutate g_snapshot in place. Cheap scalars refresh every frame; the
// expensive row sorts + totals only run when an event was recorded
// (g_rows_dirty), and the ~63KB fight-history block is copied only on
// seal/reset (g_history_dirty). Avoids rebuilding + copying a ~73KB struct
// every single frame.
void publish() {
    g_snapshot.have_fight     = g_fight.have_first_damage;
    g_snapshot.scanning_ready = true;   // hook source has no warm-up to gate on

    DWORD  now     = GetTickCount();
    double elapsed = 0.0;
    double idle    = 0.0;
    if (g_fight.have_first_damage) {
        elapsed = (now - g_fight.first_damage_tick) / 1000.0;
        if (elapsed < 0.001) elapsed = 0.001;
        idle = (now - g_fight.last_damage_tick) / 1000.0;
        if (idle < 0.0) idle = 0.0;
    }

    // Combat-state badge mirrors the seal logic: in combat if either signal.
    {
        HeroSnapshot hs           = hero_state_read();
        bool         damage_recent =
            g_fight.have_first_damage &&
            (now - g_fight.last_damage_tick) < kDamageIdleMs;
        bool         hero_active  = hs.locked && hs.in_combat;
        g_snapshot.in_combat = damage_recent || hero_active;
    }

    // Rows + totals only change when a new event was recorded.
    if (g_rows_dirty) {
        double total = 0.0;
        for (const auto& kv : g_fight.rows) total += kv.second.total;
        g_snapshot.total_damage = total;

        double total_heal = 0.0;
        for (const auto& kv : g_fight.heal_rows) total_heal += kv.second.total;
        g_snapshot.total_heal = total_heal;

        double total_shield = 0.0;
        for (const auto& kv : g_fight.shield_rows) total_shield += kv.second.total;
        g_snapshot.total_shield = total_shield;

        SkillRow temp[kMaxRows];
        std::size_t n = 0;
        for (const auto& kv : g_fight.rows) {
            if (n >= kMaxRows) break;
            temp[n++] = kv.second;
        }
        std::sort(temp, temp + n, [](const SkillRow& a, const SkillRow& b) {
            return a.total > b.total;
        });
        g_snapshot.row_count = n;
        for (std::size_t i = 0; i < n; ++i) g_snapshot.rows[i] = temp[i];

        SkillRow htemp[kMaxRows];
        std::size_t hn = 0;
        for (const auto& kv : g_fight.heal_rows) {
            if (hn >= kMaxRows) break;
            htemp[hn++] = kv.second;
        }
        std::sort(htemp, htemp + hn, [](const SkillRow& a, const SkillRow& b) {
            return a.total > b.total;
        });
        g_snapshot.heal_row_count = hn;
        for (std::size_t i = 0; i < hn; ++i) g_snapshot.heal_rows[i] = htemp[i];

        SkillRow stemp[kMaxRows];
        std::size_t sn = 0;
        for (const auto& kv : g_fight.shield_rows) {
            if (sn >= kMaxRows) break;
            stemp[sn++] = kv.second;
        }
        std::sort(stemp, stemp + sn, [](const SkillRow& a, const SkillRow& b) {
            return a.total > b.total;
        });
        g_snapshot.shield_row_count = sn;
        for (std::size_t i = 0; i < sn; ++i) g_snapshot.shield_rows[i] = stemp[i];

        g_rows_dirty = false;
    }

    // Scalars every frame (elapsed grows, so dps/hps move between events).
    g_snapshot.elapsed_sec = elapsed;
    g_snapshot.idle_sec    = idle;
    g_snapshot.dps = (elapsed > 0.001) ? (g_snapshot.total_damage / elapsed) : 0.0;
    g_snapshot.hps = (elapsed > 0.001) ? (g_snapshot.total_heal   / elapsed) : 0.0;
    g_snapshot.sps = (elapsed > 0.001) ? (g_snapshot.total_shield / elapsed) : 0.0;

    // History block only changes on seal/reset.
    if (g_history_dirty) {
        g_snapshot.history_count = g_history.size();
        for (std::size_t i = 0; i < g_snapshot.history_count; ++i)
            g_snapshot.history[i] = g_history[i];
        g_history_dirty = false;
    }
}

}  // namespace

void aggregator_tick() {
    static bool s_history_loaded = false;
    if (!s_history_loaded) {
        load_fight_history();
        s_history_loaded = true;
    }

    constexpr std::size_t kBatch = 64;
    DamageEvent batch[kBatch];
    while (true) {
        std::size_t n = damage_drain(batch, kBatch);
        if (n == 0) break;
        for (std::size_t i = 0; i < n; ++i) record(batch[i]);
        if (n < kBatch) break;
    }

    HealEvent hbatch[kBatch];
    while (true) {
        std::size_t n = heal_drain(hbatch, kBatch);
        if (n == 0) break;
        for (std::size_t i = 0; i < n; ++i) record_heal(hbatch[i]);
        if (n < kBatch) break;
    }

    // #70: shield tracking. The absorb lives on the hero's active statuses,
    // summed into HeroSnapshot.shield. Shields are typically cast pre-pull, so
    // an in-combat-at-the-instant gate (like heal) misses them: the cast lands
    // out of combat and the shield then just sits there, never re-edging. So
    // instead: credit whatever shield is up when a fight STARTS, and credit new
    // applications (0 -> positive) during an ongoing fight. Idle shields with no
    // following fight never get recorded, so no inflation. The plugin
    // shield_applied event still fires on every edge, in or out of combat.
    {
        HeroSnapshot hs  = hero_state_read();
        const char*  knd = "";
        double       top = 0.0;
        for (const auto& st : hs.statuses)
            if (st.shield_amount > top) { top = st.shield_amount;
                                          knd = st.kind.c_str(); }
        const char* kind = (knd && *knd) ? knd : "Shield";

        static double s_prev_shield     = 0.0;
        static bool   s_prev_have_fight = false;
        bool          fight_now = g_fight.have_first_damage;
        // A (re)cast is the only thing that makes the absorb go UP (it only
        // ever decreases as it soaks damage), so any rise = a fresh application,
        // including a refresh while the old shield is still partly up.
        bool          applied   = (hs.shield > s_prev_shield + 0.5);

        if (applied) plugins_emit_shield_applied(kind, hs.shield);

        if (fight_now && !s_prev_have_fight) {
            // Fight just started - credit a pre-pull shield that's still up.
            if (hs.shield > 0.5) record_shield_row(kind, hs.shield);
        } else if (fight_now && applied) {
            // Shield (re)applied during an ongoing fight.
            record_shield_row(kind, hs.shield);
        }

        s_prev_have_fight = fight_now;
        s_prev_shield     = hs.shield;
    }

    // Auto-seal once we've been *both* damage-idle AND
    // Hero.isInCombat=false continuously for the grace window.
    // Either signal alone keeps the fight open, so we don't drop a
    // legitimate fight just because the player dodged for 5 seconds
    // without dealing damage, and we don't drop it just because the
    // hero flag flickers off briefly during a target swap.
    if (g_fight.have_first_damage) {
        DWORD        now           = GetTickCount();
        HeroSnapshot hs            = hero_state_read();
        bool         damage_idle   =
            (now - g_fight.last_damage_tick) >= kDamageIdleMs;
        bool         hero_idle     = !hs.locked || !hs.in_combat;

        if (!damage_idle || !hero_idle) {
            g_out_of_combat_since = 0;
        } else {
            if (g_out_of_combat_since == 0) g_out_of_combat_since = now;
            if (now - g_out_of_combat_since >= kOutOfCombatGraceMs) {
                double elapsed =
                    (g_fight.last_damage_tick - g_fight.first_damage_tick) / 1000.0;
                logf("aggregator: combat END (damage-idle %ums, "
                     "hero %s) - sealing fight",
                     (unsigned)(now - g_fight.last_damage_tick),
                     hs.locked ? (hs.in_combat ? "in-combat" : "out") : "no-lock");
                seal_active_fight(elapsed);
                clear_active_fight();
            }
        }
    }

    publish();
}

const AggSnapshot& aggregator_snapshot() { return g_snapshot; }

// Lean scalar accessors so callers (the Lua plugin API) that only need one
// number don't copy the whole ~73KB snapshot. Render-thread only.
double aggregator_dps()          { return g_snapshot.dps; }
double aggregator_total_damage() { return g_snapshot.total_damage; }
double aggregator_elapsed_sec()  { return g_snapshot.elapsed_sec; }
bool   aggregator_in_combat()    { return g_snapshot.in_combat; }

void aggregator_reset() {
    // F9 - manual reset. If there is data, push it to history so it
    // isn't lost; then clear the live fight.
    if (g_fight.have_first_damage) {
        double elapsed =
            (GetTickCount() - g_fight.first_damage_tick) / 1000.0;
        seal_active_fight(elapsed);
    }
    clear_active_fight();
    publish();
    logf("aggregator: manual reset");
}

}  // namespace farever
