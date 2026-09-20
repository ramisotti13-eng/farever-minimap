#pragma once

// Pure, dependency-free builder for a per-character POI-progress profile key
// (the suffix in data/poi_done__<key>.json). Kept free of <windows.h> so it
// unit-tests with a plain cl invocation (see tools/test_poi_profile_key.cpp).

#include <cstdint>
#include <cstdio>
#include <string>

namespace farever {

// Build a filesystem-safe, collision-resistant key from a character name
// (UTF-8 bytes). The readable part keeps [A-Za-z0-9-], maps everything else
// to '_', and is capped at 40 chars; an 8-hex FNV-1a hash of the FULL raw
// name is appended so two names that sanitize to the same readable token
// (e.g. non-Latin names) still get distinct keys.
inline std::string poi_sanitize_profile_key(const std::string& name) {
    std::uint32_t h = 2166136261u;                 // FNV-1a 32-bit
    for (unsigned char c : name) { h ^= c; h *= 16777619u; }

    std::string readable;
    for (unsigned char c : name) {
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-';
        readable.push_back(safe ? static_cast<char>(c) : '_');
        if (readable.size() >= 40) break;
    }
    if (readable.empty()) readable = "char";

    char suf[9];
    std::snprintf(suf, sizeof(suf), "%08x", h);
    readable.push_back('_');
    readable += suf;
    return readable;
}

}  // namespace farever
