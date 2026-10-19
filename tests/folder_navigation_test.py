"""Extract real folder controller paths and link the actual library/favorites code."""
import argparse
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--zig", required=True)
parser.add_argument("--build", required=True, type=Path)
parser.add_argument("--source", type=Path)
parser.add_argument("--expect-pending-regression", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (args.source or root / "main/main.cpp").read_text(encoding="utf-8-sig")
build = args.build.resolve()
build.mkdir(parents=True, exist_ok=True)

def method(name):
    found = re.search(r"(?m)^    [^\n;{}]*\b" + name + r"\([^\n]*\)[^\n{]*\{", source)
    if not found:
        raise ValueError(f"Missing controller method {name}")
    end = source.index("\n    }", found.end()) + len("\n    }")
    return source[found.start():end]

def action(name, end_name, wrapper):
    start = source.index("        case " + name + ":")
    end = source.index("        case " + end_name + ":", start)
    return "void " + wrapper + "(int value){Command c{" + name + ",value};switch(c.action){\n" + source[start:end] + "default:assert(false);}}\n"

methods = "\n".join(method(n) for n in ("track", "queue_index", "play_queue", "rebuild_queue", "filter", "play",
                                        "skip_keeps_paused", "next", "previous"))
methods += action("PLAYER_UI_FOLDER_SELECT", "PLAYER_UI_RESCAN", "select_folder")
methods += action("PLAYER_UI_LIBRARY_TAB", "PLAYER_UI_FOLDER", "set_tab")
methods += action("PLAYER_UI_SHUFFLE", "PLAYER_UI_REPEAT", "toggle_shuffle")
methods += action("PLAYER_UI_FAVORITE", "PLAYER_UI_PLAY_PAUSE", "toggle_favorite")
methods += action("PLAYER_UI_FORMAT_FILTER", "PLAYER_UI_FAVORITE", "set_format")
(build / "folder_production.h").write_text(methods, encoding="utf-8")
audio = build / "audio_file.o"
subprocess.run([args.zig,"cc","-std=c11","-O1","-I",str(root/"main"),"-c",str(root/"main/audio_file.c"),"-o",str(audio)],check=True)
exe = build / "folder_navigation_test.exe"
subprocess.run([args.zig,"c++","-std=c++17","-O1","-Wall","-Wextra","-Werror","-I",str(root/"main"),"-I",str(build),
                "-I",str(root/"tests/host_audio"),
                str(root/"tests/folder_navigation_test.cpp"),str(root/"main/music_library.cpp"),str(root/"main/favorites_store.cpp"),
                str(root/"main/player_store.cpp"),str(audio),"-o",str(exe)],check=True)
result = subprocess.run([str(exe)])
expected = 2 if args.expect_pending_regression else 0
assert result.returncode == expected, f"Expected exit {expected}, got {result.returncode}"
if args.expect_pending_regression:
    print("CONFIRMED production pending-play/folder-switch regression.", flush=True)
