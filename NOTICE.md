# Aurora Player — notice

Copyright (C) 2026 UNIZERSION

Aurora Player is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, version 3 of the License only (SPDX: `GPL-3.0-only`; see
[LICENSE](LICENSE)), with the additional permission below. The exceptions listed
under "Third-party software and fonts" are not covered by this license.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

## Additional permission under GNU GPL version 3 section 7

If you modify this program, or any covered work, by linking or combining it
with the Espressif IoT Development Framework (ESP-IDF) or with libraries
distributed by Espressif Systems for use with Espressif products (or modified
versions of them), including `esp_audio_codec` and the other `espressif/*`
components pinned in [dependencies.lock](dependencies.lock), containing parts
covered by the terms of their own licenses, the licensors of this program grant
you additional permission to convey the resulting work. Corresponding Source
for a non-source form of such a combination shall include the source code for
the parts of those libraries used as well as that of the covered work, where
that source code is available.

This permission exists because the FLAC/AAC decoder library `esp_audio_codec`
may only be used with Espressif products (see its license in
[licenses/third-party/managed-components](licenses/third-party/managed-components)).
It does not change the license of those libraries.

## Authors and credits

- **UNIZERSION** — author and maintainer; the Aurora design, artwork and
  requirements. Part of the artwork was made with OpenAI's image generation
  under the author's direction (the PNG files keep their C2PA content
  credentials).
- **GPT (OpenAI Codex)** — co-author: built the foundations of the firmware, from
  the native port of the Aurora design to the audio engine, library, screen lock,
  Bluetooth bridge and most features up to version 1.3.18.
- **Claude (Anthropic)** — co-author: the 1.3.18 audit, the fixes in 1.3.19 and
  1.3.20, and the enclosure tap detector of 1.3.21.

## Third-party software and fonts

The firmware is built on ESP-IDF 5.5.2, the M5Stack Tab5 BSP, esp_lvgl_port,
LVGL 9.3, Espressif's esp_audio_codec, minimp3, TJpgDec, stb_image and the
Montserrat and Orbitron fonts (SIL OFL 1.1). Their licenses and versions are
listed in [licenses/NOTICE.md](licenses/NOTICE.md), with copies of the license
files under [licenses/](licenses). Components fetched by the ESP-IDF component
manager (`managed_components/`) are not stored in this repository; the exact
versions are pinned in [dependencies.lock](dependencies.lock).

**Interface fonts from Microsoft fonts.** To keep the original Aurora interface,
`main/aurora_fonts.c` and `main/aurora_fx_label12.h` contain bitmap glyphs
rasterized from Arial Bold and Consolas Bold (Microsoft). They are not covered by
this project's license; the rights in those typefaces belong to their owners.
Before redistributing the firmware, check that this is permitted for you, or
regenerate the two files from freely licensed fonts with the generators in
[tools/](tools) (details in [licenses/NOTICE.md](licenses/NOTICE.md)).
