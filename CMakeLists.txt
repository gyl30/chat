cmake_minimum_required(VERSION 3.20)

project(chat LANGUAGES C CXX)

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

set(BOOST_HTTP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BOOST_HTTP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

set(WSLAY_CONFIGURE_INSTALL OFF CACHE BOOL "" FORCE)
set(WSLAY_STATIC ON CACHE BOOL "" FORCE)
set(WSLAY_SHARED OFF CACHE BOOL "" FORCE)
set(WSLAY_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WSLAY_TESTS OFF CACHE BOOL "" FORCE)

find_package(Boost 1.92.0 EXACT REQUIRED CONFIG COMPONENTS
    assert
    config
    core
    json
    mp11
    system
    throw_exception
    type_traits
    url
    winapi
)

add_subdirectory(third/capy EXCLUDE_FROM_ALL)
add_subdirectory(third/corosio EXCLUDE_FROM_ALL)
add_subdirectory(third/http EXCLUDE_FROM_ALL)
add_subdirectory(third/wslay EXCLUDE_FROM_ALL)

find_package(PostgreSQL REQUIRED)
find_package(OpenSSL REQUIRED)

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

add_executable(chat_http_test
    tests/http_server_test.cpp
)

target_link_libraries(chat_http_test
    PRIVATE
        Boost::capy
        Boost::corosio
        Boost::http
)

target_compile_options(chat_http_test
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

add_test(NAME http_server COMMAND chat_http_test)
set_tests_properties(http_server PROPERTIES TIMEOUT 5)

add_executable(chat_websocket_test
    tests/websocket_test.cpp
    src/websocket.cpp
)

target_include_directories(chat_websocket_test PRIVATE src)

target_link_libraries(chat_websocket_test
    PRIVATE
        Boost::capy
        Boost::corosio
        Boost::http
        OpenSSL::Crypto
        wslay
)

target_compile_options(chat_websocket_test
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

add_test(NAME websocket COMMAND chat_websocket_test)
set_tests_properties(websocket PROPERTIES TIMEOUT 5)

add_executable(chat_server_test
    tests/server_test.cpp
    src/chat_session.cpp
    src/server.cpp
    src/websocket.cpp
)

target_include_directories(chat_server_test PRIVATE src)

target_link_libraries(chat_server_test
    PRIVATE
        Boost::capy
        Boost::corosio
        Boost::http
        OpenSSL::Crypto
        wslay
)

target_compile_options(chat_server_test
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

add_test(NAME server COMMAND chat_server_test)
set_tests_properties(server PROPERTIES TIMEOUT 5)
