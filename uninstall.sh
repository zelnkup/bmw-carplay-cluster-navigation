#!/bin/sh
# carplay-6wa uninstaller — run ON the head unit:   sh /mnt/<usb>/carplay-6wa/uninstall.sh
# Puts the original mm-ipod back and removes /opt/carplay-6wa. ROOT=<dir>: tests only (no mount, no slay).
ROOT="${ROOT:-}"
SYS="$ROOT/fs/sda0/opt"
D="$SYS/carplay-6wa"
MMI="$SYS/conn/bin/carplay/mm-ipod"
die() { echo "!! $*"; exit 1; }
has() { grep -e "$1" "$2" >/dev/null 2>&1; }

echo "== carplay-6wa uninstall"
has carplay-6wa "$MMI" || die "mm-ipod is not the carplay-6wa wrapper: nothing to undo here"
[ -f "$MMI.real" ] || die "mm-ipod.real is missing: cannot restore (restore mm-ipod from a backup)"
[ -z "$ROOT" ] && { slay -s TERM hud6wa_probe >/dev/null 2>&1; mount -uw /fs/sda0 || die "cannot remount /fs/sda0 read-write"; }
mv -f "$MMI.real" "$MMI" && chmod 755 "$MMI"     # rename back over the wrapper
rm -rf "$D"
[ -z "$ROOT" ] && mount -ur /fs/sda0
has carplay-6wa "$MMI" && die "restore failed: mm-ipod is still the wrapper"
echo "== removed. Power-cycle the head unit: CarPlay is stock again."
