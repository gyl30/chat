find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)

add_executable(chat_qt
    src/chat_widget.cpp
    src/chat_widget.hpp
    src/client_bridge.cpp
    src/client_bridge.hpp
    src/conversation_data.hpp
    src/conversation_delegate.cpp
    src/conversation_delegate.hpp
    src/conversation_model.cpp
    src/conversation_model.hpp
    src/main.cpp
    src/main_window.cpp
    src/main_window.hpp
)

set_target_properties(chat_qt PROPERTIES AUTOMOC ON)

target_link_libraries(chat_qt
    PRIVATE
        chat_client
        Qt6::Widgets
)
