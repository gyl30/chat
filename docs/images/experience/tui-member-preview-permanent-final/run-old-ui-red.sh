#!/usr/bin/env bash
set -u
scope=/tmp/chat-member-preview-lean.z3hVXl
cd /home/gyl/example/chat/build/tui || exit 2
/usr/bin/c++ -g "$scope/render_test.final.o" -o "$scope/red-final-old-ui" \
  -Wl,-rpath,/usr/local/lib /tmp/chat-member-preview.MMx4Oq/libchat_tui_ui.a \
  libchat_tui_core.a libchat_tui_state.a ../libchat_client.a \
  ../third/corosio/libboost_corosio.a ../third/http/libboost_http.a \
  /usr/local/lib/libboost_json.so.1.92.0 /usr/local/lib/libboost_container.so.1.92.0 \
  ../third/capy/libboost_capy.a /usr/local/lib/libboost_url.so.1.92.0 \
  /usr/lib/x86_64-linux-gnu/libcrypto.so ../third/wslay/lib/libwslay.a \
  ../third/ftxui/libftxui-component.a ../third/ftxui/libftxui-dom.a \
  ../third/ftxui/libftxui-screen.a ../_deps/utf8proc-build/libutf8proc.a \
  -lgcc_s_asneeded -lgcc_s_asneeded > "$scope/red-final-link.log" 2>&1
actual=$?
printf '%s\n' "$actual" > "$scope/red-final-link.exit"
if [ "$actual" -ne 0 ]; then exit "$actual"; fi
sha256sum "$scope/render_test.final.o" /tmp/chat-member-preview.MMx4Oq/libchat_tui_ui.a \
  /home/gyl/example/chat/tui/tests/render_test.cpp "$scope/red-final-old-ui" > "$scope/red-final-inputs.sha256"
/usr/bin/time -f '%e' -o "$scope/red-final.seconds" \
  /usr/bin/timeout --signal=KILL 5s "$scope/red-final-old-ui" > "$scope/red-final.log" 2>&1
actual=$?
printf '%s\n' "$actual" > "$scope/red-final.exit"
exit "$actual"
