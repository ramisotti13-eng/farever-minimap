add_library(minimap SHARED
    dllmain.cpp
    log.cpp
    live_position.cpp
    d3d12_hook.cpp
    overlay.cpp
    textures.cpp
)

target_link_libraries(minimap PRIVATE
    minhook
    imgui
    d3d12
    dxgi
    d3dcompiler
)

target_compile_definitions(minimap PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE _UNICODE
)

# Output: minimap.dll (no "lib" prefix). The injector loads this by name.
set_target_properties(minimap PROPERTIES
    PREFIX ""
    OUTPUT_NAME "minimap"
)
