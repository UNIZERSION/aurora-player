#!/usr/bin/env python3
"""Preserve third-party notices from the exact SDK/dependency/toolchain trees."""
import argparse
from pathlib import Path
import shutil


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--idf', required=True, type=Path)
    parser.add_argument('--toolchain', required=True, type=Path)
    args = parser.parse_args()
    project = Path(__file__).absolute().parents[1]
    output = project / 'licenses/third-party'
    count = 0
    for prefix, source in [('esp-idf', args.idf), ('managed-components', project / 'managed_components'),
                           ('compiler-runtime', args.toolchain / 'share/licenses')]:
        for file in source.rglob('*'):
            if not file.is_file() or not file.name.upper().startswith(('LICENSE', 'LICENCE', 'COPYING', 'COPYRIGHT', 'NOTICE')):
                continue
            relative = file.relative_to(source)
            if any(part == '.git' for part in relative.parts):
                continue
            if prefix == 'esp-idf' and len(relative.parts) > 1 and relative.parts[0] != 'components':
                continue
            if prefix == 'compiler-runtime' and relative.parts[0] not in ('gcc', 'newlib', 'picolibc'):
                continue
            dest = output / prefix / relative
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(file, dest)
            count += 1
    fatfs = args.idf / 'components/fatfs/src/ff.c'
    notice = fatfs.read_text(encoding='utf-8').split('*/', 1)[0] + '*/\n'
    target = output / 'esp-idf/components/fatfs/LICENSE-from-ff.c.txt'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(notice, encoding='utf-8')
    (output / 'README.txt').write_text(
        'Notices preserved from ESP-IDF v5.5.2, managed dependency versions in dependencies.lock, '
        'and ESP GCC 14.2.0_20251107 runtime sources.\n'
        'This is a preservation of upstream license files, not a legal audit or a claim that every '
        'listed optional component is linked into the firmware.\n'
        'The original third-party sources and applicable terms remain authoritative.\n', encoding='utf-8')
    print(f'Preserved {count + 1} upstream license/notice files')


if __name__ == '__main__':
    main()
