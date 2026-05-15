add_library(dpsmeter SHARED
    dllmain.cpp
    log.cpp
    d3d12_hook.cpp
    overlay.cpp
)

target_link_libraries(dpsmeter PRIVATE
    minhook
    imgui
    d3d12
    dxgi
    d3dcompiler
)

target_compile_definitions(dpsmeter PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE _UNICODE
)

# Output: dpsmeter.dll (no "lib" prefix). The injector loads this by name.
set_target_properties(dpsmeter PROPERTIES
    PREFIX ""
    OUTPUT_NAME "dpsmeter"
)
