#pragma once

// Pure, dependency-free parsing for the render-mode choice file
// (data/render_mode.txt). Kept free of <windows.h> so it unit-tests
// with a plain cl invocation (see tools/test_render_mode.cpp).

#include <optional>
#include <string>

namespace farever {

// Parse the render_mode.txt contents.
//   "game_swapchain" -> true   (Fast / render into the game swap chain)
//   "dcomp"          -> false  (Compatibility / DirectComposition window)
//   anything else / empty -> std::nullopt (treated as "no choice yet")
// Leading/trailing ASCII whitespace is ignored; the match is exact and
// case-sensitive otherwise (so a stray token never silently picks a mode).
inline std::optional<bool> parse_render_mode(const std::string& contents) {
    const char* ws = " \t\r\n\f\v";
    std::size_t b = contents.find_first_not_of(ws);
    if (b == std::string::npos) return std::nullopt;
    std::size_t e = contents.find_last_not_of(ws);
    std::string tok = contents.substr(b, e - b + 1);
    if (tok == "game_swapchain") return true;
    if (tok == "dcomp")          return false;
    return std::nullopt;
}

// Canonical token for each backend, written to render_mode.txt.
inline const char* render_mode_token(bool game_swapchain) {
    return game_swapchain ? "game_swapchain" : "dcomp";
}

}  // namespace farever
