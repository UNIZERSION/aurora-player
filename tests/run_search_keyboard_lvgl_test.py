"""Extract the production dialog/controller; compile and run with real LVGL."""
import argparse, hashlib, json, re, subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--zig',required=True);p.add_argument('--build',required=True,type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[1];build=a.build.resolve();build.mkdir(parents=True,exist_ok=True)
source=(root/'main/main.cpp').read_text(encoding='utf-8-sig')
def method(name):
    start=re.search(r'(?m)^    void '+name+r'\([^\n]*\)\s*\{',source);assert start,name
    end=source.index('\n    }',start.end())+len('\n    }');return source[start.start():end]
enums='\n'.join(re.search(r'(?s)enum '+n+r' \{.*?\};',source).group() for n in ('ExtraAction','DialogKind'))
dialog=source[source.index('static void button_event('):source.index('static void information(')]
keys=source[source.index('static uint32_t keyboard_ui_key('):source.index('\nstruct Player {')]
(build/'search_ui_globals.h').write_text(enums+'\n'+dialog+'\n'+keys,encoding='utf-8')
(build/'search_ui_methods.h').write_text(method('search_dialog')+'\n'+method('keyboard_event'),encoding='utf-8')
assert 'case PLAYER_UI_SEARCH: search_dialog(); break;' in source
(build/'lv_conf.h').write_bytes((root/'tests/lv_conf_host.h').read_bytes())
assets='.section .rdata,"dr"\n'
for filename,symbol in [('ui_shell.pack','ui_shell_pack'),('ui_dragon_thumb.rgb565','ui_dragon_thumb_rgb565'),('aurora_controls.pack','aurora_controls_pack'),('aurora_dragon.pack','aurora_dragon_pack')]:
    assets+=f'.balign 4\n.global _binary_{symbol}_start\n_binary_{symbol}_start:\n.incbin "{(root/"main"/filename).as_posix()}"\n.global _binary_{symbol}_end\n_binary_{symbol}_end:\n'
(build/'assets.S').write_text(assets)
lv=root/'managed_components/lvgl__lvgl';exe=build/'search-keyboard-lvgl-test.exe'
args=['-O1','-DLV_CONF_INCLUDE_SIMPLE']
for include in (build,lv,root/'main',root/'components/tab5_hal/include',root/'tests/host_audio'):args+=['-I',include.as_posix()]
args+=['-x','c']+[x.as_posix() for x in (lv/'src').rglob('*.c')]+[x.as_posix() for x in (root/'main').glob('ui_montserrat_latin_*.c')]+[(root/'main/aurora_fonts.c').as_posix()]
args+=['-x','c++']+[(root/f).as_posix() for f in ['main/ui_fonts.cpp','main/player_ui.cpp','tests/search_keyboard_lvgl_test.cpp']]+['-x','assembler',(build/'assets.S').as_posix(),'-o',exe.as_posix()]
(build/'build.rsp').write_text('\n'.join('"'+x+'"' for x in args))
compile=subprocess.run([a.zig,'c++','@'+str(build/'build.rsp')],capture_output=True,text=True)
(build/'compile.log').write_text(compile.stdout+compile.stderr);compile.check_returncode()
try:
    result=subprocess.run([str(exe),str(build)],capture_output=True,text=True)
except OSError as e:
    (build/'result.log').write_text('BLOCKED by operating system: '+str(e)+'\n');raise
(build/'result.log').write_text(result.stdout+result.stderr);print(result.stdout+result.stderr,end='');result.check_returncode()
(build/'result.json').write_text(json.dumps({'result':'PASS','scope':'Real LVGL + extracted production search/new_dialog/keyboard_event','limitations':['No physical I2C keyboard input or display-lock contention simulated.'],'source_sha256':hashlib.sha256(source.encode()).hexdigest()},indent=2)+'\n')
