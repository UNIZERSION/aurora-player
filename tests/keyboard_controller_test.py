"""Exercise real controller keyboard methods with deterministic UI/queue mocks.

No ESP hardware or copied routing implementation. Actual LVGL focus/rendering
and the I2C worker have separate integration tests.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

p=argparse.ArgumentParser()
p.add_argument('--zig',required=True)
p.add_argument('--build',required=True,type=Path)
p.add_argument('--skip-shortcuts',action='store_true',help='Only compile/run the controller test; leave shortcut executable untouched.')
args=p.parse_args()
root=Path(__file__).resolve().parents[1]
source=(root/'main/main.cpp').read_text(encoding='utf-8-sig')
build=args.build.resolve();build.mkdir(parents=True,exist_ok=True)

def method(name):
    found=re.search(r'(?m)^    void '+name+r'\([^\n]*\)\s*\{',source)
    assert found,name
    end=source.index('\n    }',found.end())+len('\n    }')
    return source[found.start():end]

globals_source=source[source.index('static uint32_t keyboard_ui_key('):source.index('\nstruct Player {')]
close=re.search(r'(?m)^static void close_dialog\(\).*$',source).group()
enums='\n'.join(re.search(r'(?s)enum '+name+r' \{.*?\};',source).group() for name in ('ExtraAction','DialogKind'))
ui=(root/'main/player_ui.h').read_text(encoding='utf-8-sig')
ui_enum=re.search(r'(?s)typedef enum \{.*?\} player_ui_action_t;',ui).group()
assert 'search_input = input;' in method('search_dialog'), 'Search must publish the live textarea'
run=source[source.index('    void run()'):]
assert run.index('poll_keyboard();') < run.index('while (xQueueReceive(commands, &command, 0)'), 'Queued keyboard command must run before another key is consumed'

(build/'keyboard_production.h').write_text('\n'.join(method(n) for n in ('keyboard_event','poll_keyboard')),encoding='utf-8')
(build/'keyboard_globals.h').write_text(enums+'\n'+ui_enum+'\n'+close+'\n'+globals_source,encoding='utf-8')
exe=build/'keyboard_controller_test.exe'
subprocess.run([args.zig,'c++','-std=c++17','-O1','-Wall','-Wextra','-Werror',
    '-I',str(build),'-I',str(root/'main'),'-I',str(root/'components/tab5_hal/include'),
    '-I',str(root/'tests/host_audio'),str(root/'tests/keyboard_controller_test.cpp'),'-o',str(exe)],check=True)
logs=[]
def run_test(executable):
    try:
        result=subprocess.run([str(executable)],check=True,text=True,capture_output=True)
    except OSError as error:
        logs.append(f'BLOCKED by operating system: {executable.name}: {error}\n')
        (build/'results.log').write_text(''.join(logs),encoding='utf-8')
        raise
    logs.append(result.stdout+result.stderr)
    (build/'results.log').write_text(''.join(logs),encoding='utf-8')
    print(logs[-1],end='',flush=True)
run_test(exe)
shortcuts=build/'keyboard_shortcuts_test.exe'
if not args.skip_shortcuts:
    subprocess.run([args.zig,'c++','-std=c++17','-O1','-Wall','-Wextra','-Werror',
        '-I',str(root/'main'),'-I',str(root/'components/tab5_hal/include'),
        '-I',str(root/'tests/host_audio'),str(root/'tests/keyboard_shortcuts_test.cpp'),'-o',str(shortcuts)],check=True)
    run_test(shortcuts)
(build/'results.log').write_text(''.join(logs),encoding='utf-8')
files=[root/'main/main.cpp',root/'main/keyboard_shortcuts.hpp',root/'components/tab5_hal/include/tab5_keyboard.h',
       root/'components/tab5_hal/include/tab5_keyboard_protocol.h',root/'tests/keyboard_controller_test.py',
       root/'tests/keyboard_controller_test.cpp',root/'tests/keyboard_shortcuts_test.cpp']
(build/'results.json').write_text(json.dumps({
    'result':'PASS','scope':'Extracted production keyboard controller with deterministic UI/queue mocks',
    'standalone_shortcut_test':'SKIPPED' if args.skip_shortcuts else 'PASS',
    'limitations':['No physical A164, I2C timing or hardware audio test.', 'LVGL widget focus and rendering are tested separately.'],
    'sha256':{str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files}
},indent=2)+'\n',encoding='utf-8')
