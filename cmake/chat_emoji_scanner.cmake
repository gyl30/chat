include_guard(GLOBAL)
include(FetchContent)

# Official Google emoji-segmenter 0.4.0; generated scanner stays unmodified.
FetchContent_Declare(chat_emoji_segmenter
    URL https://codeload.github.com/google/emoji-segmenter/tar.gz/72bdc08c02be6cccdfc8cb1055fea4822a6494c1
    URL_HASH SHA256=6561ffc3fe83df9f9bf07da1d02522c9b0c6878aeaf56282116b92288367e51c
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(chat_emoji_segmenter)
FetchContent_GetProperties(chat_emoji_segmenter SOURCE_DIR chat_emoji_segmenter_source)
set(emoji_scanner_files emoji_presentation_scanner.c LICENSE)
set(emoji_scanner_hashes
    e41071624cf91187ae26396b3d2b9adda648c5afb4519ff562e7aeb9415c157d
    cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30
)
foreach(source expected_hash IN ZIP_LISTS emoji_scanner_files emoji_scanner_hashes)
    file(SHA256 "${chat_emoji_segmenter_source}/${source}" actual_hash)
    if(NOT actual_hash STREQUAL expected_hash)
        message(FATAL_ERROR "Pinned Google emoji-segmenter 0.4.0 source changed: ${source}")
    endif()
endforeach()
add_library(chat_emoji_scanner INTERFACE)
target_include_directories(chat_emoji_scanner SYSTEM INTERFACE "${chat_emoji_segmenter_source}")
