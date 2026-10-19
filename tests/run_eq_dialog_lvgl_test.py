"""Real LVGL equalizer controls with the production modal shell."""
import argparse, hashlib, json, re, subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--zig',required=True);p.add_argument('--build',required=True,type=Path);p.add_argument('--fixture',choices=['eq_dialog_lvgl_test.cpp','fx_skin_lvgl_test.cpp'],default='eq_dialog_lvgl_test.cpp');a=p.parse_args()
root=Path(__file__).resolve().parents[1];build=a.build.resolve();build.mkdir(parents=True,exist_ok=True)
source=(root/'main/main.cpp').read_text(encoding='utf-8-sig')
enums='\n'.join(re.search(r'(?s)enum '+n+r' \{.*?\};',source).group() for n in ('ExtraAction','DialogKind'))
helpers=source[source.index('static void button_event('):source.index('static void information(')]
helpers+=source[source.index('static uint32_t keyboard_ui_key('):source.index('\nstruct Player {')]
(build/'eq_dialog_helpers.h').write_text(enums+'\n'+helpers,encoding='utf-8')
(build/'lv_conf.h').write_bytes((root/'tests/lv_conf_host.h').read_bytes())
assets='.section .rdata,"dr"\n'
for filename,symbol in [('ui_shell.pack','ui_shell_pack'),('ui_dragon_thumb.rgb565','ui_dragon_thumb_rgb565'),('aurora_controls.pack','aurora_controls_pack'),('aurora_dragon.pack','aurora_dragon_pack')]:
    assets+=f'.balign 4\n.global _binary_{symbol}_start\n_binary_{symbol}_start:\n.incbin "{(root/"main"/filename).as_posix()}"\n.global _binary_{symbol}_end\n_binary_{symbol}_end:\n'
(build/'assets.S').write_text(assets)
lv=root/'managed_components/lvgl__lvgl';exe=build/'eq-dialog-lvgl-test.exe'
args=['-O1','-DLV_CONF_INCLUDE_SIMPLE']
for include in (build,lv,root/'main'):args+=['-I',include.as_posix()]
args+=['-x','c']+[x.as_posix() for x in (lv/'src').rglob('*.c')]+[x.as_posix() for x in (root/'main').glob('ui_montserrat_latin_*.c')]+[(root/'main/aurora_fonts.c').as_posix()]
args+=['-x','c++']+[(root/f).as_posix() for f in ['main/ui_fonts.cpp','main/player_ui.cpp','tests/'+a.fixture]]+['-x','assembler',(build/'assets.S').as_posix(),'-o',exe.as_posix()]
(build/'build.rsp').write_text('\n'.join('"'+x+'"' for x in args))
compile=subprocess.run([a.zig,'c++','@'+str(build/'build.rsp')],capture_output=True,text=True)
(build/'compile.log').write_text(compile.stdout+compile.stderr);compile.check_returncode()
try:result=subprocess.run([str(exe),str(build)],capture_output=True,text=True)
except OSError as e:
    (build/'result.log').write_text('BLOCKED by operating system: '+str(e)+'\n');raise
(build/'result.log').write_text(result.stdout+result.stderr);print(result.stdout+result.stderr,end='');result.check_returncode()
files=['main/aurora_eq_dialog.hpp','main/audio_eq.h','main/eq_presets.h','main/player_ui.cpp','main/aurora_fx_skin.hpp','main/aurora_fx_label12.h','tests/'+a.fixture]
(build/'result.json').write_text(json.dumps({'result':'PASS','scope':'Real LVGL '+a.fixture,'limitations':['Preset persistence and audio DSP are tested separately. No physical display/touch hardware test.'],'source_sha256':{f:hashlib.sha256((root/f).read_bytes()).hexdigest() for f in files}},indent=2)+'\n')
