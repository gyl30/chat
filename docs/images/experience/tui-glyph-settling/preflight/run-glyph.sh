#!/usr/bin/env bash
set -uo pipefail
set +x
prep_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
run_scope=${1:?explicit brand-new owned /tmp/chat-tui-glyph-run-* scope}
[[ "$run_scope" = /tmp/chat-tui-glyph-run-* && ! -e "$run_scope" ]] || exit 2
source /tmp/chat-goal-test-env.sh >/dev/null 2>&1 || exit 3
unset LD_LIBRARY_PATH LD_PRELOAD QT_PLUGIN_PATH QT_QPA_PLATFORM_PLUGIN_PATH
python3 -B "$prep_dir/glyph_probe.py" --execute \
  --reviewed-sha 7a52f8338383e937c93c9ccebeb7be35215597f0ca08308fb096a9abb3ac6863 \
  --scope "$run_scope" --port 18964 > "$prep_dir/actual-driver.log" 2>&1
actual=$?
printf '%s\n' "$actual" > "$prep_dir/actual-driver.exit"
exit "$actual"
