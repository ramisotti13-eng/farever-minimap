// Anti-cheat detector (issue #54). Fail-safe kill switch: if Farever ever
// ships a third-party anti-cheat, or runs an unverified game build, the mod
// refuses to inject and tells the user why.
//
// Two signals:
//   1. Signature scan - known AC module/process names (baked in).
//   2. Version gate    - SHA-256 of hlboot.dat vs data/verified_builds.json.
// On a trip the user is asked (Yes/No) whether to disable the mod or keep it
// running anyway at their own risk - for BOTH signals. The dev override
// data/ignore_anticheat.flag still skips the version-gate prompt entirely.

#include "anticheat.h"
#include "anticheat_match.h"
#include "log.h"
#include "user_data.h"

#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace farever {
namespace {

std::atomic<bool> g_disabled{false};
// Set once the user chose to keep running despite a signature hit, so the
// re-scan loop stops re-prompting for the rest of the session.
std::atomic<bool> g_signature_ack{false};

// Directory containing module `m` (NULL = the main exe), trailing backslash.
// Empty on error.
std::wstring module_dir(HMODULE m) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring s(path, n);
    auto pos = s.find_last_of(L'\\');
    if (pos == std::wstring::npos) return L"";
    s.resize(pos + 1);
    return s;
}

bool flag_present(HMODULE self, const wchar_t* name) {
    std::wstring p = module_dir(self);
    if (p.empty()) return false;
    p += L"data\\";
    p += name;
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Crude ASCII view of a wide name, for logging + matching. Module/process
// names are ASCII in practice.
std::string ascii_of(const wchar_t* w) {
    std::string s;
    for (; *w; ++w) s.push_back(static_cast<char>(*w & 0x7F));
    return s;
}

// Lowercased copy of a wide string (for case-insensitive path prefix tests).
std::wstring to_lower_w(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
    return s;
}

// #85: full image path of a process (lowercased), or "" if it can't be opened
// (e.g. a SYSTEM-owned service). Used to scope the process-name scan to the
// game folder so an unrelated game's anti-cheat service is not mistaken for
// Farever shipping one.
std::wstring process_image_path_lower(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[MAX_PATH];
    DWORD sz = MAX_PATH;
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf, &sz)) out.assign(buf, sz);
    CloseHandle(h);
    return to_lower_w(out);
}

// SHA-256 of the file at `path`, lowercase hex. Empty string on any failure.
std::string sha256_hex(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) != 0)
        return "";

    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out;
    do {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) break;
        std::vector<char> buf(1 << 16);
        bool ok = true;
        while (f) {
            f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            std::streamsize got = f.gcount();
            if (got > 0 &&
                BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()),
                               static_cast<ULONG>(got), 0) != 0) {
                ok = false;
                break;
            }
        }
        if (!ok) break;
        unsigned char digest[32];
        if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) break;
        static const char* hexd = "0123456789abcdef";
        out.reserve(64);
        for (unsigned char b : digest) {
            out.push_back(hexd[b >> 4]);
            out.push_back(hexd[b & 0xF]);
        }
    } while (false);

    if (hash) BCryptDestroyHash(hash);
    if (alg)  BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

// Scan our own loaded modules + all running processes for a known AC name.
// Returns the matched name (ASCII) or "" if clean. The dev-only
// data/test_anticheat_trip.flag forces a synthetic hit for local testing.
std::string scan_signatures(HMODULE self) {
    if (flag_present(self, L"test_anticheat_trip.flag"))
        return "test_anticheat_trip.flag (synthetic)";

    // Loaded modules in our process.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
            do {
                if (ac_name_hits(ac_to_lower(ascii_of(me.szModule)))) {
                    std::string hit = ascii_of(me.szModule);
                    CloseHandle(snap);
                    return hit;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }

    // Running processes: catch an anti-cheat companion service/launcher that
    // Farever itself ships, but NOT one belonging to a different game.
    // #85: EasyAntiCheat (and BattlEye, etc.) keep a system-wide background
    // service running for OTHER games (Fortnite, Apex, Rust, ...), so a plain
    // process-name match flagged "Farever appears to be running an anti-cheat"
    // on any PC that merely had such a game installed. Only treat a name hit as
    // real if the process image actually lives inside the Farever folder; a
    // real in-process anti-cheat is still caught by the module scan above.
    const std::wstring game_dir = to_lower_w(module_dir(nullptr));
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (!ac_name_hits(ac_to_lower(ascii_of(pe.szExeFile)))) continue;
                std::wstring img = process_image_path_lower(pe.th32ProcessID);
                if (game_dir.empty() || img.empty() ||
                    img.rfind(game_dir, 0) != 0) {
                    logf("anticheat: ignoring AC-named process '%s' outside the "
                         "game folder (likely another game's anti-cheat)",
                         ascii_of(pe.szExeFile).c_str());
                    continue;
                }
                std::string hit = ascii_of(pe.szExeFile);
                CloseHandle(snap);
                return hit;
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    return "";
}

// Hash hlboot.dat (next to Farever.exe) and check it against the allowlist.
// Returns true only if the build is verified.
bool version_verified(HMODULE self, std::string& out_hash) {
    out_hash.clear();
    std::wstring game = module_dir(nullptr);   // dir of Farever.exe
    if (game.empty()) {
        logf("anticheat: cannot resolve game dir; treating as UNVERIFIED");
        return false;
    }
    std::string hash = sha256_hex(game + L"hlboot.dat");
    out_hash = hash;
    if (hash.empty()) {
        logf("anticheat: hlboot.dat hash failed; treating as UNVERIFIED");
        return false;
    }
    logf("anticheat: hlboot.dat sha256=%s", hash.c_str());

    std::wstring listp = module_dir(self) + L"data\\verified_builds.json";
    std::ifstream lf(listp, std::ios::binary);
    if (!lf) {
        logf("anticheat: verified_builds.json missing; UNVERIFIED");
        return false;
    }
    std::string contents((std::istreambuf_iterator<char>(lf)),
                          std::istreambuf_iterator<char>());
    bool ok = ac_allowlist_has(ac_to_lower(contents), hash);
    logf("anticheat: build %s", ok ? "verified" : "NOT in allowlist");
    return ok;
}

// Ask the user whether to disable the mod. Yes = disable (default, the safe
// choice), No = keep running anyway at their own risk. Returns true if the
// mod should disable. Any failure to show the box defaults to disabling.
bool ask_disable(const wchar_t* text) {
    int r = MessageBoxW(nullptr, text, L"Farever Mod",
                        MB_YESNO | MB_ICONWARNING | MB_TOPMOST | MB_DEFBUTTON1);
    return r != IDNO;
}

const wchar_t* const kSigText =
    L"Farever appears to be running an anti-cheat now.\n\n"
    L"Continuing to run the mod could put your account at risk.\n\n"
    L"Disable the mod?\n"
    L"    Yes  =  disable it  (recommended)\n"
    L"    No   =  keep it running anyway  (at your own risk)";
const wchar_t* const kBuildText =
    L"Unknown Farever build.\n\n"
    L"This mod has not been verified against this game version.\n\n"
    L"Disable the mod?\n"
    L"    Yes  =  disable it  (recommended)\n"
    L"    No   =  keep it running anyway  (at your own risk)";

// #238: persisted per-build acknowledgements. data/acknowledged_builds.txt is
// a plain newline-delimited list of lowercase hlboot.dat hashes the user chose
// to "keep running" on. Internal, per-install, never shipped. Substring-matched
// (same as verified_builds.json) so no parsing is needed.
std::wstring ack_path(HMODULE self) {
    (void)self;   // ack list now lives in the per-user data dir, not the install
    return user_data_path(L"acknowledged_builds.txt");
}

bool build_acknowledged(HMODULE self, const std::string& hash) {
    if (hash.empty()) return false;
    std::ifstream f(ack_path(self), std::ios::binary);
    if (!f) return false;
    std::string contents((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    return ac_allowlist_has(ac_to_lower(contents), hash);
}

void acknowledge_build(HMODULE self, const std::string& hash) {
    if (hash.empty() || build_acknowledged(self, hash)) return;
    std::ofstream f(ack_path(self), std::ios::binary | std::ios::app);
    if (!f) {
        logf("anticheat: cannot write acknowledged_builds.txt");
        return;
    }
    f << hash << "\n";
    logf("anticheat: build %s acknowledged (persisted, won't re-prompt)",
         hash.c_str());
}

}  // namespace

bool anticheat_gate(HMODULE self) {
    // Signal 1: known AC present. Inform the user and let them decide.
    std::string sig = scan_signatures(self);
    if (!sig.empty()) {
        logf("anticheat: AC SIGNATURE detected: %s", sig.c_str());
        if (ask_disable(kSigText)) {
            logf("anticheat: user chose DISABLE -> no injection");
            g_disabled.store(true, std::memory_order_release);
            return false;
        }
        logf("anticheat: user chose CONTINUE ANYWAY despite AC signature");
        g_signature_ack.store(true, std::memory_order_release);
        return true;
    }

    // Signal 2: version gate. The dev override flag skips the prompt entirely.
    if (flag_present(self, L"ignore_anticheat.flag")) {
        logf("anticheat: ignore_anticheat.flag set -> version gate SKIPPED "
             "(dev override)");
        return true;
    }
    std::string build_hash;
    if (!version_verified(self, build_hash)) {
        // #238: if the user already chose to keep running on this exact build,
        // don't prompt again on every launch.
        if (build_acknowledged(self, build_hash)) {
            logf("anticheat: unverified build previously acknowledged -> "
                 "injecting");
            return true;
        }
        logf("anticheat: UNVERIFIED build");
        if (ask_disable(kBuildText)) {
            logf("anticheat: user chose DISABLE -> no injection");
            g_disabled.store(true, std::memory_order_release);
            return false;
        }
        logf("anticheat: user chose CONTINUE ANYWAY on unverified build");
        acknowledge_build(self, build_hash);   // remember for next launch
        return true;
    }
    logf("anticheat: gate clear (verified build, no AC signature)");
    return true;
}

void anticheat_start_rescans(HMODULE self) {
    std::thread([self]() {
        const int delays_ms[] = {5000, 5000, 10000, 10000, 15000, 15000};
        for (int d : delays_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(d));
            if (g_disabled.load(std::memory_order_acquire) ||
                g_signature_ack.load(std::memory_order_acquire))
                return;
            std::string sig = scan_signatures(self);
            if (!sig.empty()) {
                logf("anticheat: AC signature appeared mid-session: %s",
                     sig.c_str());
                if (ask_disable(kSigText)) {
                    logf("anticheat: user chose DISABLE mid-session -> going "
                         "passive (latch set, hooks left in place)");
                    g_disabled.store(true, std::memory_order_release);
                } else {
                    logf("anticheat: user chose CONTINUE ANYWAY mid-session");
                    g_signature_ack.store(true, std::memory_order_release);
                }
                return;
            }
        }
        logf("anticheat: re-scan window clear");
    }).detach();
}

bool anticheat_is_disabled() {
    return g_disabled.load(std::memory_order_acquire);
}

}  // namespace farever
