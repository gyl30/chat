#!/usr/bin/env bash
# Explicit, heavyweight terminal verification; deliberately not a default CTest.
set -euo pipefail
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir="$repo_root/build"
work_dir=
port=18880
soak_seconds=1800
restarts=10
keep_database=0
while (($#)); do
    case "$1" in
        --build-dir|--work-dir|--port|--soak-seconds|--restarts)
            (($# >= 2)) || { echo "Missing value for $1" >&2; exit 2; }
            case "$1" in
                --build-dir) build_dir=$2;;
                --work-dir) work_dir=$2;;
                --port) port=$2;;
                --soak-seconds) soak_seconds=$2;;
                --restarts) restarts=$2;;
            esac
            shift 2;;
        --keep-database) keep_database=1; shift;;
        --help)
            echo 'Usage: tests/tui_100_member_smoke.sh [--build-dir DIR] [--work-dir NEW_DIR] [--port N] [--soak-seconds N] [--restarts N] [--keep-database]'
            echo 'Inherits libpq PG* credentials. Creates and migrates a dedicated database; drops it on success unless retained explicitly. Failure evidence and database are retained.'
            echo 'Full verification requires soak >=1800 seconds and >=10 restart cycles. Short runs are diagnostic only.'
            exit 0;;
        *) echo "Unknown argument: $1" >&2; exit 2;;
    esac
done
for value in "$port" "$soak_seconds" "$restarts"; do
    [[ $value =~ ^[0-9]+$ ]] || { echo 'Numeric options must be nonnegative integers' >&2; exit 2; }
done
((port > 0 && port < 65536)) || { echo 'Invalid port' >&2; exit 2; }
for command in cmake python3 tmux psql createdb dropdb; do command -v "$command" >/dev/null; done
umask 077
if [[ -z $work_dir ]]; then
    work_dir=$(mktemp -d /tmp/chat-tui100.XXXXXXXX)
else
    mkdir -- "$work_dir"
fi
work_dir=$(cd -- "$work_dir" && pwd)
run_id="$(date -u +%m%d%H%M%S)_$$"
maintenance_database=${PGDATABASE:-postgres}
scale_database="chat_tui100_${run_id}"
database_created=0
child_pid=
cleanup() {
    local result=$?
    trap - EXIT INT TERM
    if [[ -n $child_pid ]] && kill -0 "$child_pid" 2>/dev/null; then
        kill -TERM "$child_pid" 2>/dev/null || true
        wait "$child_pid" || true
    fi
    if ((database_created && result == 0 && !keep_database)); then
        if dropdb --maintenance-db="$maintenance_database" "$scale_database"; then
            printf 'dropped\n' > "$work_dir/database-cleanup.txt"
        else
            printf 'drop failed; retained for inspection\n' > "$work_dir/database-cleanup.txt"
            result=1
        fi
    elif ((database_created)); then
        printf 'retained: %s\n' "$scale_database" > "$work_dir/database-cleanup.txt"
        echo "Retained isolated database: $scale_database"
    fi
    echo "Evidence: $work_dir"
    exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
cmake -S "$repo_root" -B "$build_dir" -DCHAT_BUILD_TUI_CLIENT=ON -DCHAT_BUILD_QT_CLIENT=ON -DCMAKE_BUILD_TYPE=Debug > "$work_dir/build.log" 2>&1
cmake --build "$build_dir" --target chat_tui chat_server chat_tui_scale_fixture -j12 >> "$work_dir/build.log" 2>&1
build_dir=$(cd -- "$build_dir" && pwd)
createdb --maintenance-db="$maintenance_database" --template=template0 --encoding=UTF8 "$scale_database"
database_created=1
export PGDATABASE="$scale_database"
printf '%s\n' "$scale_database" > "$work_dir/database.txt"
git -C "$repo_root" rev-parse HEAD > "$work_dir/head.txt"
for migration in "$repo_root"/sql/[0-9][0-9][0-9]_*.sql; do
    psql -X -v ON_ERROR_STOP=1 -f "$migration" >> "$work_dir/migrations.log" 2>&1
done
python3 -B "$repo_root/tests/tui_100_member_smoke.py" \
    --build-dir "$build_dir" --fixture-tool "$build_dir/chat_tui_scale_fixture" \
    --work-dir "$work_dir" --port "$port" --run-id "$run_id" \
    --soak-seconds "$soak_seconds" --restarts "$restarts" &
child_pid=$!
wait "$child_pid"
child_pid=
