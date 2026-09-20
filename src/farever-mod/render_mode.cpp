#include "render_mode.h"
#include "render_mode_parse.h"
#include "log.h"
#include "user_data.h"

#include <windows.h>
#include <commctrl.h>   // TaskDialogIndirect, TASKDIALOGCONFIG
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

// NOTE: TaskDialogIndirect is resolved DYNAMICALLY at runtime (see
// show_chooser_dialog), and we deliberately do NOT statically import it from
// comctl32. TaskDialogIndirect lives only in comctl32 v6 (side-by-side), so a
// static import (by ordinal) fails to bind against the base System32 comctl32
// (v5.82) without a v6 activation manifest -> the whole DLL fails to load
// before DllMain even runs. That is exactly the directx.hdll startup crash the
// first build of this feature hit. <commctrl.h> is still included for the
// TASKDIALOGCONFIG / TASKDIALOG_BUTTON / TDF_* declarations only (no import).

namespace farever {
namespace {

// Directory of THIS DLL (no HMODULE threading needed: locate ourselves by
// the address of a local function). Returns "" on failure.
std::wstring self_dir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&self_dir), &self)) {
        return L"";
    }
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring s(path);
    auto pos = s.find_last_of(L'\\');
    if (pos == std::wstring::npos) return L"";
    s.resize(pos);
    return s;
}

bool file_exists(const std::wstring& p) {
    return !p.empty() &&
           GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// True when running under Wine / Proton. Canonical detection: Wine exports
// wine_get_version from ntdll, which is absent on real Windows. Used to skip
// the first-run render-mode chooser (#66): its command-link buttons do not
// render usably under Wine, and DirectComposition (Compatibility) does not work
// under DXVK anyway, so game-swapchain (Fast) is the only working backend there.
// data/test_force_wine.flag forces this on so the path can be smoke-tested on
// real Windows.
bool running_under_wine() {
    if (file_exists(self_dir() + L"\\data\\test_force_wine.flag")) return true;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

std::string read_file_utf8(const std::wstring& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

void write_mode_file(bool game_swapchain) {
    std::ofstream f(user_data_path(L"render_mode.txt"),
                    std::ios::binary | std::ios::trunc);
    if (!f) { logf("render_mode: cannot write render_mode.txt"); return; }
    f << render_mode_token(game_swapchain) << "\n";
}

// Returns 1 = Fast (game_swapchain), 2 = Compatibility (dcomp), 0 = dismissed.
int show_chooser_dialog() {
    static const wchar_t* const kContent =
        L"You only set this once. Restart the game afterwards.\n\n"
        L"Note: Farever has no built-in Frame Generation. If you use it "
        L"(NVIDIA Smooth Motion, DLSS Frame Gen, Lossless Scaling), the cleanest "
        L"fix is to turn it OFF for Farever - it can crash the game.";

    const TASKDIALOG_BUTTON buttons[] = {
        { 101, L"Fast  -  normal mode for Windows (recommended)\n"
               L"Best performance. Almost everyone should pick this, including "
               L"all normal Windows players. (Also the right choice on "
               L"Linux / Steam Deck / Proton.)" },
        { 102, L"Compatibility  -  only if you use Frame Generation\n"
               L"Pick this ONLY if you keep Frame Generation turned on (NVIDIA "
               L"Smooth Motion, DLSS Frame Gen, Lossless Scaling). Slower, but "
               L"stops the crash. If you do not use Frame Gen, choose Fast." },
    };

    TASKDIALOGCONFIG cfg = {};
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = nullptr;
    cfg.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    cfg.pszWindowTitle = L"Farever Mod";
    cfg.pszMainIcon = TD_INFORMATION_ICON;
    cfg.pszMainInstruction = L"Choose how the overlay is drawn";
    cfg.pszContent = kContent;
    cfg.pButtons = buttons;
    cfg.cButtons = ARRAYSIZE(buttons);
    cfg.nDefaultButton = 101;

    // Resolve TaskDialogIndirect at runtime so we never statically depend on
    // comctl32 v6 (a static import would stop the DLL loading at all). If the
    // process has no v6 comctl32 active, GetProcAddress returns null and we
    // drop to the plain MessageBox below.
    int pressed = 0;
    HRESULT hr = E_NOTIMPL;
    HMODULE comctl = LoadLibraryW(L"comctl32.dll");
    if (comctl) {
        using PFN_TDI =
            HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
        auto pTaskDialogIndirect = reinterpret_cast<PFN_TDI>(
            GetProcAddress(comctl, "TaskDialogIndirect"));
        if (pTaskDialogIndirect)
            hr = pTaskDialogIndirect(&cfg, &pressed, nullptr, nullptr);
    }
    if (FAILED(hr)) {
        logf("render_mode: TaskDialog unavailable (0x%08lx), MessageBox "
             "fallback", static_cast<unsigned long>(hr));
        int r = MessageBoxW(
            nullptr,
            L"How should the overlay be drawn? You only set this once. "
            L"Restart the game afterwards.\n\n"
            L"YES = Fast  (normal mode for Windows, recommended)\n"
            L"Best performance. Almost everyone should pick this, including all "
            L"normal Windows players. (Also the right choice on Linux/Proton.)\n\n"
            L"NO = Compatibility  (only if you use Frame Generation)\n"
            L"Pick this ONLY if you keep Frame Generation on (NVIDIA Smooth "
            L"Motion, DLSS Frame Gen, Lossless Scaling). Slower, but stops the "
            L"crash. If you do not use Frame Gen, choose Fast (Yes).\n\n"
            L"Tip: Farever has no built-in Frame Generation - the cleanest fix "
            L"is to turn it OFF for Farever.",
            L"Farever Mod - choose render mode",
            MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST | MB_DEFBUTTON1);
        if (r == IDYES) return 1;
        if (r == IDNO)  return 2;
        return 0;
    }
    if (pressed == 101) return 1;
    if (pressed == 102) return 2;
    return 0;  // IDCANCEL / closed via X
}

}  // namespace

RenderModeResolution render_mode_resolve() {
    std::wstring dir = self_dir();

    // 1. Power-user override: force_dcomp.flag always wins, no prompt.
    if (file_exists(dir + L"\\data\\force_dcomp.flag")) {
        logf("render_mode: force_dcomp.flag present -> DCOMP (override)");
        return RenderModeResolution::UseDcomp;
    }

    // 2. Saved choice.
    std::wstring mf = user_data_path(L"render_mode.txt");
    if (file_exists(mf)) {
        std::optional<bool> v = parse_render_mode(read_file_utf8(mf));
        if (v.has_value()) {
            logf("render_mode: saved choice = %s",
                 *v ? "game_swapchain" : "dcomp");
            return *v ? RenderModeResolution::UseGameSwapchain
                      : RenderModeResolution::UseDcomp;
        }
        logf("render_mode: render_mode.txt unparseable -> re-prompting");
    }

    // 3. First run under Wine/Proton: the chooser modal does not render usable
    //    buttons there, and Compatibility/DCOMP does not work under DXVK anyway,
    //    so game-swapchain is the only viable backend (#45/#66). Skip the
    //    chooser entirely, pick Fast, save it, and render THIS session. There is
    //    no modal here, so the issue #60 boot-race that forces the Windows path
    //    to Defer does not apply -- we can run immediately, no restart needed.
    if (running_under_wine()) {
        logf("render_mode: Wine/Proton detected -> Fast/game_swapchain "
             "(chooser skipped, rendering this session; #66)");
        write_mode_file(true);
        return RenderModeResolution::UseGameSwapchain;
    }

    // 4. First run (Windows): ask, save, defer to restart.
    logf("render_mode: no saved choice -> showing chooser");
    int pick = show_chooser_dialog();
    if (pick == 0) {
        logf("render_mode: chooser dismissed -> not rendering this session, "
             "will ask again next launch");
        return RenderModeResolution::Defer;
    }
    bool game_swapchain = (pick == 1);
    write_mode_file(game_swapchain);
    logf("render_mode: user chose %s -> saved, deferring to restart",
         game_swapchain ? "Fast/game_swapchain" : "Compatibility/dcomp");
    MessageBoxW(nullptr,
        game_swapchain
            ? L"Render mode saved: Fast.\n\nPlease restart Farever to apply."
            : L"Render mode saved: Compatibility.\n\nPlease restart Farever to apply.",
        L"Farever Mod", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    return RenderModeResolution::Defer;
}

void render_mode_save(bool game_swapchain) {
    write_mode_file(game_swapchain);
    logf("render_mode: overlay toggle saved %s (restart to apply)",
         game_swapchain ? "game_swapchain" : "dcomp");
}

}  // namespace farever
