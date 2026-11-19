# Building
Prebuilt binaries are in `bin/` (`SHA256SUMS`). To rebuild you need clang, arm-none-eabi binutils and `libc.so.3` pulled
from your own head unit (`/proc/boot/libc.so.3`), turned into a link-only import stub; the scripts check every imported
symbol against it. `cp_guide/gen_glyph_table.py` regenerates `hud6wa_probe/glyph_table.h` from the Python glyphs.
`rghook/build.sh` builds `third_party/nbtevo-carplay-advanced/src/rghook.c` unmodified (hash-pinned).
