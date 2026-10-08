find_package(Qt6 6.5 REQUIRED COMPONENTS Svg Widgets)
include("${PROJECT_SOURCE_DIR}/cmake/chat_emoji_scanner.cmake")

add_library(chat_qt_widgets OBJECT
    src/avatar.cpp
    src/attachment_dialog.cpp
    src/attachment_dialog.hpp
    src/avatar.hpp
    src/chat_widget.cpp
    src/chat_widget.hpp
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
    src/group_dialog.cpp
    src/group_dialog.hpp
    src/member_data.hpp
    src/icons.cpp
    src/icons.hpp
    src/main_window.cpp
    src/main_window.hpp
    src/message_data.hpp
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

qt_add_resources(chat_qt_widgets chat_icons
    PREFIX "/icons"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons"
    FILES
        resources/icons/activity.svg
        resources/icons/attach.svg
        resources/icons/bookmark.svg
        resources/icons/chat.svg
        resources/icons/check.svg
        resources/icons/chevron-down.svg
        resources/icons/close.svg
        resources/icons/contacts.svg
        resources/icons/copy.svg
        resources/icons/groups.svg
        resources/icons/more.svg
        resources/icons/plus.svg
        resources/icons/search.svg
        resources/icons/send.svg
        resources/icons/settings.svg
        resources/icons/user-plus.svg
        resources/icons/window-maximize.svg
        resources/icons/window-minimize.svg
        resources/icons/window-restore.svg
)

set_target_properties(chat_qt_widgets PROPERTIES AUTOMOC ON)
target_include_directories(chat_qt_widgets PRIVATE "${chat_unicode_include}")

target_link_libraries(chat_qt_widgets
    PUBLIC
        chat_client
        Qt6::Svg
        Qt6::Widgets
    PRIVATE
        utf8proc::utf8proc
        chat_emoji_scanner
)

add_executable(chat_qt_models_test tests/models_test.cpp)
target_include_directories(chat_qt_models_test PRIVATE src)
target_link_libraries(chat_qt_models_test PRIVATE chat_qt_widgets)
add_test(NAME qt_models COMMAND chat_qt_models_test)

add_executable(chat_qt_delegate_test tests/delegate_test.cpp)
target_include_directories(chat_qt_delegate_test PRIVATE src)
target_link_libraries(chat_qt_delegate_test PRIVATE chat_qt_widgets)
add_test(NAME qt_delegate COMMAND chat_qt_delegate_test)
set_tests_properties(qt_delegate PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(chat_qt src/main.cpp)
target_link_libraries(chat_qt PRIVATE chat_qt_widgets)

add_executable(chat_qt_ui_test tests/ui_test.cpp)
target_include_directories(chat_qt_ui_test PRIVATE src)
target_link_libraries(chat_qt_ui_test PRIVATE chat_qt_widgets PostgreSQL::PostgreSQL)
add_test(NAME qt_widgets COMMAND chat_qt_ui_test --widgets-only)
set_tests_properties(qt_widgets PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_test(NAME qt_ui COMMAND chat_qt_ui_test $<TARGET_FILE:chat_server> "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(qt_ui PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
