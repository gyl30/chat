"""Launch a native Qt acceptance case in a temporary X11 session."""

import os
from pathlib import Path
import subprocess
import sys
from xml.sax.saxutils import escape


def run_x11(root, script, *args):
    runtime = root / "runtime"
    runtime.mkdir(mode=0o700)
    bus = root / "session.conf"
    bus.write_text(
        f"<busconfig><type>session</type><listen>unix:tmpdir={escape(str(runtime))}</listen>"
        '<policy context="default"><allow send_destination="*"/>'
        '<allow receive_sender="*"/><allow own="*"/></policy></busconfig>'
    )
    env = os.environ.copy()
    for key in (
        "DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "SESSION_MANAGER",
        "DBUS_SESSION_BUS_ADDRESS", "DBUS_STARTER_ADDRESS", "DBUS_STARTER_BUS_TYPE",
        "QT_IM_MODULE", "QT_IM_MODULES", "GTK_IM_MODULE", "IBUS_ADDRESS", "XMODIFIERS",
    ):
        env.pop(key, None)
    env.update(
        XDG_RUNTIME_DIR=str(runtime),
        XDG_CONFIG_HOME=str(root / "config"),
        XDG_DATA_HOME=str(root / "data"),
        XDG_CACHE_HOME=str(root / "cache"),
        XDG_SESSION_TYPE="x11",
        QT_QPA_PLATFORM="xcb",
        QT_QPA_PLATFORMTHEME="generic",
        GDK_BACKEND="x11",
        GSETTINGS_BACKEND="memory",
        GTK_USE_PORTAL="0",
        NO_AT_BRIDGE="1",
    )
    return subprocess.call(
        [
            "xvfb-run", "-a", "-f", str(root / "Xauthority"),
            "-s", "-screen 0 1280x900x24 -nolisten tcp",
            "dbus-run-session", f"--config-file={bus}", "--",
            sys.executable, "-c",
            "import sys; sys.path.insert(0, sys.argv.pop(1)); " + script,
            str(Path(__file__).resolve().parent), *map(str, args),
        ],
        env=env,
    )
