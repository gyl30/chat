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
# Pinned Boost.HTTP adds its optional JSON body middleware unconditionally.
# Chat uses routing/upgrade, not that middleware. Keep the submodule unmodified.
block()
    get_target_property(http_sources boost_http SOURCES)
    list(REMOVE_ITEM http_sources "${CMAKE_CURRENT_SOURCE_DIR}/third/http/src/json/json_body.cpp")
    set_property(TARGET boost_http PROPERTY SOURCES "${http_sources}")
    foreach(property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(http_links boost_http ${property})
        list(REMOVE_ITEM http_links Boost::json)
        set_property(TARGET boost_http PROPERTY ${property} "${http_links}")
    endforeach()
endblock()


set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
add_subdirectory(third/wslay EXCLUDE_FROM_ALL)
set(CMAKE_WARN_DEPRECATED ON CACHE BOOL "" FORCE)
unset(CMAKE_WARN_DEPRECATED CACHE)

add_subdirectory(third/simdjson EXCLUDE_FROM_ALL)
add_subdirectory(third/spdlog EXCLUDE_FROM_ALL)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
add_subdirectory(third/sqlite EXCLUDE_FROM_ALL)
include(cmake/chat_curl.cmake)
add_subdirectory(third/yxml EXCLUDE_FROM_ALL)

set(common_sources
    src/websocket_handshake.cpp
    src/sqlite_database.cpp
    src/blocking_worker.cpp
)

add_library(sdk STATIC
    ${common_sources}
    client/src/attachment_files.cpp
    client/src/client.cpp
    client/src/server_url.cpp
    client/src/websocket.cpp
)

target_include_directories(sdk PUBLIC client/include PRIVATE src)

target_link_libraries(sdk
    PRIVATE
        Boost::corosio_openssl
        simdjson::simdjson
        Boost::capy
        Boost::corosio
        Boost::http
        Boost::url
        OpenSSL::Crypto
        Threads::Threads
        wslay
        SQLite::SQLite3
        utf8proc::utf8proc
        spdlog::spdlog_header_only
        CURL::libcurl_static
)

add_subdirectory(third/lodepng EXCLUDE_FROM_ALL)

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
)

option(CHAT_BUILD_CLIENT "Build Qt Widgets client" OFF)
include(cmake/chat_unicode.cmake)
if(CHAT_BUILD_CLIENT)
    add_subdirectory(qt)
endif()

add_executable(server
    ${common_sources}
    src/attachment_access.cpp
    src/upload_store.cpp
    src/attachment_cleanup.cpp
    src/conversation_list.cpp
    src/conversation_store.cpp
    src/database.cpp
    src/friendship_store.cpp
    src/group_access_store.cpp
    src/message_changes.cpp
    src/message_history.cpp
    src/message_store.cpp
    src/archive_reader.cpp
    src/schema.cpp
    src/sqlite_mentions.cpp
    src/user_store.cpp
    src/attachment_io.cpp
    src/s3_objects.cpp
    src/s3_signer.cpp
    src/s3_xml.cpp
    src/main.cpp
    src/user_profile_request.cpp
    src/avatar_request.cpp
    src/avatar_image.cpp
    src/attachment_request.cpp
    src/conversation_file_request.cpp
    src/contact_request.cpp
    src/chat_session.cpp
    src/conversation_request.cpp
    src/group_management_request.cpp
    src/group_invite_request.cpp
    src/group_join_request.cpp
    src/authenticate_request.cpp
    src/login_session_store.cpp
    src/update_message_request.cpp
    src/reaction_request.cpp
    src/conversation_folder_request.cpp
    src/conversation_preference_request.cpp
    src/typing_request.cpp
    src/get_contacts_request.cpp
    src/get_conversations_request.cpp
    src/get_messages_request.cpp
    src/get_presence_request.cpp
    src/mark_read_request.cpp
    src/register_request.cpp
    src/send_message_request.cpp
    src/search_users_request.cpp
    src/password_hash.cpp
    src/invite_attempts.cpp
    src/json_rpc.cpp
    src/online_users.cpp
    src/server.cpp
    src/tls_config.cpp
    src/websocket.cpp
)

target_include_directories(server PRIVATE src client/include)

target_link_libraries(server
    PRIVATE
        Boost::corosio_openssl
        OpenSSL::SSL
        Boost::capy
        Boost::corosio
        Boost::http
        Threads::Threads
        OpenSSL::Crypto
        SQLite::SQLite3
        utf8proc::utf8proc
        CURL::libcurl_static
        yxml
        lodepng
        simdjson::simdjson
        spdlog::spdlog_header_only
        wslay
)
