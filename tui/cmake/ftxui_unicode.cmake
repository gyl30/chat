include(FetchContent)

block()
    set(BUILD_SHARED_LIBS OFF)
    set(UTF8PROC_INSTALL OFF CACHE BOOL "" FORCE)
    set(UTF8PROC_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(utf8proc
        URL https://codeload.github.com/JuliaStrings/utf8proc/tar.gz/79cdd5ab40c79afa559f689ffc13b76812dee1ac
        URL_HASH SHA256=e8fadfcd531d65525cbef157b866c5408e2e3f0d7c0f3aefb0bce01a7a08e35d
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(utf8proc)
endblock()

export(TARGETS utf8proc NAMESPACE utf8proc::
    FILE "${CMAKE_CURRENT_BINARY_DIR}/utf8proc-targets.cmake")

set(ftxui_sources
    src/ftxui/screen/string.cpp
    src/ftxui/component/input.cpp
    src/ftxui/screen/string_internal.hpp
)
set(ftxui_source_hashes
    39d528e6f515e900f39a3014a8ca72a49db3a9611ecd17ff77005360d7fb9023
    1d14f449393ffefa926a3bd6fb9080b0870fa8433b3bce0147080caa5d4991d3
    683a8e04ac0d6b6cbdf87ddf3d2af47504109a6e8d577ee4cb4edfdf0d68005a
)
set(ftxui_patch "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ftxui-7.0.3-grapheme.patch")
set(ftxui_staging "${CMAKE_CURRENT_BINARY_DIR}/ftxui-staging")
set(ftxui_patched "${CMAKE_CURRENT_BINARY_DIR}/ftxui-patched")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ftxui_patch}")
foreach(source expected_hash IN ZIP_LISTS ftxui_sources ftxui_source_hashes)
    set(original "${PROJECT_SOURCE_DIR}/third/ftxui/${source}")
    file(SHA256 "${original}" actual_hash)
    if(NOT actual_hash STREQUAL expected_hash)
        message(FATAL_ERROR "FTXUI 7.0.3 Unicode patch source changed: ${source}")
    endif()
    configure_file("${original}" "${ftxui_staging}/${source}" COPYONLY)
endforeach()

find_program(PATCH_EXECUTABLE patch REQUIRED)
execute_process(
    COMMAND "${PATCH_EXECUTABLE}" --batch --forward --fuzz=0 --no-backup-if-mismatch
        -p1 -i "${ftxui_patch}"
    WORKING_DIRECTORY "${ftxui_staging}"
    RESULT_VARIABLE ftxui_patch_result
    OUTPUT_VARIABLE ftxui_patch_output
    ERROR_VARIABLE ftxui_patch_error
)
if(NOT ftxui_patch_result EQUAL 0)
    message(FATAL_ERROR "FTXUI Unicode patch failed: ${ftxui_patch_output}${ftxui_patch_error}")
endif()
foreach(source IN LISTS ftxui_sources)
    configure_file("${ftxui_staging}/${source}" "${ftxui_patched}/${source}" COPYONLY)
endforeach()

get_target_property(screen_sources screen SOURCES)
list(FIND screen_sources "src/ftxui/screen/string.cpp" string_index)
if(string_index LESS 0)
    message(FATAL_ERROR "FTXUI screen source list changed")
endif()
list(REMOVE_ITEM screen_sources "src/ftxui/screen/string.cpp")
list(APPEND screen_sources "${ftxui_patched}/src/ftxui/screen/string.cpp")
set_property(TARGET screen PROPERTY SOURCES "${screen_sources}")
target_include_directories(screen BEFORE PRIVATE "${ftxui_patched}/src")
target_link_libraries(screen PRIVATE utf8proc::utf8proc)

get_target_property(component_sources component SOURCES)
list(FIND component_sources "src/ftxui/component/input.cpp" input_index)
if(input_index LESS 0)
    message(FATAL_ERROR "FTXUI component source list changed")
endif()
list(REMOVE_ITEM component_sources "src/ftxui/component/input.cpp")
list(APPEND component_sources "${ftxui_patched}/src/ftxui/component/input.cpp")
set_property(TARGET component PROPERTY SOURCES "${component_sources}")
target_include_directories(component BEFORE PRIVATE "${ftxui_patched}/src")
