// Standalone unit test for the pure POI profile-key helper.
// Build (MSVC, from repo root):
//   cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /EHsc /std:c++17 /I src\farever-mod tools\test_poi_profile_key.cpp /Fe:test_ppk.exe'
//   .\test_ppk.exe
#include "poi_profile_key.h"
#include <cassert>
#include <cstdio>
#include <string>

using namespace farever;

int main() {
    // ASCII name: kept verbatim, plus an 8-hex hash suffix.
    std::string k = poi_sanitize_profile_key("Skydog");
    assert(k.rfind("Skydog_", 0) == 0);          // starts with "Skydog_"
    assert(k.size() == 6 + 1 + 8);               // name + '_' + 8 hex

    // Deterministic: same input -> same key.
    assert(poi_sanitize_profile_key("Skydog") == k);

    // Different names -> different keys.
    assert(poi_sanitize_profile_key("Skydog") != poi_sanitize_profile_key("Skydog2"));

    // Unsafe characters become '_' in the readable part, but the hash still
    // distinguishes the two distinct raw names.
    std::string a = poi_sanitize_profile_key("a b/c");
    assert(a.find('/') == std::string::npos && a.find(' ') == std::string::npos);
    assert(poi_sanitize_profile_key("\xe4\xb8\xad") !=
           poi_sanitize_profile_key("\xe6\x96\x87"));   // two different CJK names

    // Empty / all-unsafe name still yields a usable key.
    std::string e = poi_sanitize_profile_key("");
    assert(!e.empty());
    assert(e.rfind("char_", 0) == 0);

    // Long name is capped on the readable part (<= 40 chars) before the hash.
    std::string longn(100, 'x');
    std::string lk = poi_sanitize_profile_key(longn);
    assert(lk.size() <= 40 + 1 + 8);

    std::printf("poi_profile_key: all tests passed\n");
    return 0;
}
