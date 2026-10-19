"""Windows/Zig host render benchmark; timings are not Tab5 frame rates.

Example: python tests/run_scroll_render_benchmark.py --zig C:/tools/zig/zig.exe
An optional --source points to an older player_ui.cpp for a baseline run.
Compare the matching *.hashes files between runs for deterministic render parity.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--zig', required=True)
parser.add_argument('--build-dir')
parser.add_argument('--source')
parser.add_argument('--cache-root')
parser.add_argument('--quick', action='store_true')
args = parser.parse_args()
project = Path(__file__).resolve().parents[1]
build_dir = Path(args.build_dir).resolve() if args.build_dir else project / 'build-host/scroll-render'
build_dir.mkdir(parents=True, exist_ok=True)
source = Path(args.source).resolve() if args.source else project / 'main/player_ui.cpp'
source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
harness = project / 'tests/scroll_render_benchmark.cpp'
lvgl = project / 'managed_components/lvgl__lvgl'
if not (lvgl / 'src').is_dir():
    raise SystemExit('Build the ESP-IDF project once to fetch the pinned LVGL component.')

# Match the shipped RGB565 software renderer and refresh period. Object sanity
# checks are disabled in firmware and would distort these host work timings.
config = (project / 'tests/lv_conf_host.h').read_text()
config = config.replace('#define LV_COLOR_DEPTH 32', '#define LV_COLOR_DEPTH 16')
config = config.replace('#define LV_USE_ASSERT_OBJ 1', '#define LV_USE_ASSERT_OBJ 0\n#define LV_DEF_REFR_PERIOD 16')
(build_dir / 'lv_conf.h').write_text(config)
assembly = '.section .rdata,"dr"\n'
for filename, symbol in [('ui_shell.pack', 'ui_shell_pack'), ('ui_dragon_thumb.rgb565', 'ui_dragon_thumb_rgb565'),
                         ('aurora_controls.pack', 'aurora_controls_pack'), ('aurora_dragon.pack', 'aurora_dragon_pack')]:
    symbol = '_binary_' + symbol
    assembly += (f'.balign 4\n.global {symbol}_start\n{symbol}_start:\n'
                 f'.incbin "{(project / "main" / filename).as_posix()}"\n'
                 f'.global {symbol}_end\n{symbol}_end:\n')
(build_dir / 'assets.S').write_text(assembly)

compiler_args = ['-O2', '-DLV_CONF_INCLUDE_SIMPLE', '-I', str(build_dir), '-I', str(project / 'main'), '-I', str(lvgl), '-x', 'c']
compiler_args += [str(p) for p in (lvgl / 'src').rglob('*.c')]
compiler_args += [str(p) for p in (project / 'main').glob('ui_montserrat_latin_*.c')]
compiler_args += [str(project / 'main/aurora_fonts.c'), '-x', 'c++', str(project / 'main/ui_fonts.cpp'),
                  str(source), str(harness), '-x', 'assembler', str(build_dir / 'assets.S'), '-o', str(build_dir / 'benchmark.exe')]
(build_dir / 'build.rsp').write_text('\n'.join('"' + value.replace('\\', '/') + '"' for value in compiler_args))
environment = dict(os.environ)
if args.cache_root:
    cache = Path(args.cache_root).resolve()
    environment.update(ZIG_GLOBAL_CACHE_DIR=str(cache / 'global'), ZIG_LOCAL_CACHE_DIR=str(cache / 'local'))
compiled = subprocess.run([args.zig, 'c++', '@' + str(build_dir / 'build.rsp')], capture_output=True, text=True, env=environment)
(build_dir / 'compile.log').write_text(compiled.stdout + compiled.stderr)
compiled.check_returncode()
if source_hash != hashlib.sha256(source.read_bytes()).hexdigest():
    raise SystemExit('Source changed while compiling; rerun required.')

scenarios = [(0, 1, True)] if args.quick else [(mode, dragon, True) for dragon in range(2) for mode in range(3)] + [(0, 1, False)]
results = []
for mode, dragon, animated in scenarios:
    name = f'mode{mode}-dragon{dragon}-anim{int(animated)}'
    result = subprocess.run([str(build_dir / 'benchmark.exe'), str(build_dir / name), str(mode), str(dragon), str(int(animated))],
                            capture_output=True, text=True)
    (build_dir / (name + '.log')).write_text(result.stdout + result.stderr)
    print(name, result.stdout + result.stderr, flush=True)
    result.check_returncode()
    results.append(json.loads(next(line for line in result.stdout.splitlines() if line.startswith('{'))))
report = {
    'result': 'PASS',
    'limitations': 'Host CPU rendering and pixel work, not physical Tab5 frame rate; synthetic covers arrive from RAM without SD I/O; real LVGL pointer processing, rendering and production player_ui source.',
    'source_sha256': source_hash,
    'harness_sha256': hashlib.sha256(harness.read_bytes()).hexdigest(),
    'scenarios': results,
}
(build_dir / 'result.json').write_text(json.dumps(report, indent=2))
print('PASS:', build_dir / 'result.json')
