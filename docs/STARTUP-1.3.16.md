# Startup screen

Copy the supplied `Aurora/startup.jpg` to `/Aurora/startup.jpg` on the SD card.
It is optional and is not embedded in the firmware. The player also accepts
`/Aurora/startup.png`; a valid JPG takes priority. A missing, corrupt or unsupported
image leaves the previous `AURORA PLAYER <version> / Starting interface...` text
screen in place. Real library-loading progress subsequently replaces the second
line. The main panel does not appear early when the image is missing.

Use the supplied 1280 × 720 baseline JPEG for native landscape output. Other
aspect ratios are center-cropped to the display. Decoder bounds match lock-screen
images: files up to 8 MiB, baseline JPEG up to 4096 pixels per edge and 16 MP,
or PNG up to 1 MP and 8-bit channels. Startup graphics retain their colors and
are not blurred or darkened. Transparent PNG pixels flatten against black.

The existing text appears while SD mounts and the optional image decodes. The
image remains visible while settings, audio, graphics and the music catalog
initialize. A saved catalog is loaded sequentially; a necessary first scan
reports actual item and track counts. This stage does not decode every music
file or cover into memory. Album metadata and covers still load on demand.

The main LVGL screen is constructed off-screen. Its animation and input are
blocked during startup. After library completion it is populated, and an enabled
PIN is presented before the main panel can become visible. Touch and keyboard
events collected while loading are discarded, and a held touch must be released.
The splash pixels and its LVGL screen are released after the transition.

Missing SD, unreadable music and failure to create a scan task finish startup
with the existing library error notice. A catalog worker that makes no progress
for three minutes keeps the startup screen visible and exposes **RESTART PLAYER**;
it is not forcibly killed while holding files. If the worker subsequently
completes, startup finishes normally. Image decoding cooperatively stops after
15 seconds and falls back to text.

Validation: `tests/run_lock_wallpaper_tests.ps1` exercises production JPEG/PNG
decoding and LVGL startup view creation. It covers valid, absent and corrupt
artwork; JPG/PNG priority; short reads, interruptions and cancellation; unmodified
colors; hidden main controls; progress/restart controls; and preparation of a PIN
view before the destination screen is exposed. Physical SD timing, PIN transition
and boot rendering still require the hardware validation for this release.
