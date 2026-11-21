"""Link an isolated timing probe against a Ninja Release build. No production hooks."""

import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

build, target = map(Path.resolve, map(Path, sys.argv[1:]))
assert "CMAKE_BUILD_TYPE:STRING=Release" in (build / "CMakeCache.txt").read_text()
target.parent.mkdir(parents=True, exist_ok=True)
commands = json.loads((build / "compile_commands.json").read_text())
objects = {}
header = """
#include <chrono>
#include <cstdio>
namespace timing {
using clock = std::chrono::steady_clock;
using ms = std::chrono::duration<double, std::milli>;
inline thread_local double open_ms = 0;
struct open_timer {
    clock::time_point begin = clock::now();
    ~open_timer() { open_ms += ms(clock::now()-begin).count(); }
};
inline thread_local int opens = 0, reads = 0;
template<class F> auto measure(const char* tag, clock::time_point queued, F job) {
    auto begin = clock::now();
    opens = reads = 0;
    open_ms = 0;
    auto result = job();
    auto end = clock::now();
    std::fprintf(stderr,"METRIC %s %.6f %.6f %d %d %.6f\\n",tag,ms(begin-queued).count(),ms(end-begin).count(),opens,reads,open_ms);
    return result;
}
}
"""
for name in ("get_messages_request", "send_message_request", "archive_reader"):
    unit = next(cmd for cmd in commands if cmd["file"].endswith("/src/" + name + ".cpp"))
    path = Path(unit["file"])
    source = path.read_text()
    if name == "archive_reader":
        assert source.count("        row.reset();") == 1
        source = source.replace("        row.reset();", "        timing::open_timer timer;\n        row.reset();")
        for token, counter in (("int rc = sqlite3_open_v2", "opens"), ("auto& reader = *static_cast", "reads")):
            assert source.count(token) == 1
            source = source.replace(token, f"++timing::{counter};\n    " + token)
    else:
        tag = "search" if name == "get_messages_request" else "send"
        start = "auto result =" if tag == "search" else "delivered = co_await database_.run(\n"
        pos = source.index(start, source.index('request.method == "search_all_messages"') if tag == "search" else 0)
        source = source[:pos] + "auto queued = timing::clock::now();\n        " + source[pos:]
        pat = (
            r"return (search_all_messages\(.*?\));"
            if tag == "search"
            else r"return (persist_delivery\(db,\s*\n.*?\));"
        )
        source, count = re.subn(
            pat, rf'return timing::measure("{tag}", queued, [&] {{ return \1; }});', source, flags=re.S
        )
        assert count == 1, name
    tmp = target.parent / path.name
    tmp.write_text(header + source)
    args = shlex.split(unit["command"])
    out = args.index("-o") + 1
    obj = str(target.parent / (name + ".o"))
    objects[args[out]] = obj
    args[out] = obj
    args[args.index(str(path))] = str(tmp)
    subprocess.run(args + ["-I" + str(path.parent)], cwd=unit["directory"], check=True)
line = subprocess.check_output(["ninja", "-t", "commands", "chat_server"], cwd=build, text=True).splitlines()[-1]
args = shlex.split(line.split("&&")[1])
archive = next(key for key in objects if key.endswith("/archive_reader.cpp.o"))
args.insert(args.index(next(arg for arg in args if arg.endswith("libchat_sqlite_storage.a"))), archive)
assert all(key in args for key in objects)
args[args.index("-o") + 1] = str(target)
subprocess.run([objects.get(arg, arg) for arg in args], cwd=build, check=True)
