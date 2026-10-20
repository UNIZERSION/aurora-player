"""Compile the actual vendored method bodies against deterministic SDK mocks."""
from pathlib import Path
import subprocess
import sys
import re

root=Path(__file__).resolve().parent
source=(root.parent/'lib/ESP32-A2DP/src/BluetoothA2DPSource.cpp').read_text(encoding='utf-8')
names=('set_media_active','recover_media','service_media','bt_app_av_state_connected_hdlr','bt_app_av_media_proc','get_audio_data','get_audio_data_volume')
parts=['#include "library_media_harness.hpp"\n']
for name in names:
    start=re.search(r'(?:void|int32_t) BluetoothA2DPSource::'+name+r'\(',source).start()
    body=source.index('{',start);depth=1;end=body+1
    while depth:
        if source[end]=='{':depth+=1
        if source[end]=='}':depth-=1
        end+=1
    parts.append(source[start:end])
parts.append('#include "library_media_checks.hpp"\n')
generated=root/'library_media_generated.cpp'
generated.write_text('\n'.join(parts),encoding='utf-8')
exe=root/'library_media.exe'
subprocess.run([sys.argv[1],'c++','-std=c++17','-O2','-Wall','-Wextra','-Werror',str(generated),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
