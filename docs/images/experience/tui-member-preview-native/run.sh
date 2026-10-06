#!/bin/bash
set -Eeuo pipefail
scope=/tmp/chat-member-preview-native.UivDD0NU
trap 'printf "%s\n" "$?" > "$scope/runner.exit"' EXIT
printf '%s\n' "$$" > "$scope/runner.pid"
set +x
source /tmp/chat-goal-test-env.sh
cd /home/gyl/example/chat
python3 -B - "$scope" <<'PY'
import sys,json,hashlib,subprocess
from pathlib import Path
s=Path(sys.argv[1]);r=Path.cwd();p=s/'chat_tui.after';h=hashlib.sha256(p.read_bytes()).hexdigest()
assert h=='1ecdf579af21099c08210e0d14b5383cef97342fc0a5ae0540f8877dac030350'
data={'scope':str(s),'production_head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'candidate_binary':str(p),'candidate_binary_sha256':h,'tui_source_sha256':{str(p.relative_to(r)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [r/'tui/cmake/ftxui-7.0.3-grapheme.patch',r/'tui/tests/render_test.cpp',*sorted((r/'tui/src').glob('*'))] if p.is_file()},'limits':'Focused actual4member Group Preview label/Enter fullMembers/Esc; 3widths2heights dark/light; not fullcampaign or terminalMatrix.'}
(s/'build-manifest.json').write_text(json.dumps(data,indent=2))
PY
run_profile() {
  local profile="$1"
  set +e
  python3 -B "$scope/$profile/driver.py" "$scope/$profile" mux > "$scope/$profile/driver.log" 2>&1
  local actual="$?"
  printf '%s\n' "$actual" > "$scope/$profile/driver.exit"
  return "$actual"
}
run_profile dark & dark_pid=$!
run_profile light & light_pid=$!
set +e
wait "$dark_pid"; dark_exit=$?
wait "$light_pid"; light_exit=$?
if [ "$dark_exit" -ne 0 ] || [ "$light_exit" -ne 0 ]; then exit 1; fi
exit 0
