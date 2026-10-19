"""Exercise extracted EQ controller callbacks/methods, with deterministic I/O mocks."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--zig', required=True)
p.add_argument('--build', required=True, type=Path)
args = p.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root/'main/main.cpp').read_text(encoding='utf-8-sig')
ui = (root/'main/aurora_eq_dialog.hpp').read_text(encoding='utf-8-sig')
reverb_ui = (root/'main/aurora_reverb.inc').read_text(encoding='utf-8-sig')
build = args.build.resolve()
build.mkdir(parents=True, exist_ok=True)

def method(name):
    found = re.search(r'(?m)^    void '+name+r'\([^\n]*\)\s*\{', source)
    assert found, name
    end = source.index('\n    }', found.end())+len('\n    }')
    return source[found.start():end]

enums = '\n'.join(re.search(r'(?s)enum '+name+r' \{.*?\};', source).group()
                  for name in ('ExtraAction', 'DialogKind'))
enum_ui = re.search(r'enum EqDialogAction \{.*?\};', ui).group()
# Reverb shares the EQ input ring (kinds 100+ReverbDialogAction) since 1.3.16.
enum_reverb = re.search(r'enum ReverbDialogAction \{.*?\};', reverb_ui).group()
command = re.search(r'struct Command \{.*?\};', source).group()
ring = source[source.index('struct EqInput {'):source.index('static void ui_action(')]
(build/'eq_controller_globals.h').write_text(enums+'\n'+enum_ui+'\n'+enum_reverb+'\n'+command+'\n'+ring)
(build/'eq_controller_close.h').write_text(re.search(r'(?m)^static void close_dialog\(\).*$', source).group())
(build/'eq_controller_methods.h').write_text('\n'.join(method(name) for name in
    ('save', 'eq_refresh', 'eq_apply', 'poll_eq_edits', 'eq_action', 'reverb_action')))
exe = build/'eq_controller_test.exe'
fx_obj = build/'audio_fx.o'
subprocess.run([args.zig, 'cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-c',
                str(root/'main/audio_fx.c'), '-o', str(fx_obj)], check=True)
subprocess.run([args.zig, 'c++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                '-I', str(build), '-I', str(root/'main'),
                str(root/'tests/eq_controller_test.cpp'), str(fx_obj), '-o', str(exe)], check=True)
try:
    run = subprocess.run([str(exe)], capture_output=True, text=True)
except OSError as error:
    (build/'result.log').write_text('BLOCKED by operating system: '+str(error)+'\n')
    raise
log = run.stdout+run.stderr
(build/'result.log').write_text(log)
print(log, end='', flush=True)
files = [root/'main/main.cpp', root/'main/aurora_eq_dialog.hpp', root/'main/eq_store.h', root/'main/eq_presets.h', root/'main/audio_level_policy.h',
         root/'main/aurora_reverb.inc', root/'main/audio_fx.h', root/'main/audio_fx.c',
         Path(__file__), root/'tests/eq_controller_test.cpp']
(build/'result.json').write_text(json.dumps({
    'result': 'PASS' if run.returncode == 0 else 'FAIL', 'exit_code': run.returncode,
    'scope': 'Actual extracted EQ/Reverb input callbacks, lock gate, ordered ring, poll, EQ/Reverb action and save controller methods',
    'limitations': ['Audio, SD and LVGL are deterministic mocks; separate DSP/store/LVGL suites cover those implementations.',
                    'No USB or hardware timing tests.'],
    'sha256': {str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files}
}, indent=2)+'\n')
raise SystemExit(run.returncode)
