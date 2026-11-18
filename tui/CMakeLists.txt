set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${PROJECT_SOURCE_DIR}/third/ftxui" "${PROJECT_BINARY_DIR}/third/ftxui" EXCLUDE_FROM_ALL)
include(cmake/ftxui_unicode.cmake)

add_executable(chat_tui src/main.cpp)
target_link_libraries(chat_tui PRIVATE chat_tui_ui)
set_target_properties(chat_tui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
    INSTALL_RPATH_USE_LINK_PATH TRUE)
install(TARGETS chat_tui RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

add_library(chat_tui_state STATIC src/state.cpp)
target_include_directories(chat_tui_state PUBLIC src)
target_link_libraries(chat_tui_state PUBLIC chat_client)

add_library(chat_tui_core STATIC src/app.cpp src/messages.cpp src/profile.cpp src/groups.cpp)
target_link_libraries(chat_tui_core PUBLIC chat_tui_state Threads::Threads)
add_library(chat_tui_ui STATIC src/ui.cpp)
target_link_libraries(chat_tui_ui PUBLIC chat_tui_core ftxui::component)
