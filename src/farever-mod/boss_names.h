#pragma once

#include <cstdio>
#include <string>

namespace farever {

// A target is a boss when its HashLink class name is under ent.boss.*.
inline bool is_boss_type(const std::string& class_name) {
    static const char* kPrefix = "ent.boss.";
    return class_name.compare(0, 9, kPrefix) == 0;
}

// "ent.boss.Crabgantua" -> "Crabgantua"; anything without the prefix is
// returned unchanged. A few bosses carry an internal id that differs from
// the name players actually see in game, so those are remapped here.
// (#65: "Lady Bee" is internally "ent.boss.Mokshi"; "Honeyzabeth" is
// internally "ent.boss.Cleodora".)
inline std::string boss_display_name(const std::string& class_name) {
    std::string id = is_boss_type(class_name) ? class_name.substr(9) : class_name;
    if (id == "Mokshi")   return "Lady Bee";
    if (id == "Cleodora") return "Honeyzabeth";
    return id;
}

// Format a duration as M:SS.cc (centiseconds, rounded). Negatives / NaN clamp
// to "0:00.00".
inline std::string format_run_time(double seconds) {
    if (!(seconds > 0.0)) seconds = 0.0;          // also catches NaN
    long cs    = static_cast<long>(seconds * 100.0 + 0.5);   // total centisec
    long mins  = cs / 6000;
    long secs  = (cs / 100) % 60;
    long centi = cs % 100;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%ld:%02ld.%02ld", mins, secs, centi);
    return buf;
}

// Mean clear time. Guarded: no kills -> 0.
inline double boss_avg_s(double sum_s, int kills) {
    return kills > 0 ? sum_s / static_cast<double>(kills) : 0.0;
}

// Success rate as a percentage: kills / (kills + deaths) * 100. Guarded: no
// attempts -> 0.
inline double boss_win_pct(int kills, int deaths) {
    int attempts = kills + deaths;
    return attempts > 0 ? 100.0 * static_cast<double>(kills) / attempts : 0.0;
}

}  // namespace farever
