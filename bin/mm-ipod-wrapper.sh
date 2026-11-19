#!/bin/sh
# carplay-6wa: mm-ipod wrapper (installed as /opt/conn/bin/carplay/mm-ipod; the original is kept as mm-ipod.real).
#
# Connectivity starts two mm-ipod instances from this path: Bluetooth iAP2 and CarPlay Wi-Fi. Only the CarPlay Wi-Fi one
# (its arguments name iap2_wifi) gets the route-guidance hook and the cluster renderer; the Bluetooth one runs stock.
#
# Fail-safe: the hook is first loaded into a throwaway /bin/uname. If that fails (bad file, loader error), nothing is
# preloaded and CarPlay runs exactly as stock. Disable without uninstalling: create /opt/carplay-6wa/disable (whole mod)
# or /opt/carplay-6wa/disable_renderer (keep the hook, no cluster drawing).
D=/opt/carplay-6wa
case "$*" in
    *iap2_wifi*)
        if [ ! -f "$D/disable" ] && [ -r "$D/librghook.so" ] && LD_PRELOAD="$D/librghook.so" /bin/uname >/dev/null 2>&1; then
            # librghook must come first in LD_PRELOAD (it finds the next write() after itself by that name)
            export LD_PRELOAD="$D/librghook.so${LD_PRELOAD:+:$LD_PRELOAD}"
            export RGHOOK_PPS=/tmp/carplay6wa.routeguidance   # rewritten on every guidance message: keep it in RAM
            export RGHOOK_LOG=/tmp/carplay6wa.rghook.log
            if [ ! -f "$D/disable_renderer" ] && [ -x "$D/hud6wa_probe" ]; then
                # one renderer per CarPlay session: the previous one clears the cluster on SIGTERM
                slay -s TERM hud6wa_probe >/dev/null 2>&1
                ( trap '' 1; LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib:/boot/lib \
                    "$D/hud6wa_probe" rg 86400 "$RGHOOK_PPS" </dev/null >/tmp/carplay6wa.render.log 2>&1 & )
            fi
        fi
        ;;
esac
exec /opt/conn/bin/carplay/mm-ipod.real "$@"
