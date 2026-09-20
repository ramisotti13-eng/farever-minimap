#pragma once
// Pure, Win32-free matching helpers for the anti-cheat detector, split out so
// they can be unit-tested standalone (tools/test_anticheat_match.cpp).
#include <string>
#include <vector>
#include <cctype>

namespace farever {

// Copy of `s` lowercased (ASCII). Non-ASCII bytes pass through unchanged.
inline std::string ac_to_lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Known third-party anti-cheat module / process / driver name fragments,
// lowercase. Matched as substrings against loaded module names and running
// process names. Extend here when a new commercial AC shows up.
inline const std::vector<std::string>& ac_signatures() {
    static const std::vector<std::string> sigs = {
        "easyanticheat",  // EAC: EasyAntiCheat.dll / .exe / _EOS
        "beclient",       // BattlEye client
        "bedaisy",        // BattlEye driver
        "beservice",      // BattlEye service
        "gamemon",        // nProtect GameGuard
        "npggnt",         // nProtect GameGuard
        "xigncode",       // XignCode3
        "xhunter1",       // XignCode3 driver
        "vgk.sys",        // Riot Vanguard driver
        "vgc.exe",        // Riot Vanguard service
        "mhyprot",        // miHoYo anti-cheat driver
        "denuvo",         // Denuvo Anti-Cheat
        "ricochet",       // Activision Ricochet
    };
    return sigs;
}

// True if the (already lowercased) name contains any known AC signature.
inline bool ac_name_hits(const std::string& lower_name) {
    for (const std::string& sig : ac_signatures())
        if (lower_name.find(sig) != std::string::npos) return true;
    return false;
}

// True if the allowlist file text contains the build hash. The allowlist is
// read verbatim; we only need the hex digest to appear somewhere in it (the
// surrounding JSON / labels are ignored), so no JSON parser is needed. Caller
// passes both sides lowercased.
inline bool ac_allowlist_has(const std::string& file_contents_lower,
                             const std::string& hash_hex_lower) {
    if (hash_hex_lower.empty()) return false;
    return file_contents_lower.find(hash_hex_lower) != std::string::npos;
}

}  // namespace farever
