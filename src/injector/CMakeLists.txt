add_executable(inject WIN32
    main.cpp
)
set_target_properties(inject PROPERTIES WIN32_EXECUTABLE FALSE)  # console app
target_compile_definitions(inject PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE _UNICODE
)
target_link_libraries(inject PRIVATE psapi)
