# Aurora SD card layout

The application owns one top-level directory, `Aurora`. User music remains in
`/Music`; organizing application data does not move or rename music files.

```text
/Aurora/
  startup.jpg                  Optional full-screen startup artwork
  startup.png                  PNG alternative; JPG is tried first
  Settings/
    settings-a.bin, settings-b.bin
    eq-a.bin, eq-b.bin
    reverb-a.bin, reverb-b.bin
    favorites-a.bin, favorites-b.bin
  Cache/
    catalog.bin                Track paths and cached metadata
    catalog.bin.index          Completed discovery checkpoint
    catalog.bin.index.tmp      Present only during checkpoint replacement
  Screenshots/
    Aurora-00001.bmp, ...
  Lockscreen/
    *.jpg, *.png               User-provided lock screen backgrounds
  Legacy/                      Created only for migration recovery/conflicts
```

PIN data is stored in the private internal flash partition, not on the SD card.
Decoded covers/thumbnails are cached in RAM; they do not create another SD
directory. Startup and lock screen images are user inputs and are never modified
by migration.

## Migration from earlier releases

After mounting the card, before any settings/cache/screenshot worker starts,
`prepare_storage_layout()` moves the former `/.tab5-player` directory to
`/Aurora/Settings` with a single same-volume directory rename when possible.
It then moves the catalog and checkpoint into `Cache`. No large music, bitmap,
or catalog payload is copied during this normal migration.

When both directories already exist, settings, EQ, reverb and favourites are
resolved independently. The actual stores validate their A/B records, including
CRC, supported version and field/path checks. The highest valid sequence wins;
equal sequences prefer `Aurora/Settings`. Older/conflicting/corrupt files are
retained under unique `Aurora/Legacy` names, not overwritten or deleted. Unknown
files in an existing legacy directory remain untouched. A whole-directory fast
rename preserves unknown files inside the renamed directory as well.
Before resolving a conflict, guarded reads distinguish a transient I/O failure
from invalid file contents. An unreadable family remains in place for retry;
it is never archived merely because its store could not read it.
`persistent_read_error` then tells the controller to leave durable settings,
favourites, EQ and reverb writes disabled until a successful preparation on the
next boot/retry. A readable older fallback can still drive the UI, but must not
supersede an unknown higher sequence while its record is unreadable.

Each rename is completed on the same filesystem. If power is interrupted between
operations, the newest valid record remains in one of the two active directories;
the next boot compares both again. If the card becomes unwritable mid-migration,
the returned per-store paths select the newest readable directory, and future
boots retry. The controller must use those returned paths for both load and save,
not unconditionally assume migration succeeded. FAT filesystem damage from a
physical power cut cannot be prevented by application-level rename ordering.

Cache checkpoints never cross conflicting catalogs. A cache collision or an
interruption between catalog/checkpoint moves can require one full discovery
scan; this is preferable to accepting a stale completed-index assertion. Music
files and saved preferences are unaffected.

Only exact `Aurora-` plus five digits plus `.bmp` filenames are moved out of the
old `/Screenshots` directory. Same-named directories and unrelated files remain
there. Screenshot name collisions use `Aurora-00001-legacy-0001.bmp` etc. in the
new `Screenshots` directory, retaining both images. The old directories are
removed only when empty. Nothing is recursively deleted.

Migration runs before normal playback/UI work and outside the LVGL/audio locks.
Screenshot names are collected before renaming because changing a FAT directory
while traversing it can otherwise skip entries. Boot logs should report moved,
archived and error counts plus any legacy fallback; the UI should report failed
organization rather than claiming success if errors remain.

## Host verification

`tests/run_storage_layout_tests.ps1` compiles the actual migration and production
stores. It exercises all mutation boundaries with abrupt-stop and persistent
write-failure injection, cold-boot retry, both valid sequences, corrupt higher
sequences, ties, screenshot collisions, missing/read-only/blocked destinations,
unknown-file retention, cache checkpoint invalidation and repeated idempotent
runs. These tests verify application ordering, not physical FAT power-loss
atomicity on every SD controller.
