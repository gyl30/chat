add_library(lodepng STATIC lodepng.cpp)
target_include_directories(lodepng SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(lodepng
    PUBLIC
        LODEPNG_NO_COMPILE_ENCODER
        LODEPNG_NO_COMPILE_DISK
        LODEPNG_NO_COMPILE_CPP
        LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
        LODEPNG_NO_COMPILE_ERROR_TEXT
    PRIVATE
        LODEPNG_MAX_ALLOC=1048576
)
