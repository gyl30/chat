"""Build a Qt interaction driver using the application's actual objects and resources."""

import json
from pathlib import Path
import shlex
import subprocess
import sys

build, target = (Path(x).resolve() for x in sys.argv[1:3])
source = (
    Path(sys.argv[3]).resolve()
    if len(sys.argv) > 3
    else Path(__file__).with_name("qt_probe.cpp").resolve()
)
target.parent.mkdir(parents=True, exist_ok=True)
unit = next(
    u
    for u in json.loads((build / "compile_commands.json").read_text())
    if u["file"].endswith("/qt/src/main.cpp")
)
args = shlex.split(unit["command"])
obj = target.with_suffix(".o")
args[args.index("-o") + 1] = str(obj)
args[args.index(unit["file"])] = str(source)
# The test-only bridge interception never changes the production Qt binary.
subprocess.run(
    args + ["-UNDEBUG", "-fno-access-control", "-I" + str(Path(unit["file"]).parent)],
    cwd=unit["directory"],
    check=True,
)
if (build / "build.ninja").exists():
    line = subprocess.check_output(
        ["ninja", "-t", "commands", "client"], cwd=build, text=True
    ).splitlines()[-1]
    args = shlex.split(line.split("&&")[1])
    cwd = build
else:
    cwd = build / "qt"
    args = shlex.split((cwd / "CMakeFiles/client.dir/link.txt").read_text())
args[args.index("-o") + 1] = str(target)
(old,) = (a for a in args if a.endswith("/main.cpp.o"))
args[args.index(old)] = str(obj)
subprocess.run(args, cwd=cwd, check=True)
themes = target.parent / "themes"
if not themes.exists():
    themes.symlink_to(build / "qt/themes", target_is_directory=True)
