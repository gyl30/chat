"""Build the standalone S3 probe against Chat's actual static libraries."""

import json
from pathlib import Path
import shlex
import subprocess
import sys

build, output = (Path(x).resolve() for x in sys.argv[1:])
line = next(
    entry
    for entry in json.loads((build / "compile_commands.json").read_text())
    if entry["file"].endswith("/src/s3_signer.cpp")
)
args = shlex.split(line["command"])
obj = output.with_suffix(".o")
args[args.index("-o") + 1] = str(obj)
args[args.index(line["file"])] = str(Path(__file__).with_name("s3_probe.cpp"))
args += ["-UNDEBUG"]
subprocess.run(args, cwd=line["directory"], check=True)

link = subprocess.check_output(
    ["ninja", "-t", "commands", "server"], cwd=build, text=True
).splitlines()[-1]
link_args = shlex.split(link.split("&&")[1])
units = [
    next(arg for arg in link_args if arg.endswith("/" + name + ".cpp.o"))
    for name in ("s3_signer", "s3_xml")
]
libraries = list(
    dict.fromkeys(
        arg
        for arg in link_args
        if arg.endswith(("/libcurl.a", "/libyxml.a", "/libssl.so", "/libcrypto.so"))
    )
)
assert len(libraries) == 4, libraries
subprocess.run(
    [
        args[0],
        str(obj),
        *units,
        *sorted(libraries, key=lambda path: path.endswith(".so")),
        "-pthread",
        "-o",
        output,
    ],
    cwd=build,
    check=True,
)
