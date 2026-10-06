include_guard(GLOBAL)
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

# The standard offline override bypasses archive verification, not source pins.
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

set(emoji_property_data "${PROJECT_SOURCE_DIR}/tui/cmake/unicode/emoji-data-18.0.0.txt")
set(emoji_property_table "${PROJECT_SOURCE_DIR}/tui/cmake/unicode/emoji_properties_18.inc")
file(SHA256 "${emoji_property_data}" emoji_data_hash)
file(SHA256 "${emoji_property_table}" emoji_table_hash)
if(NOT emoji_data_hash STREQUAL "80d00f8e616a0ef27fd6b8de3b758c06383b5d917e2977709578e68baf733bf1" OR
   NOT emoji_table_hash STREQUAL "b200a53eeae768e7e67a2d465936b0cb0e859618cc877b99dac79d454e2e6937")
    message(FATAL_ERROR "Pinned Unicode18 emoji data/table changed; regenerate and review pins")
endif()

# Derive the scanner properties from the same reviewed Unicode18 source.
# No Python, runtime data files, or manually maintained emoji whitelist.
set(chat_unicode_include "${PROJECT_BINARY_DIR}/chat-unicode")
file(STRINGS "${emoji_property_data}" emoji_property_lines)
set(emoji_properties_header "// Generated from pinned Unicode18 emoji-data.txt. Unicode License V3.\n#pragma once\nnamespace chat_unicode {\nstruct interval { char32_t first; char32_t last; };\n")
foreach(property Emoji Emoji_Presentation Emoji_Modifier_Base Emoji_Modifier)
    string(APPEND emoji_properties_header "inline constexpr interval ${property}[] = {\n")
    foreach(line IN LISTS emoji_property_lines)
        if(line MATCHES "^([0-9A-F]+)(\\.\\.([0-9A-F]+))?[ \t]*;[ \t]*${property}[ \t]*#")
            set(first "${CMAKE_MATCH_1}")
            set(last "${CMAKE_MATCH_3}")
            if(last STREQUAL "")
                set(last "${first}")
            endif()
            string(APPEND emoji_properties_header "    {0x${first}, 0x${last}},\n")
        endif()
    endforeach()
    string(APPEND emoji_properties_header "};\n")
endforeach()
string(APPEND emoji_properties_header "}\n")
file(GENERATE OUTPUT "${chat_unicode_include}/emoji_properties.hpp" CONTENT "${emoji_properties_header}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${emoji_property_data}" "${emoji_property_table}")
