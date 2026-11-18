cmake_minimum_required(VERSION 3.30)

project(chat LANGUAGES C CXX)

include(GNUInstallDirs)

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
    client/src/server_url.cpp
    client/src/websocket.cpp
)

target_include_directories(chat_client PUBLIC client/include)

target_link_libraries(chat_client
    PRIVATE
        Boost::corosio_openssl
        Boost::json
        Boost::capy
        Boost::corosio
        Boost::http
        Boost::url
        OpenSSL::Crypto
        Threads::Threads
        wslay
)

find_package(SQLite3 REQUIRED)
find_package(miniocpp CONFIG REQUIRED)
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
)

option(CHAT_BUILD_QT_CLIENT "Build Qt Widgets client" OFF)
include(cmake/chat_unicode.cmake)
if(CHAT_BUILD_QT_CLIENT)
    add_subdirectory(qt)
endif()

add_executable(chat_server
    src/main.cpp
    src/avatar_request.cpp
    src/avatar_image.cpp
    src/attachment_request.cpp
    src/contact_request.cpp
    src/chat_session.cpp
    src/conversation_request.cpp
    src/group_management_request.cpp
    src/group_invite_request.cpp
    src/group_join_request.cpp
    src/authenticate_request.cpp
    src/update_message_request.cpp
    src/reaction_request.cpp
    src/conversation_preference_request.cpp
    src/typing_request.cpp
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
    src/password_hash.cpp
    src/blocking_worker.cpp
    src/invite_attempts.cpp
    src/json_rpc.cpp
    src/online_users.cpp
    src/server.cpp
    src/tls_config.cpp
    src/websocket.cpp
)

target_include_directories(chat_server PRIVATE src client/include)

target_link_libraries(chat_server
    PRIVATE
        Boost::corosio_openssl
        OpenSSL::SSL
        Boost::capy
        Boost::corosio
        Boost::http
        Threads::Threads
        OpenSSL::Crypto
        chat_attachment_storage
        PNG::PNG
        JPEG::JPEG
        simdjson::simdjson
        spdlog::spdlog_header_only
        wslay
)

# Keep external dependency locations discovered by CMake in installed binaries.
set_target_properties(chat_server PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)
install(TARGETS chat_server RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

add_library(chat_sqlite_storage STATIC
    src/attachment_access.cpp
    src/attachment_cleanup.cpp
    src/blocking_worker.cpp
    src/conversation_list.cpp
    src/conversation_store.cpp
    src/database.cpp
    src/friendship_store.cpp
    src/group_access_store.cpp
    src/message_changes.cpp
    src/message_history.cpp
    src/message_store.cpp
    src/schema.cpp
    src/sqlite_database.cpp
    src/sqlite_mentions.cpp
    src/user_store.cpp
)
target_include_directories(chat_sqlite_storage PUBLIC src client/include)
target_link_libraries(chat_sqlite_storage PUBLIC Boost::capy SQLite::SQLite3 Threads::Threads PRIVATE utf8proc OpenSSL::Crypto)

add_library(chat_attachment_storage STATIC src/attachment_io.cpp src/s3_objects.cpp)
target_link_libraries(chat_attachment_storage
    PUBLIC chat_sqlite_storage
    PRIVATE miniocpp::miniocpp OpenSSL::Crypto
)
