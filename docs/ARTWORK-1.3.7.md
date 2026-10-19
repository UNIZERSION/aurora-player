# Artwork in Aurora 1.3.7

## Confirmed regression and fix

A locally supplied MP3 test file contains a normal ID3v2.3 APIC frame, type **0 — Other**, with a 77,374-byte, 640 × 640 baseline JPEG. Its Y, Cb and Cr components all use sampling byte `0x12`. The previous production TJpgDec rejects that legal layout during preparation with `JDR_FMT3`. Type 0 was already a fallback; it was not the cause of this failure. The image is not progressive.

A private JPEG-only stb_image fallback now decodes that sampling layout and progressive JPEG. The fast baseline path uses private upstream TJpgDec with genuine 1/2, 1/4 and 1/8 scaling. The shorter decoded edge always remains at least 312 pixels. Both paths preserve the centered square crop and return RGB565 at 312 × 312. No approved shell, dragon or control artwork is regenerated.

That file was read locally. Neither its audio nor its artwork is included in the source package or synthetic fixtures. The corrected production loader passed against that file on the host: 81,624 bytes read; 2,478,271 bytes peak JPEG scratch; no open descriptor or JPEG scratch allocation left after return. Against Pillow's independent JPEG decoder and the same crop, RMS channel error was 2.425/255, including RGB565 quantization. These are host results; they do not establish physical cover-load timing or firmware installation status.

## Container and image support

| Input | Embedded artwork | Scope |
|---|---|---|
| MP3 | ID3v2.2 `PIC`; ID3v2.3/v2.4 `APIC` | JPEG/PNG; front preferred, Other fallback |
| WAV / RIFF WAVE | `id3 ` or `ID3 ` chunk containing ID3 | May occur after a large `data` chunk; checked seeks skip audio and odd-byte padding |
| Native FLAC | Metadata block type 6 `PICTURE` | JPEG/PNG; same type ranking; metadata lengths and file bounds checked |
| AAC in ADTS | Leading ID3v2 tag | Uses the same PIC/APIC parser; does not search arbitrarily inside the audio stream |
| MP4 / M4A | None in this loader | No atom/`covr` parser is implemented; an AAC filter is not a claim of MP4 support |
| Same-folder image | `cover`, `folder`, then `front`; `.jpg`, `.jpeg`, `.png`, case-insensitive | Existing file fallback after embedded candidates |

| Image encoding | Decoder and limits |
|---|---|
| Baseline JPEG accepted by TJpgDec | Private TJpgDec; maximum 4096 pixels per dimension; scaled output when appropriate |
| Other supported baseline layouts and progressive JPEG | JPEG-only stb_image fallback; maximum dimension 4096; maximum 4,194,304 pixels; 13 MiB live decoder scratch budget |
| PNG | Existing LVGL LodePNG path; maximum dimension 2048, maximum 1,048,576 total pixels, bit depth at most 8; alpha composited over black |
| Linked picture URL | Not fetched |

Encoded images are limited to 2 MiB. The fallback's scratch budget includes allocation headers, and is separate from the encoded buffer and final 194,688-byte RGB565 image. On ESP32, fallback scratch is explicitly allocated in PSRAM. A budget or allocation failure rejects that candidate safely; it does not force a restart. JPEG cancellation checks run between MCU rows, scans, progressive finalization rows and output rows. Actual free memory can cause rejection below the configured maximum.

## Selection and bounded I/O

Candidates are ranked as front (`3`), Other (`0`), back (`4`), leaflet (`5`), media (`6`), remaining pictures, then file icons (`1`/`2`). Within one rank, file order is stable. Up to 32 candidate descriptors are retained; a failed decode tries the next ranked candidate. Lower-priority pictures are not decoded before a higher-priority valid cover.

Ordinary ID3 reads frame headers and a bounded metadata/signature probe, then only the selected encoded image. It seeks over padding and unrelated frames. The parser supports v2.3/v2.4 extended headers, grouping/data-length prefixes, v2.4 frame/tag unsynchronisation and one- or two-byte description terminators. Compressed/encrypted ID3 frames are skipped. Global v2.2/v2.3 unsynchronisation has a separate whole-tag compatibility path, capped at 2 MiB, because inserted bytes affect frame offsets.

Bounds are 1,024 ID3 frames, 256 RIFF chunks or FLAC blocks, and a 4,096-byte APIC/PIC metadata probe. An image whose description pushes its signature beyond that probe is not selected. File and enclosing chunk bounds are checked before reads/seeks. Cancellation stays latched through all fallbacks, and short reads/EINTR are handled.

Measured host read work:

| Synthetic input | Bytes read | Read calls |
|---|---:|---:|
| ID3 with 8 MiB of unrelated body/padding and a small cover | 11,181 | 7 |
| WAV with 3 MiB of audio before its ID3 cover | 11,207 | 10 |
| FLAC with 3 MiB padding and an unwanted preceding picture | 7,137 | 15 |

These measurements count I/O volume. They are not SD latency or device FPS measurements.

## Worker and visible thumbnails

The worker retains the four most recently requested main covers and shares immutable images with the displayed result using reference counts. RAM hits avoid SD probes and avoid cloning the approximately 195 KB full-size image. Background thumbnails may reuse these covers, but do not change their recency or evict them. A background miss is decoded, reduced to 56 × 56, and its full-size pixels are released. Thus scrolling preserves fast return to the last four main covers. The tradeoff is that first playing a track previously seen only as a background thumbnail requires another full decode. Explicit library refresh/stop clears caches. A bounded negative cache has 32 entries and a 30-second lifetime; a failed load can be transient, so it is not permanent evidence of missing artwork.

Up to eight visible playlist thumbnails are produced at 56 × 56 after a 180 ms viewport debounce. Main-cover requests take priority and cancel superseded background work. Audio I/O gating and the close-before-stop-acknowledgment barrier remain in force. Cache and thumbnail behavior is tested separately by the worker suite.

## Reproducible host checks

Run from the project directory with the existing Python/Pillow, Zig and LVGL paths:

```powershell
./tests/run_artwork_decode_tests.ps1 -Zig <zig.exe> -Python <python.exe> -LvglRoot ./managed_components/lvgl__lvgl -BuildDirectory ./build-host/artwork-decode
./tests/run_artwork_stop_test.ps1 -Zig <zig.exe> -LvglRoot ./managed_components/lvgl__lvgl -BuildDirectory ./build-host/artwork-stop
```

The optional `-Sample <local.mp3>` decoder argument tests the supplied regression file and compares pixels; it does not bundle the sample. The fixture generator creates all packaged test inputs from code, including an independent JPEG with all sampling factors `0x12`.

Decoder results: 40 container/image fixtures, 40 cancellation boundaries, 128 progressive truncation boundaries, short reads/EINTR, exact allocation-budget exhaustion, and five independent pixel comparisons passed. The final host configuration disables LVGL's unused automatic TJpgDec decoder, matching the firmware configuration. Maximum measured progressive scratch was 3,092,764 bytes for 640 × 640 and 7,886,236 bytes for 1024 × 1024. A 2048 × 2048 progressive case safely stopped at 12,601,558 bytes when its next allocation would exceed the budget. A 4096 × 4096 progressive case was rejected by the pixel guard.

Pillow comparison RMS including RGB565 was 3.182/255 at 1/2 scale, 3.037/255 at 1/4 and 2.494/255 at 1/8. The scaled path averages blocks, so it is not bit-identical to the old unfiltered nearest-neighbor downscale. Firmware build size, release hashes and physical testing are recorded separately when verified.

## Lossless control atlas packing

The control atlas is compressed directly from its original bytes with zlib level 9. APZ1 adds a four-byte magic and little-endian exact raw length. Only this atlas changes packing: 622,343 bytes of APK1 become 270,946 bytes of APZ1, saving 351,397 bytes. Decoded size remains 1,088,192 bytes, SHA-256 `19992426ac3ef8f000aed6bd259d0fb7ce9ee442fb520c02a8cc69620e2834b5`. The shell and dragon keep their existing APK1 files. This compression does not change pixels, opacity, dimensions, animation frames or per-frame work.

At startup the production wrapper calls the ESP32-P4 ROM's low-level `tinfl_decompress`, using the destination image as its non-wrapping dictionary. The target compiler measures the temporary state as 10,992 bytes; it is allocated on the heap, freed before return and before the following dragon allocation. Input consumption, output count and Adler checksum must all match. The one-shot call includes `HAS_MORE_INPUT` so a truncated stream cannot be completed by the old inflater's implicit zero padding. Any request for more input/output is rejected, as are trailing bytes, mismatched lengths and allocation failure. Existing graphics initialization displays its error instead of using a partially decoded atlas.

The native test/preview path uses a host-only, public-domain low-level subset of [miniz v1.14](https://github.com/richgel999/miniz/blob/v114/miniz.c). The firmware uses the pinned IDF [ROM API header](https://github.com/espressif/esp-idf/blob/v5.5.2/components/esp_rom/include/miniz.h) and exported ROM function; host tests are not a simulation of the silicon implementation. The original downloaded miniz source SHA-256 is `254342f2712562d9cd86a1c5a5d7a6875d1d9f87e56a7b39b86d847368f9bebe`.

```powershell
python ./tools/pack-assets.py --controls-only
python ./tests/test_asset_packer.py
./tests/run_assets_tests.ps1 -ZigPath <zig.exe> -BuildDir ./build-host/assets
```

The tests compare every decoded byte of all three resources (10,851,392 bytes). Additional cases cover every truncation of short fixtures, including a missing final zero Adler byte, header/checksum corruption, trailing streams, forged output lengths and buffer guard bytes. The packer passes 6,549 APK1 optimality/boundary cases and 16 APZ1 round trips across 32 KiB dictionary boundaries. Host atlas decode took approximately 3.6 ms in this run; that is not an ESP32-P4 boot-time measurement. Physical ROM startup and final firmware size must be confirmed separately.

## Primary sources and licenses

Picture types, MIME fields, descriptions and frame structure follow [ID3v2.2](https://id3.org/id3v2-00), [ID3v2.3](https://id3.org/id3v2.3.0), [ID3v2.4 frames](https://id3.org/id3v2.4.0-frames) and [ID3v2.4 structure](https://id3.org/id3v2.4.0-structure). Native FLAC pictures follow [RFC 9639, section 8.8](https://www.rfc-editor.org/rfc/rfc9639.html#section-8.8).

The private decoders use [TJpgDec R0.03](https://elm-chan.org/fsw/tjpgd/) and [stb_image v2.30](https://github.com/nothings/stb/blob/master/stb_image.h). Copyright/license notices remain in the source files. `main/vendor/ARTWORK-DECODERS.txt` records the source hashes, local configuration and cooperative-cancellation edits. Managed dependencies are unchanged.
