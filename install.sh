#!/bin/sh
# carplay-6wa installer — run ON the head unit from the USB stick:   sh /mnt/<usb>/carplay-6wa/install.sh
# Copies the hook + renderer to /opt/carplay-6wa and puts the wrapper in place of mm-ipod (original kept as mm-ipod.real).
# Changes nothing else (no iap2_Wifi.cfg / Connectivity.cfg edits). Undo: uninstall.sh. Re-running updates the files.
# ROOT=<dir> runs against a copy of the HU file tree (tests only: no mount, no slay).
ROOT="${ROOT:-}"
HERE="${0%/*}"; [ "$HERE" = "$0" ] && HERE=.        # no dirname/tr/cut/head on the HU: shell + wc/grep/cp only
SYS="$ROOT/fs/sda0/opt"
D="$SYS/carplay-6wa"
MMI="$SYS/conn/bin/carplay/mm-ipod"
CFG="$SYS/conn/bin/Connectivity.cfg"
die() { echo "!! $*"; echo "!! nothing was changed"; exit 1; }
size() { set -- $(wc -c < "$1"); echo "$1"; }      # word-split drops wc's padding
listed() { while read n f; do [ "$f" = "$1" ] && echo "$n"; done < "$HERE/bin/SIZES"; }
has() { grep -e "$1" "$2" >/dev/null 2>&1; }

echo "== carplay-6wa install"
[ -f "$MMI" ] || die "no /opt/conn/bin/carplay/mm-ipod: not an NBT Evo with CarPlay?"
for f in librghook.so hud6wa_probe mm-ipod-wrapper.sh; do
    [ -r "$HERE/bin/$f" ] || die "missing bin/$f next to install.sh"
    [ "$(size "$HERE/bin/$f")" = "$(listed $f)" ] || die "bin/$f is damaged (size differs from bin/SIZES): copy the release again"
done
ours=0
if has 'carplay-6wa' "$MMI"; then
    ours=1; echo "   already installed: updating"
    [ -f "$MMI.real" ] || die "wrapper present but mm-ipod.real missing: restore mm-ipod from a backup first"
elif has '^#!' "$MMI"; then
    die "mm-ipod is already replaced by another mod's wrapper: uninstall that mod first"
fi
[ -f "$CFG" ] && { has rgi_hook "$CFG" || has librghook "$CFG" || has libnbt_carplay "$CFG"; } && \
    die "Connectivity.cfg already preloads a CarPlay hook into mm-ipod: restore the stock Connectivity.cfg first"

[ -z "$ROOT" ] && { mount -uw /fs/sda0 || die "cannot remount /fs/sda0 read-write"; }
mkdir -p "$D"
for f in librghook.so hud6wa_probe; do cp "$HERE/bin/$f" "$D/$f.new" && mv -f "$D/$f.new" "$D/$f"; done   # rename: may be running
chmod 755 "$D/hud6wa_probe"
[ $ours = 1 ] || mv "$MMI" "$MMI.real"            # rename, not copy: the Bluetooth mm-ipod is running from it
cp "$HERE/bin/mm-ipod-wrapper.sh" "$MMI"
chmod 755 "$MMI" "$MMI.real"
ok=1
for f in librghook.so hud6wa_probe; do [ "$(size "$D/$f")" = "$(size "$HERE/bin/$f")" ] || ok=0; done
[ "$(size "$MMI")" = "$(size "$HERE/bin/mm-ipod-wrapper.sh")" ] || ok=0
[ "$(size "$MMI.real")" -gt 10000 ] || ok=0
[ -z "$ROOT" ] && mount -ur /fs/sda0
if [ $ok = 1 ]; then
    echo "== installed. Power-cycle the head unit, then start a route in Apple Maps or Google Maps."
else
    echo "!! a copied file has the wrong size: run uninstall.sh, then install again"; exit 2
fi
