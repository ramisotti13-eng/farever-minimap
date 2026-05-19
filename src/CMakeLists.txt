cmake_minimum_required(VERSION 3.20)
project(farevermod
    LANGUAGES C CXX
    DESCRIPTION "Farever minimap mod — DLL + injector"
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT WIN32)
    message(FATAL_ERROR "farevermod is Windows-only (DirectX 12 hook)")
endif()

# x64 only.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "must build for x64 (the game is 64-bit)")
endif()

# Build everything as release-with-symbols by default.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# MinHook: tiny hooking library, fetched from upstream.
include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(minhook)

# Dear ImGui: pinned to the docking branch (DX12 backend + multi-viewport).
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
)
FetchContent_MakeAvailable(imgui)

# ImGui has no CMakeLists; create a static target manually.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui
    PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)
target_compile_features(imgui PUBLIC cxx_std_17)

# Lua 5.4 — embedded scripting runtime for the plugin system. Plugin
# authors write .lua files into the user's data/plugins/ folder, the
# mod loads them automatically and gives them a curated read-only API
# + ImGui-subset for drawing their own overlay windows. Lua is MIT
# licensed; the upstream tarball ships C sources but no CMakeLists,
# so we create the static target ourselves (same approach as ImGui).
FetchContent_Declare(
    lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG        v5.4.7
)
FetchContent_MakeAvailable(lua)

# Build Lua sources into a static library. We deliberately omit
# lua.c (interpreter entry point) and onelua.c (one-file build helper);
# only the runtime is needed.
file(GLOB LUA_SOURCES
    "${lua_SOURCE_DIR}/lapi.c"
    "${lua_SOURCE_DIR}/lauxlib.c"
    "${lua_SOURCE_DIR}/lbaselib.c"
    "${lua_SOURCE_DIR}/lcode.c"
    "${lua_SOURCE_DIR}/lcorolib.c"
    "${lua_SOURCE_DIR}/lctype.c"
    "${lua_SOURCE_DIR}/ldblib.c"
    "${lua_SOURCE_DIR}/ldebug.c"
    "${lua_SOURCE_DIR}/ldo.c"
    "${lua_SOURCE_DIR}/ldump.c"
    "${lua_SOURCE_DIR}/lfunc.c"
    "${lua_SOURCE_DIR}/lgc.c"
    "${lua_SOURCE_DIR}/linit.c"
    "${lua_SOURCE_DIR}/liolib.c"
    "${lua_SOURCE_DIR}/llex.c"
    "${lua_SOURCE_DIR}/lmathlib.c"
    "${lua_SOURCE_DIR}/lmem.c"
    "${lua_SOURCE_DIR}/loadlib.c"
    "${lua_SOURCE_DIR}/lobject.c"
    "${lua_SOURCE_DIR}/lopcodes.c"
    "${lua_SOURCE_DIR}/loslib.c"
    "${lua_SOURCE_DIR}/lparser.c"
    "${lua_SOURCE_DIR}/lstate.c"
    "${lua_SOURCE_DIR}/lstring.c"
    "${lua_SOURCE_DIR}/lstrlib.c"
    "${lua_SOURCE_DIR}/ltable.c"
    "${lua_SOURCE_DIR}/ltablib.c"
    "${lua_SOURCE_DIR}/ltm.c"
    "${lua_SOURCE_DIR}/lundump.c"
    "${lua_SOURCE_DIR}/lutf8lib.c"
    "${lua_SOURCE_DIR}/lvm.c"
    "${lua_SOURCE_DIR}/lzio.c"
)
# We include the full Lua runtime here (including io/os libs because
# linit.c references their symbols). The plugin sandbox is enforced
# at lua_State setup time in plugins.cpp by removing dangerous globals
# (io, os.execute, os.remove, os.exit, package.loadlib, dofile,
# loadfile, require) before any plugin code runs.
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC ${lua_SOURCE_DIR})
# Lua 5.4 compiles as C99. MSVC warnings about secure CRT are noise.
target_compile_definitions(lua PRIVATE _CRT_SECURE_NO_WARNINGS)
if (MSVC)
    target_compile_options(lua PRIVATE /wd4244 /wd4267 /wd4310 /wd4334)
endif()

add_subdirectory(minimap-dll)
add_subdirectory(dpsmeter-dll)
add_subdirectory(injector)
add_subdirectory(farever-mod)
