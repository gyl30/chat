find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)

add_executable(chat_qt
    src/client_bridge.cpp
    src/client_bridge.hpp
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
