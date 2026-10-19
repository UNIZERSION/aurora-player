# Dependency sources and licenses

The firmware uses ESP-IDF 5.5.2, the official M5Stack Tab5 BSP 1.3.2, esp_lvgl_port 2.8.0, LVGL 9.3.0 and the other versions recorded in `dependencies.lock`. Copies of the vendors' license files are in `third-party/`; the presence of a license file does not mean that every optional component is linked. The original upstream licenses remain authoritative.

## ESP_AUDIO_CODEC 2.5.0

The `espressif/esp_audio_codec` component, added in player 1.2.0, comes from the official [Espressif esp-adf-libs](https://github.com/espressif/esp-adf-libs/tree/3bb83597d07b604e1ab5b78dd4370a28d6fa802d/esp_audio_codec) repository, commit `3bb83597d07b604e1ab5b78dd4370a28d6fa802d`. The metadata of the downloaded package declares version 2.5.0; it was chosen for the earlier ESP32-P4 profile in use. The application integrates the FLAC and AAC decoders, not every format the library offers. The vendor describes the API details and limitations in the [README of this version](https://github.com/espressif/esp-adf-libs/blob/3bb83597d07b604e1ab5b78dd4370a28d6fa802d/esp_audio_codec/README.md).

Copyright © 2025 Espressif Systems (Shanghai) CO., LTD. SPDX: `LicenseRef-Espressif-Modified-MIT`. The license permits use only with Espressif products; it requires the copyright and permission notices to be retained; it prohibits redistribution for use with products of other companies. The full, unmodified text is in the [LICENSE copy](third-party/managed-components/espressif__esp_audio_codec/LICENSE) and [upstream](https://github.com/espressif/esp-adf-libs/blob/3bb83597d07b604e1ab5b78dd4370a28d6fa802d/esp_audio_codec/LICENSE). The Tab5 uses an ESP32-P4. The component documentation also points to the [combined copyright information for the ESP-ADF libraries](https://docs.espressif.com/projects/esp-adf/en/latest/COPYRIGHT.html).

## Montserrat Medium: Latin extension in 1.2.1

The new glyphs U+00A0–U+017F in sizes 14, 16, 18, 20, 24 and 32 px were generated from [Montserrat-Medium.ttf from LVGL 9.3.0](https://github.com/lvgl/lvgl/blob/v9.3.0/scripts/built_in_font/Montserrat-Medium.ttf), the same source as LVGL's built-in Montserrat fonts. TTF SHA256: `421f26b23e2be6b98373d32acd3cb2897b154d4bf0a77d26534ce476e4cbed53`. Copyright 2011 The Montserrat Project Authors. The [SIL Open Font License 1.1](Montserrat-OFL.txt) applies; the full text shipped in the [pinned LVGL version](https://github.com/lvgl/lvgl/blob/v9.3.0/scripts/built_in_font/font_license/Montserrat/OFL.txt) is retained.

The source TTF and provenance data are in `tools/fonts/`. The generator `tools/generate_latin_fonts.py` uses `lv_font_conv` 1.5.3. The finished bitmaps are built into the firmware; the TTF file exists for reproducible generation and is not read from the card at runtime. The extension keeps the base ASCII glyphs and icons and adds a separate Latin set. Technical details and the limitation on kerning between the sets are described in `tools/fonts/SOURCE.txt`.

## Other sources

- [ESP-IDF 5.5.2](https://github.com/espressif/esp-idf/tree/v5.5.2): SDK, drivers, FreeRTOS and FatFs integration; the local FatFs configuration adds exFAT without modifying the SDK installation.
- [M5Stack Tab5 BSP 1.3.2](https://components.espressif.com/components/espressif/m5stack_tab5/versions/1.3.2): hardware initialization; the project's own HAL extends image presentation, battery reading and power-off.
- [LVGL 9.3](https://github.com/lvgl/lvgl/tree/v9.3.0): native widgets, the renderer and image decoders, with their own license information.
- [LVGL 9.3](https://github.com/lvgl/lvgl/tree/v9.3.0) license: MIT, copy in [third-party/managed-components/lvgl__lvgl/LICENCE.txt](third-party/managed-components/lvgl__lvgl/LICENCE.txt), together with LVGL's `COPYRIGHTS.md`.
- `main/vendor/minimp3.h`: minimp3 (lieff), CC0-1.0, license in `main/vendor/minimp3.LICENSE`; the MP3 path still uses it.
- `main/vendor/tjpgd/`: TJpgDec R0.03 by ChaN (2021), from https://elm-chan.org/fsw/tjpgd/; its permissive copyright and redistribution notice is kept in `tjpgd.c`. Local changes: fixed-width integer types and private `aurora_jd_*` symbol names. Provenance and SHA-256 in `main/vendor/ARTWORK-DECODERS.txt`.
- `main/vendor/stb_image.h`: stb_image v2.30 by Sean Barrett and contributors, MIT or public domain (the full dual license is at the end of the header). Local changes only add cancellation/yield checks; details in `main/vendor/ARTWORK-DECODERS.txt`.
- `main/vendor/tinfl_host.h`: the tinfl decompressor subset of miniz v1.14 by Rich Geldreich, public domain (Unlicense text kept in the file); used only by the host tests, the device uses the ESP32-P4 ROM through ESP-IDF.
- `main/ui_orbitron_28.c`: Orbitron Bold, Copyright 2018 The Orbitron Project Authors (https://github.com/theleagueof/orbitron), SIL Open Font License 1.1 with the Reserved Font Name "Orbitron"; license in [Orbitron-OFL.txt](Orbitron-OFL.txt).

## Interface fonts derived from Microsoft fonts

`main/aurora_fonts.c` (the `aurora_sans*`, `aurora_title31` and `aurora_mono*` faces) and `main/aurora_fx_label12.h` are bitmap glyphs rasterized from **Arial Bold** and **Consolas Bold**, fonts by Microsoft that come with Windows (generators: `tools/generate-aurora-fonts.py`, `tools/generate-fx-label-font.py`; the TTF files are not included). They are kept to preserve the original Aurora interface. They are **not** covered by this project's GPL-3.0 license; the rights in the Arial and Consolas typefaces belong to their owners. If you redistribute the firmware, check that this is permitted for you, or regenerate these files from fonts with a free license (for example Liberation Sans Bold and Liberation Mono Bold under the SIL OFL 1.1) using the same generators.

The interface raster images (`main/*.pack`, `main/*.rgb565`, `tools/aurora-reference/`) contain text that was rendered with the fonts of the browser used for the design.

## Artwork

The reference artwork and the Aurora design were created for this project by its author, partly with OpenAI's image generation (the PNG files in `main/` and `tools/aurora-reference/` keep their C2PA content credentials). They are part of this project (see the top-level [NOTICE.md](../NOTICE.md)); no Espressif or LVGL license is attributed to them.

This summary preserves the provenance and terms of the dependencies; it is not a complete legal audit.
