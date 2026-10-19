#!/usr/bin/env python3
"""Package an already successful ESP-IDF build. Never contacts or flashes a device."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=Path('build'))
    parser.add_argument('--output', type=Path, default=Path('firmware'))
    parser.add_argument('--hardware-report', type=Path,
                        help='JSON recording the exact tested application SHA-256 and limited hardware test scope')
    args = parser.parse_args()
    build = args.build.absolute()
    output = args.output.absolute()
    flashing = json.loads((build / 'flasher_args.json').read_text())
    config = json.loads((build / 'config/sdkconfig.json').read_text())
    project = json.loads((build / 'project_description.json').read_text())
    if flashing['extra_esptool_args']['chip'] != 'esp32p4':
        raise SystemExit('Refusing to package a target other than esp32p4')
    app_path = build / flashing['app']['file']
    if not app_path.is_file() or not (build / 'tab5_player.elf').is_file():
        raise SystemExit('The firmware must be successfully built first')
    hardware_report = None
    if args.hardware_report:
        hardware_report = json.loads(args.hardware_report.read_text(encoding='utf-8'))
        if hardware_report.get('application_sha256') != sha256(app_path):
            raise SystemExit('Hardware report does not match this application binary')
        if not hardware_report.get('checks') or not hardware_report.get('limitations'):
            raise SystemExit('Hardware report must state checks and remaining limitations')
    source_root = Path(__file__).absolute().parents[1]
    sources = sorted(p for folder in ['main', 'components', 'cmake']
                     for p in (source_root / folder).rglob('*') if p.is_file())
    sources += [source_root / n for n in ['CMakeLists.txt', 'sdkconfig.defaults', 'partitions.csv', 'dependencies.lock']]
    newer = [str(p.relative_to(source_root)) for p in sources if p.stat().st_mtime > app_path.stat().st_mtime]
    if newer:
        raise SystemExit('Sources changed after the binary was built: ' + ', '.join(newer))
    output.mkdir(parents=True, exist_ok=True)
    entries = []
    merge_args = []
    for address, relative in sorted(flashing['flash_files'].items(), key=lambda item: int(item[0], 0)):
        name = Path(relative).name
        dst = output / name
        shutil.copy2(build / relative, dst)
        entries.append({'offset': address, 'file': name, 'size': dst.stat().st_size, 'sha256': sha256(dst)})
        merge_args += [address, str(dst)]
    settings = flashing['write_flash_args']
    subprocess.run([sys.executable, '-m', 'esptool', '--chip', 'esp32p4', 'merge_bin',
                    '-o', str(output / 'tab5-player-merged.bin'), *settings, *merge_args], check=True)
    image_info = subprocess.check_output([sys.executable, '-m', 'esptool', '--chip', 'esp32p4',
                                         'image_info', '--version', '2', str(app_path)], text=True)
    (output / 'image-info.txt').write_text(image_info, encoding='utf-8')
    shutil.copy2(build / 'config/sdkconfig.json', output / 'sdkconfig.json')
    source_hashes = {p.relative_to(source_root).as_posix(): sha256(p) for p in sources}
    merged = output / 'tab5-player-merged.bin'
    manifest = {
        'project': project['project_name'], 'version': project['project_version'],
        'chip': 'esp32p4', 'idf': '5.5.2', 'hardware_tested': hardware_report is not None,
        'hardware_test_scope': hardware_report,
        'min_chip_revision_full': config['ESP32P4_REV_MIN_FULL'],
        'max_chip_revision_full': config['ESP32P4_REV_MAX_FULL'],
        'flash_settings': flashing['flash_settings'], 'files': entries,
        'merged': {'offset': '0x0', 'file': merged.name, 'sha256': sha256(merged), 'size': merged.stat().st_size},
        'sources_sha256': source_hashes,
    }
    (output / 'build-info.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    (output / 'flash_args').write_text(' '.join(settings) + '\n' +
                                     '\n'.join(f"{entry['offset']} {entry['file']}" for entry in entries) + '\n', encoding='utf-8')
    print(f'Packaged {app_path.stat().st_size:,} byte application in {output}')


if __name__ == '__main__':
    main()
