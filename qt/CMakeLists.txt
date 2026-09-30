find_package(Qt6 6.2 REQUIRED COMPONENTS Svg Widgets)

add_executable(chat_qt
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
    src/main.cpp
    src/main_window.cpp
    src/main_window.hpp
    src/message_data.hpp
    src/message_delegate.cpp
    src/message_delegate.hpp
    src/message_model.cpp
    src/message_model.hpp
    src/theme.cpp
    src/theme.hpp
)

qt_add_resources(chat_qt chat_icons
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

set_target_properties(chat_qt PROPERTIES AUTOMOC ON)

target_link_libraries(chat_qt
    PRIVATE
        chat_client
        Qt6::Svg
        Qt6::Widgets
)
