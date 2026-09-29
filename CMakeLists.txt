cmake_minimum_required(VERSION 3.20)

project(chat LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BOOST_CAPY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BOOST_CAPY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BOOST_CAPY_BUILD_BENCH OFF CACHE BOOL "" FORCE)

set(BOOST_COROSIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BOOST_COROSIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BOOST_COROSIO_BUILD_BENCH OFF CACHE BOOL "" FORCE)

include(FetchContent)

FetchContent_Declare(
    capy
    GIT_REPOSITORY https://github.com/cppalliance/capy.git
    GIT_TAG b2dd252cbcac816658c89fd87b5fa02390202d43
)

FetchContent_Declare(
    corosio
    GIT_REPOSITORY https://github.com/cppalliance/corosio.git
    GIT_TAG 54e28fd40cf7ee6cc18365dd81ad5615c38908cc
)

FetchContent_MakeAvailable(capy corosio)

find_package(PostgreSQL REQUIRED)

add_executable(chat_db_poc
    src/main.cpp
    src/pg_connection.cpp
)

target_include_directories(chat_db_poc PRIVATE src)

target_link_libraries(chat_db_poc
    PRIVATE
        Boost::capy
        Boost::corosio
        PostgreSQL::PostgreSQL
)

target_compile_options(chat_db_poc
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

enable_testing()

add_executable(chat_db_test
    tests/pg_connection_test.cpp
    src/pg_connection.cpp
)

target_include_directories(chat_db_test PRIVATE src)

target_link_libraries(chat_db_test
    PRIVATE
        Boost::capy
        Boost::corosio
        PostgreSQL::PostgreSQL
)

target_compile_options(chat_db_test
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

add_test(NAME pg_connection COMMAND chat_db_test)
set_tests_properties(pg_connection PROPERTIES TIMEOUT 15)

add_executable(chat_corosio_timeout_test
    tests/corosio_timeout_test.cpp
)

target_link_libraries(chat_corosio_timeout_test
    PRIVATE
        Boost::capy
        Boost::corosio
)

target_compile_options(chat_corosio_timeout_test
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

add_test(NAME corosio_timeout COMMAND chat_corosio_timeout_test)
set_tests_properties(corosio_timeout PROPERTIES TIMEOUT 5)
