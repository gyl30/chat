find_package(Qt6 6.5 REQUIRED COMPONENTS Svg Widgets)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Qt6 6.5 REQUIRED COMPONENTS DBus)
endif()
add_subdirectory("${PROJECT_SOURCE_DIR}/third/emoji-segmenter" "${PROJECT_BINARY_DIR}/third/emoji-segmenter" EXCLUDE_FROM_ALL)

add_executable(client
    src/main.cpp
    src/avatar.cpp
    src/attachment_dialog.cpp
    src/download_dialog.cpp
    src/attachment_dialog.hpp
    src/avatar.hpp
    src/global_search_dialog.cpp
    src/bookmarks_dialog.cpp
    src/chat_history_dialog.cpp
    src/chat_history_dialog.hpp
    src/chat_widget.cpp
    src/chat_widget.hpp
    src/create_group_dialog.cpp
    src/create_group_dialog.hpp
    src/clipboard_image.cpp
    src/client_bridge.cpp
    src/client_bridge.hpp
    src/conversation_data.hpp
    src/conversation_delegate.cpp
    src/conversation_delegate.hpp
    src/conversation_model.cpp
    src/conversation_model.hpp
    src/emoji_segments.cpp
    src/emoji_segments.hpp
    src/emoji_text.cpp
    src/emoji_text.hpp
    src/group_files_page.cpp
    src/group_files_page.hpp
    src/group_dialog.cpp
    src/group_dialog.hpp
    src/member_data.hpp
    src/icons.cpp
    src/icons.hpp
    src/main_window.cpp
    src/main_window.hpp
    src/message_data.hpp
    src/message_markdown.cpp
    src/bundle_dialog.cpp
    src/poll_dialog.cpp
    src/message_links.cpp
    src/message_card.cpp
    src/message_delegate.cpp
    src/message_delegate.hpp
    src/message_model.cpp
    src/message_model.hpp
    src/message_images.cpp
    src/message_images.hpp
    src/message_search_dialog.cpp
    src/message_search_dialog.hpp
    src/presence.hpp
    src/presence_data.hpp
    src/screenshot_dialog.cpp
    src/settings_dialog.cpp
    src/settings_dialog.hpp
    src/login_sessions_dialog.cpp
    src/login_sessions_dialog.hpp
    src/theme.cpp
    src/theme.hpp
    src/theme_manager.cpp
    src/theme_manager.hpp
    src/title_bar.cpp
    src/title_bar.hpp
    src/user_data.hpp
    src/user_delegate.cpp
    src/user_delegate.hpp
    src/user_model.cpp
    src/user_model.hpp
)

qt_add_resources(client chat_icons
    PREFIX "/icons"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons"
    FILES
        resources/icons/activity.svg
        resources/icons/attach.svg
        resources/icons/bell-off.svg
        resources/icons/bookmark.svg
        resources/icons/chat.svg
        resources/icons/check.svg
        resources/icons/chevron-down.svg
        resources/icons/chevron-up.svg
        resources/icons/chevron-right.svg
        resources/icons/back.svg
        resources/icons/close.svg
        resources/icons/contacts.svg
        resources/icons/copy.svg
        resources/icons/emoji.svg
        resources/icons/groups.svg
        resources/icons/history.svg
        resources/icons/menu.svg
        resources/icons/more.svg
        resources/icons/pin.svg
        resources/icons/plus.svg
        resources/icons/screenshot.svg
        resources/icons/search.svg
        resources/icons/send.svg
        resources/icons/settings.svg
        resources/icons/user-plus.svg
        resources/icons/window-maximize.svg
        resources/icons/window-minimize.svg
        resources/icons/window-restore.svg
)

set_target_properties(client PROPERTIES AUTOMOC ON)
target_include_directories(client PRIVATE "${chat_unicode_include}")

target_link_libraries(client
    PRIVATE
        sdk
        Qt6::Svg
        Qt6::Widgets
        utf8proc::utf8proc
        emoji_scanner
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(client PRIVATE Qt6::DBus)
    configure_file(io.github.gyl30.Chat.desktop.in io.github.gyl30.Chat.desktop @ONLY)
endif()

# Keep editable themes beside the executable in the build tree.
set(chat_theme_files common.qss daylight.qss daylight.json midnight.qss midnight.json)
foreach(theme_file IN LISTS chat_theme_files)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/themes/${theme_file}"
                   "${CMAKE_CURRENT_BINARY_DIR}/themes/${theme_file}" COPYONLY)
endforeach()
