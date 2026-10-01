find_package(Qt6 6.2 REQUIRED COMPONENTS Svg Widgets)

add_library(chat_qt_widgets OBJECT
    src/avatar.cpp
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
    src/icons.cpp
    src/icons.hpp
    src/main_window.cpp
    src/main_window.hpp
    src/message_data.hpp
    src/message_delegate.cpp
    src/message_delegate.hpp
    src/message_model.cpp
    src/message_model.hpp
    src/presence.hpp
    src/presence_data.hpp
    src/theme.cpp
    src/theme.hpp
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
        resources/icons/bookmark.svg
        resources/icons/chat.svg
        resources/icons/close.svg
        resources/icons/contacts.svg
        resources/icons/copy.svg
        resources/icons/groups.svg
        resources/icons/send.svg
)

set_target_properties(chat_qt_widgets PROPERTIES AUTOMOC ON)

target_link_libraries(chat_qt_widgets
    PUBLIC
        chat_client
        Qt6::Svg
        Qt6::Widgets
)

add_executable(chat_qt_models_test tests/models_test.cpp src/message_model.cpp src/conversation_model.cpp)
target_include_directories(chat_qt_models_test PRIVATE src)
target_link_libraries(chat_qt_models_test PRIVATE Qt6::Widgets)
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
add_test(NAME qt_ui COMMAND chat_qt_ui_test $<TARGET_FILE:chat_server> "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(qt_ui PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
