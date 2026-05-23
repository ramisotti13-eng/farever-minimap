add_library(farever_mod SHARED
    dllmain.cpp
    dinput8_proxy.cpp
    libhl.cpp
    log.cpp
    mem_scan.cpp
    type_anchor.cpp
    hl_hook.cpp
    hl_pump.cpp
    damage.cpp
    aggregator.cpp
    hero_state.cpp
    textures.cpp
    pois.cpp
    poi_progress.cpp
    skill_resolve.cpp
    progress_state.cpp
    entity_state.cpp
    overlay.cpp
    d3d12_hook.cpp
    dinput8.def
)

target_link_libraries(farever_mod PRIVATE
    minhook
    imgui
    d3d12
    dxgi
    d3dcompiler
)

target_compile_definitions(farever_mod PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE _UNICODE
)

# Output: dinput8.dll. Windows looks up dinput8 in the EXE directory
# before %SystemRoot%\System32, so dropping our DLL next to Farever.exe
# is enough — no injector needed.
set_target_properties(farever_mod PROPERTIES
    PREFIX ""
    OUTPUT_NAME "dinput8"
)
