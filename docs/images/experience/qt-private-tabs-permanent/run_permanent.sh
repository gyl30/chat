#!/bin/bash
set -u
phase="$1"
case "$phase" in red|green) ;; *) exit 2 ;; esac
cd /home/gyl/example/chat || exit 2
scope=/tmp/chat-private-tabs-permanent.c6pTzeeO
sdk=/tmp/chat-qt-dependency-compare.dmJWg7/install
sha256sum qt/src/group_dialog.cpp qt/src/group_dialog.hpp qt/tests/ui_test.cpp build/qt/chat_qt_ui_test build/qt/CMakeFiles/chat_qt_widgets.dir/src/group_dialog.cpp.o > "$scope/q35-$phase-inputs.sha256"
nm -C build/qt/CMakeFiles/chat_qt_widgets.dir/src/group_dialog.cpp.o | rg 'QTabWidget::(removeTab|setTabVisible|setCurrentWidget|setCurrentIndex)' > "$scope/q35-$phase-production-symbols.txt"
cp build/qt/chat_qt_ui_test "$scope/chat_qt_ui_test.$phase"
/usr/bin/time -p -o "$scope/q35-$phase-time.txt" timeout 60s env QT_QPA_PLATFORM=offscreen LD_LIBRARY_PATH="$sdk/lib" QT_PLUGIN_PATH="$sdk/plugins" build/qt/chat_qt_ui_test --widgets-only > "$scope/q35-$phase.log" 2>&1
task_exit=$?
printf '%s\n' "$task_exit" > "$scope/q35-$phase.exit"
sha256sum qt/src/group_dialog.cpp qt/src/group_dialog.hpp qt/tests/ui_test.cpp build/qt/chat_qt_ui_test build/qt/CMakeFiles/chat_qt_widgets.dir/src/group_dialog.cpp.o > "$scope/q35-$phase-output-inputs.sha256"
exit "$task_exit"
