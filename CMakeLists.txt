cmake_minimum_required(VERSION 3.30)

project(chat LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
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

set(SIMDJSON_EXCEPTIONS OFF CACHE BOOL "" FORCE)
set(SIMDJSON_STATIC_REFLECTION_MODE "ON" CACHE STRING "" FORCE)

set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)

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

set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
add_subdirectory(third/wslay EXCLUDE_FROM_ALL)
set(CMAKE_WARN_DEPRECATED ON CACHE BOOL "" FORCE)
unset(CMAKE_WARN_DEPRECATED CACHE)

add_subdirectory(third/simdjson EXCLUDE_FROM_ALL)
add_subdirectory(third/spdlog EXCLUDE_FROM_ALL)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_library(chat_client STATIC
    client/src/client.cpp
    client/src/websocket.cpp
)

target_include_directories(chat_client PUBLIC client/include)

target_link_libraries(chat_client
    PRIVATE
        Boost::json
        Boost::capy
        Boost::corosio
        Boost::http
        Boost::url
        OpenSSL::Crypto
        Threads::Threads
        wslay
)

find_package(PostgreSQL REQUIRED)

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
)

enable_testing()

option(CHAT_BUILD_QT_CLIENT "Build Qt Widgets client" OFF)
if(CHAT_BUILD_QT_CLIENT)
    add_subdirectory(qt)
endif()

add_executable(chat_server
    src/main.cpp
    src/contact_request.cpp
    src/chat_session.cpp
    src/conversation_request.cpp
    src/authenticate_request.cpp
    src/update_message_request.cpp
    src/echo_request.cpp
    src/get_contacts_request.cpp
    src/get_conversations_request.cpp
    src/get_messages_request.cpp
    src/get_presence_request.cpp
    src/get_unread_count_request.cpp
    src/mark_read_request.cpp
    src/register_request.cpp
    src/send_message_request.cpp
    src/search_users_request.cpp
    src/json_rpc.cpp
    src/online_users.cpp
    src/pg_connection.cpp
    src/pg_connection_pool.cpp
    src/server.cpp
    src/websocket.cpp
)

target_include_directories(chat_server PRIVATE src)

target_link_libraries(chat_server
    PRIVATE
        Boost::capy
        Boost::corosio
        Boost::http
        OpenSSL::Crypto
        PostgreSQL::PostgreSQL
        simdjson::simdjson
        spdlog::spdlog_header_only
        wslay
)

add_executable(chat_migration_test tests/migration_test.cpp)
target_link_libraries(chat_migration_test PRIVATE PostgreSQL::PostgreSQL)
add_test(NAME migration COMMAND chat_migration_test "${CMAKE_CURRENT_SOURCE_DIR}/sql")
set_tests_properties(migration PROPERTIES TIMEOUT 15)

add_executable(chat_client_test
    client/tests/client_test.cpp
    src/websocket.cpp
)

target_include_directories(chat_client_test PRIVATE src)

target_link_libraries(chat_client_test
    PRIVATE
        chat_client
        Boost::capy
        Boost::json
        Boost::corosio
        Boost::http
        OpenSSL::Crypto
        wslay
)

add_test(NAME client COMMAND chat_client_test)
set_tests_properties(client PROPERTIES TIMEOUT 5)

add_executable(chat_db_test
    tests/pg_connection_test.cpp
    src/pg_connection.cpp
    src/pg_connection_pool.cpp
)

target_include_directories(chat_db_test PRIVATE src)

target_link_libraries(chat_db_test
    PRIVATE
        Boost::capy
        Boost::corosio
        PostgreSQL::PostgreSQL
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

add_test(NAME websocket COMMAND chat_websocket_test)
set_tests_properties(websocket PROPERTIES TIMEOUT 5)

add_executable(chat_server_test
    tests/server_test.cpp
    tests/conversation_test.cpp
    src/contact_request.cpp
    src/chat_session.cpp
    src/conversation_request.cpp
    src/authenticate_request.cpp
    src/update_message_request.cpp
    src/echo_request.cpp
    src/get_contacts_request.cpp
    src/get_conversations_request.cpp
    src/get_messages_request.cpp
    src/get_presence_request.cpp
    src/get_unread_count_request.cpp
    src/mark_read_request.cpp
    src/register_request.cpp
    src/send_message_request.cpp
    src/search_users_request.cpp
    src/json_rpc.cpp
    src/online_users.cpp
    src/pg_connection.cpp
    src/pg_connection_pool.cpp
    src/server.cpp
    src/websocket.cpp
)

target_include_directories(chat_server_test PRIVATE src)

target_link_libraries(chat_server_test
    PRIVATE
        chat_client
        Boost::json
        Boost::capy
        Boost::corosio
        Boost::http
        OpenSSL::Crypto
        PostgreSQL::PostgreSQL
        simdjson::simdjson
        wslay
)

add_test(NAME server COMMAND chat_server_test)
set_tests_properties(server PROPERTIES TIMEOUT 30)

add_executable(chat_simdjson_reflection_test
    tests/simdjson_reflection_test.cpp
)

target_link_libraries(chat_simdjson_reflection_test
    PRIVATE
        simdjson::simdjson
)

add_test(NAME simdjson_reflection COMMAND chat_simdjson_reflection_test)
set_tests_properties(simdjson_reflection PROPERTIES TIMEOUT 5)

add_executable(chat_online_users_test
    tests/online_users_test.cpp
    src/online_users.cpp
)

target_include_directories(chat_online_users_test PRIVATE src)

add_test(NAME online_users COMMAND chat_online_users_test)
set_tests_properties(online_users PROPERTIES TIMEOUT 5)

add_executable(chat_bcrypt_test
    tests/bcrypt_test.cpp
)

target_link_libraries(chat_bcrypt_test
    PRIVATE
        Boost::capy
        Boost::corosio
        Boost::http
)

add_test(NAME bcrypt COMMAND chat_bcrypt_test)
set_tests_properties(bcrypt PROPERTIES TIMEOUT 5)
