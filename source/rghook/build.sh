#!/usr/bin/env bash
# Offline cross-build of the vendored upstream route-guidance hook (third_party/nbtevo-carplay-advanced/src/rghook.c, plain
# variant: no KOMBI/MOST code) as a QNX 6.5 / ARMv7 ET_DYN `librghook.so` for LD_PRELOAD into the Wi-Fi mm-ipod.
# Same recipe as src/rgi_hook/build.sh: clang + arm-none-eabi-ld + link-only stub of the CAR's libc.so.3 (tools.qnx_importlib
# fails if a symbol is not exported by the real library). NEVER executes the output; never touches the car.
# The soname/file name must stay `librghook.so`: rghook.c's next_sym() finds itself in LD_PRELOAD by that name.
set -eu
cd "$(dirname "$0")"
HERE="$(pwd)"
SRC="$HERE/../../third_party/nbtevo-carplay-advanced/src/rghook.c"
NBT="${NBT_REPO:?set NBT_REPO, see source/BUILDING.md}"
LIBC="$NBT/artifacts/car-collection-20261109-v5/sysroot/boot/lib/libc.so.3"
for f in "$LIBC" "$NBT/tools/qnx_importlib/make_importlib.py"; do
  [ -f "$f" ] || { echo "SKIP: missing $f (needs the nbt-evo-offline-analysis collection)"; exit 0; }
done
if [ -f "$NBT/ARTIFACTS.sha256" ]; then
  want=$(grep " $LIBC\$" "$NBT/ARTIFACTS.sha256" | cut -d' ' -f1 || true)
  [ -z "$want" ] || [ "$want" = "$(shasum -a 256 "$LIBC" | cut -d' ' -f1)" ] || { echo "FAIL: libc.so.3 hash"; exit 5; }
fi
[ "$(shasum -a 256 "$SRC" | cut -d' ' -f1)" = 0502507792c96c8d721d181baefcffde0cbcf1b2bcd0b8d706f25e9ab6c36f6a ] \
  || { echo "FAIL: vendored rghook.c differs from the reviewed upstream 9a80636 (re-review before changing this hash)"; exit 6; }

OUT=build; rm -rf "$OUT"; mkdir -p "$OUT/imp"
LIBC_SYMS="dlopen dlsym getenv memcpy memmove memset snprintf strcmp strlen usleep"
( cd "$NBT" && python3 -m tools.qnx_importlib.make_importlib \
    --libc "$LIBC" --symbols "$(echo $LIBC_SYMS | tr ' ' ,)" --out "$HERE/$OUT/imp" >/dev/null )

# upstream flags (-Os, -mno-unaligned-access, freestanding, no float) on our toolchain; -w: third-party source, kept unedited
clang --target=armv7-unknown-none-eabi -march=armv7-a -marm -mfloat-abi=softfp -mfpu=vfpv3-d16 -mno-unaligned-access \
      -fPIC -nostdlib -ffreestanding -fno-common -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
      -Os -fno-ident -w -c "$SRC" -o "$OUT/rghook.o"
echo '{ global: read; write; local: *; };' > "$OUT/hook.ver"   # export ONLY the two interposers
arm-none-eabi-ld -shared -soname librghook.so -o "$OUT/librghook.so" --version-script="$OUT/hook.ver" \
  --hash-style=sysv -z now -z text -z defs -z noexecstack -z max-page-size=0x1000 --build-id=none \
  --no-allow-shlib-undefined -L"$OUT/imp" "$OUT/rghook.o" -l:libc.so.3

H="$OUT/librghook.so"; fail() { echo "CHECK FAIL: $*"; exit 7; }
RE() { arm-none-eabi-readelf "$@" "$H"; }
RE -hW | grep -q "Type:.*DYN"                                  || fail "not ET_DYN"
RE -hW | grep -q "Machine:.*ARM"                              || fail "not ARM"
RE -lW | grep -q INTERP                                       && fail "PT_INTERP in a .so"
RE -dW | grep -q '(HASH)'                                     || fail "no DT_HASH"
RE -dW | grep -q 'GNU_HASH'                                   && fail "GNU_HASH (ldqnx 6.5 wants sysv)"
RE -dW | grep -q '(TEXTREL)'                                  && fail "TEXTREL"
RE -dW | grep -qE 'VERSYM|VERNEED|VERDEF'                     && fail "symbol versioning"
RE -dW | grep -qE '\((INIT|FINI|INIT_ARRAY|FINI_ARRAY|PREINIT_ARRAY)\)' && fail "constructors/destructors (upstream has none)"
[ "$(RE -dW | grep NEEDED | sed 's/.*\[\(.*\)\]/\1/' | tr '\n' ' ')" = "libc.so.3 " ] || fail "NEEDED != libc.so.3"
[ "$(RE -dW | awk '/SONAME/{print $5}')" = "[librghook.so]" ] || fail "SONAME"
RE -SW | grep -qE '\.(tbss|tdata|ctors|dtors)'                && fail "TLS/ctors section"
RE -lW | grep -E 'LOAD|GNU_STACK' | grep -q 'RWE'            && fail "RWX segment"
RE -rW | grep -vE 'R_ARM_(RELATIVE|JUMP_SLOT|GLOB_DAT|ABS32)|^$|Relocation|Offset|There are' | grep -q R_ARM && fail "unexpected reloc"
defs=$(RE --dyn-syms -W | awk '$7!="UND" && ($5=="GLOBAL"||$5=="WEAK"){print $8}' | grep -v '^$' | sort | tr '\n' ' ')
[ "$defs" = "read write " ] || fail "defined globals [$defs] != [read write]"
got=$(RE --dyn-syms -W | awk '$7=="UND" && $8!=""{print $8}' | sort | tr '\n' ' ')
exp=$(printf '%s\n' $LIBC_SYMS | sort | tr '\n' ' ')
[ "$got" = "$exp" ] || fail "imports [$got] != [$exp]"
echo "OK $H  sha256=$(shasum -a 256 "$H" | cut -d' ' -f1)  size=$(wc -c < "$H")  (NOT EXECUTED)"
