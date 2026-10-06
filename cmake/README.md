# Shared Unicode dependencies

The Qt client requires Qt 6.5 or newer, including Svg and Widgets. Older Qt
versions reproduce fractional-scale repaint clipping after a modal editor is
closed and predate the IBus native cursor-coordinate correction. The build
uses the supplied Qt package; it does not install Qt or patch around those
toolkit defects. Use CMAKE_PREFIX_PATH or Qt6_DIR for a separately installed SDK.
This requirement applies only when CHAT_BUILD_QT_CLIENT is enabled.

chat_unicode.cmake is enabled only when the Qt or TUI client is enabled. It
fetches utf8proc 2.12.0 / Unicode 18 once, with the original archive and offline
source pins. Qt does not run the FTXUI patch module; TUI does not find Qt.

The existing reviewed Unicode18 emoji-data.txt and modifier table remain under
tui/cmake/unicode/. Four scanner property arrays are derived deterministically
from that same pinned data during configuration, without Python or network data
fetches. The generated header is build-only. Data and derived arrays are under
Unicode License V3: distribute tui/cmake/unicode/UNICODE-LICENSE.txt with them.
The utf8proc distribution notice remains tui/cmake/utf8proc-LICENSE.md; retain it
when distributing statically linked clients.

Qt additionally fetches Google's official emoji-segmenter 0.4.0, commit
72bdc08c02be6cccdfc8cb1055fea4822a6494c1:

- Archive SHA256: 6561ffc3fe83df9f9bf07da1d02522c9b0c6878aeaf56282116b92288367e51c.
- Unmodified generated scanner SHA256: e41071624cf91187ae26396b3d2b9adda648c5afb4519ff562e7aeb9415c157d.
- Upstream Apache-2.0 LICENSE SHA256: cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30.

The archive's complete LICENSE and scanner copyright notice must accompany
distributions. Normal builds need neither Ragel nor scanner regeneration.
FETCHCONTENT_SOURCE_DIR_CHAT_EMOJI_SEGMENTER supports offline configuration;
the scanner and LICENSE pins are still verified. Dependencies are not added to
the transport/server targets or changed inside third-party submodules.
