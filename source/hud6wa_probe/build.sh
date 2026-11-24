#!/usr/bin/env bash
# Offline cross-build of hud6wa_probe.c with the M1 toolchain (clang + arm-none-eabi-ld + import stub of the car's
# libc.so.3 + tools/m1_1_smoketest/m1_1_start.S, whose _start calls _exit after main). Never executes the binary.
set -eu
cd "$(dirname "$0")"
HERE="$(pwd)"
REPO="$(cd ../.. && pwd)"
# Sysroot + toolchain sources live in the sibling analysis repo (read-only), like cp_capture/build.sh.
NBT="${NBT_REPO:?set NBT_REPO, see source/BUILDING.md}"
LIBC="$NBT/artifacts/car-collection-20261109-v5/sysroot/boot/lib/libc.so.3"
START="$NBT/tools/m1_1_smoketest/m1_1_start.S"
for f in "$LIBC" "$START" "$NBT/tools/qnx_importlib/make_importlib.py"; do
  [ -f "$f" ] || { echo "SKIP: missing $f (needs the nbt-evo-offline-analysis collection)"; exit 0; }
done
if [ -f "$NBT/ARTIFACTS.sha256" ]; then
  want=$(grep " $LIBC\$" "$NBT/ARTIFACTS.sha256" | cut -d' ' -f1 || true)
  [ -z "$want" ] || [ "$want" = "$(shasum -a 256 "$LIBC" | cut -d' ' -f1)" ] || { echo "FAIL: libc.so.3 hash"; exit 5; }
fi
OUT=build; rm -rf "$OUT"; mkdir -p "$OUT"
UNDEF="_exit _init_libc alarm calloc clock_gettime close dlerror dlopen dlsym nanosleep open pthread_create pthread_mutex_init pthread_mutex_lock pthread_mutex_unlock read signal write"
CFLAGS=(--target=armv7-unknown-none-eabi -march=armv7-a -mfloat-abi=softfp -mfpu=vfpv3-d16
        -mno-unaligned-access -nostdlib -ffreestanding -fno-builtin -fno-exceptions -Os -fno-ident -Wall -Werror -ffile-prefix-map="$REPO"=.)
clang "${CFLAGS[@]}" -c hud6wa_probe.c -o "$OUT/hud6wa_probe.o"
clang "${CFLAGS[@]}" -c rgi_render.c -o "$OUT/rgi_render.o"
clang "${CFLAGS[@]}" -c rgi_mem.c -o "$OUT/rgi_mem.o"
# st1ng's Mapbox shapes (third_party, unedited C++): freestanding, no exceptions/RTTI/static guards/atexit; -w: third-party code
clang++ "${CFLAGS[@]/-Werror/}" -w -std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit \
    -c rgi_shape.cpp -o "$OUT/rgi_shape.o"
clang "${CFLAGS[@]}" -c "$START" -o "$OUT/start.o"
printf '%s\n' $UNDEF > "$OUT/libc_needed.txt"
( cd "$NBT" && python3 -m tools.qnx_importlib.make_importlib --libc "$LIBC" \
    --symbols-file "$HERE/$OUT/libc_needed.txt" --out "$HERE/$OUT/importlib" >/dev/null )
arm-none-eabi-ld -o "$OUT/hud6wa_probe" --dynamic-linker=/usr/lib/ldqnx.so.2 -e _start -Ttext-segment=0x100000 \
  -z now -z noexecstack -z max-page-size=0x1000 --build-id=none --no-allow-shlib-undefined \
  -L"$OUT/importlib" "$OUT/start.o" "$OUT/hud6wa_probe.o" "$OUT/rgi_render.o" "$OUT/rgi_shape.o" "$OUT/rgi_mem.o" -l:libc.so.3
H="$OUT/hud6wa_probe"; fail() { echo "CHECK FAIL: $*"; exit 7; }
arm-none-eabi-readelf -hW "$H" | grep -q "Type:.*EXEC"                    || fail "not ET_EXEC"
arm-none-eabi-readelf -lW "$H" | grep -q "/usr/lib/ldqnx.so.2"            || fail "INTERP"
[ "$(arm-none-eabi-readelf -dW "$H" | grep NEEDED | sed 's/.*\[\(.*\)\]/\1/')" = "libc.so.3" ] || fail "NEEDED != libc.so.3"
arm-none-eabi-readelf -dW "$H" | grep -qE '\((INIT|FINI)(_ARRAY)?\)'     && fail "DT_INIT/FINI present"
arm-none-eabi-readelf -SW "$H" | grep -qE 'init_array|fini_array|tbss|tdata' && fail "init/fini/TLS section"
arm-none-eabi-readelf -lW "$H" | grep LOAD | grep -q RWE                  && fail "RWX segment"
got=$(arm-none-eabi-nm -u "$H" | awk '{print $2}' | sort | tr '\n' ' ')
exp=$(printf '%s\n' $UNDEF | sort | tr '\n' ' ')
[ "$got" = "$exp" ] || fail "imports [$got] != [$exp]"          # no exit/atexit/dlclose/__cxa_*/__aeabi_*
DIS=$(arm-none-eabi-objdump -d "$H")
echo "$DIS" | grep -qE 'bl\s.*<(exit|dlclose)@plt>'                       && fail "exit/dlclose call"
echo "$DIS" | awk '/<_start>:/{f=1} f&&/bl.*<main>/{getline; print; exit}' | grep -q '_exit@plt' || fail "_start must _exit"
echo "OK $H sha256=$(shasum -a 256 "$H" | cut -d' ' -f1) size=$(wc -c < "$H")  (NOT EXECUTED)"
