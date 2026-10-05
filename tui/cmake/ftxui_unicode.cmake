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

# FETCHCONTENT_SOURCE_DIR_UTF8PROC bypasses archive verification. Verify the
# pinned API/Unicode source files as well, including the Unicode18 data.
FetchContent_GetProperties(utf8proc SOURCE_DIR utf8proc_pinned_source)
set(utf8proc_pinned_files utf8proc.h utf8proc.c utf8proc_data.c)
set(utf8proc_pinned_hashes
    08da85e820efaa3613500946c26b2dc427a07f76001ed7c16bc90f298b814c35
    f38438eb986ba7eedad0f2e820a70fae1aaf05197364ba0381a5e84775099ef8
    b4911d4162d216afa612317a9b981851e15d672f4d93b25f3a1eb986f8c0aedb
)
foreach(source expected_hash IN ZIP_LISTS utf8proc_pinned_files utf8proc_pinned_hashes)
    file(SHA256 "${utf8proc_pinned_source}/${source}" actual_hash)
    if(NOT actual_hash STREQUAL expected_hash)
        message(FATAL_ERROR "Pinned utf8proc 2.12.0 / Unicode18 source changed: ${source}")
    endif()
endforeach()

set(emoji_property_data "${CMAKE_CURRENT_SOURCE_DIR}/cmake/unicode/emoji-data-18.0.0.txt")
set(emoji_property_table "${CMAKE_CURRENT_SOURCE_DIR}/cmake/unicode/emoji_properties_18.inc")
file(SHA256 "${emoji_property_data}" emoji_data_hash)
file(SHA256 "${emoji_property_table}" emoji_table_hash)
if(NOT emoji_data_hash STREQUAL "80d00f8e616a0ef27fd6b8de3b758c06383b5d917e2977709578e68baf733bf1" OR
   NOT emoji_table_hash STREQUAL "b200a53eeae768e7e67a2d465936b0cb0e859618cc877b99dac79d454e2e6937")
    message(FATAL_ERROR "Pinned Unicode18 emoji data/table changed; regenerate and review pins")
endif()

export(TARGETS utf8proc NAMESPACE utf8proc::
    FILE "${CMAKE_CURRENT_BINARY_DIR}/utf8proc-targets.cmake")

set(ftxui_sources
    include/ftxui/dom/canvas.hpp
    include/ftxui/dom/selection.hpp
    include/ftxui/screen/cell.hpp
    include/ftxui/screen/string.hpp
    include/ftxui/screen/surface.hpp
    src/ftxui/component/input.cpp
    src/ftxui/dom/border.cpp
    src/ftxui/dom/canvas.cpp
    src/ftxui/dom/clear_under.cpp
    src/ftxui/dom/frame.cpp
    src/ftxui/dom/gauge.cpp
    src/ftxui/dom/graph.cpp
    src/ftxui/dom/node.cpp
    src/ftxui/dom/scroll_indicator.cpp
    src/ftxui/dom/selection.cpp
    src/ftxui/dom/separator.cpp
    src/ftxui/dom/text.cpp
    src/ftxui/screen/screen.cpp
    src/ftxui/screen/string.cpp
    src/ftxui/screen/string_internal.hpp
    src/ftxui/screen/surface.cpp
)
set(ftxui_source_hashes
    da0422ceda34d931dd41c31ebe735013187e016b9843208c799c6a93818d6f9c
    7aaf87f678e82a54ac42eae929bc62d9cecd61eba50838851b66eb8b0750d3c3
    07cb6ac81fd63acb24c42a7cf9c1d1d3032ea7c46115b2b4b09d117035c4fd38
    d0e324f820d8c2d363e9e1aaf60609b9f29877686a8d311d0670fe4865538681
    c4767859f2f201084d660692e3b91a9955184ac2cc5ab64df5ae82b1fb7ec9e9
    1d14f449393ffefa926a3bd6fb9080b0870fa8433b3bce0147080caa5d4991d3
    4d351ffb550d64e12be57c750099c663eb571af0ec128fd7b44daa30263ebf42
    585ec7711a32f8d04bb4fc414f4e8e7695b49996e19c1d4dc27a74af690475b3
    77bc07b3646b30e3c74b83bcfa5b3cd85c89c3f6b786f865b163919c55059ff4
    a96aaa0777cc1b53779b90c5a8cf6e76f17d8271198fc607a55f825e3018e091
    0df5df1e262a66ed374310e705054439aaf8445762a848d0b35e0b6e1001068a
    b54977279864d7b6c49abd34149daeb80e9e384398f2551dbf7be23ed2479452
    25e04e33ce309b1588e08c070779e47cd56368b83d5d74e14d818cb0a062fdce
    bc5ff9ad372fbe83f027463634bf5efd316de3863a449e2a44c0a5d611b4cdd5
    745c4a010350c1de80897023e9e4fd927e0f3e322a545b5ac8cdbe05baeaad6d
    0fdcd2aaa04716716fe5f5397e75edf82ab1d9f1f433d74bb398c822bb222da3
    5fffdbe42b4285349f0300368f7c31dc452d4179a71d430793814f265045052e
    8a3d3d73dda1c34aafd95387f13cb06be531dc6527aac6f24ae9b01593aceaf4
    39d528e6f515e900f39a3014a8ca72a49db3a9611ecd17ff77005360d7fb9023
    683a8e04ac0d6b6cbdf87ddf3d2af47504109a6e8d577ee4cb4edfdf0d68005a
    337a071f8ec4612e838f5778606ae316e300e16a3c787b19ddaf8a496271cfbd
)
set(ftxui_patch "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ftxui-7.0.3-grapheme.patch")
set(ftxui_staging "${CMAKE_CURRENT_BINARY_DIR}/ftxui-staging")
set(ftxui_patched "${CMAKE_CURRENT_BINARY_DIR}/ftxui-patched")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ftxui_patch}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${emoji_property_data}" "${emoji_property_table}")
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
configure_file("${emoji_property_table}"
    "${ftxui_patched}/src/ftxui/screen/emoji_properties_18.inc" COPYONLY)

foreach(target screen dom component)
    get_target_property(target_sources "${target}" SOURCES)
    foreach(source IN LISTS ftxui_sources)
        if(source MATCHES "^src/ftxui/${target}/.*\\.cpp$")
            list(FIND target_sources "${source}" source_index)
            if(source_index LESS 0)
                message(FATAL_ERROR "FTXUI ${target} source list changed: ${source}")
            endif()
            list(REMOVE_ITEM target_sources "${source}")
            list(APPEND target_sources "${ftxui_patched}/${source}")
        endif()
    endforeach()
    set_property(TARGET "${target}" PROPERTY SOURCES "${target_sources}")
    target_include_directories("${target}" BEFORE
        PUBLIC "$<BUILD_INTERFACE:${ftxui_patched}/include>"
        PRIVATE "${ftxui_patched}/src")
endforeach()
target_link_libraries(screen PRIVATE utf8proc::utf8proc)
