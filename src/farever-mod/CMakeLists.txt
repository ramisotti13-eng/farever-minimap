add_library(farever_mod SHARED
    dllmain.cpp
    dinput8_proxy.cpp
    libhl.cpp
    log.cpp
    user_data.cpp
    mem_scan.cpp
    type_anchor.cpp
    hl_hook.cpp
    hl_pump.cpp
    damage.cpp
    heal.cpp
    aggregator.cpp
    hero_state.cpp
    camera_state.cpp
    boss_timer.cpp
    boss_target.cpp
    mob_watch.cpp
    party_state.cpp
    target_state.cpp
    uid_registry.cpp
    codex_state.cpp
    textures.cpp
    pois.cpp
    poi_progress.cpp
    activity_icons.cpp
    waypoints.cpp
    skill_resolve.cpp
    progress_state.cpp
    entity_state.cpp
    anticheat.cpp
    render_mode.cpp
    overlay.cpp
    overlay_window.cpp
    d3d12_hook.cpp
    plugins.cpp
    dinput8.def
)

target_link_libraries(farever_mod PRIVATE
    minhook
    imgui
    lua
    d3d12
    dxgi
    d3dcompiler
    dcomp
    winmm
    bcrypt
    shell32
    ole32
)

target_compile_definitions(farever_mod PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE _UNICODE
)

# Output: dinput8.dll. Windows looks up dinput8 in the EXE directory
# before %SystemRoot%\System32, so dropping our DLL next to Farever.exe
# is enough - no injector needed.
set_target_properties(farever_mod PROPERTIES
    PREFIX ""
    OUTPUT_NAME "dinput8"
)
