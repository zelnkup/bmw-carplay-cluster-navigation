# Vendored: st1ng/nbtevo-carplay-advanced — `librghook.so` source only

- Upstream: https://github.com/st1ng/nbtevo-carplay-advanced @ `9a80636b5d8c6eee259105a5e9311b912935a9c7` (2026-11-18)
- Files: `src/rghook.c`, `src/rgview.h`, `LICENSE`, `NOTICE` — byte-identical to upstream (do not edit; patch via build flags/env).
- Licence: PolyForm Noncommercial 1.0.0 (personal, non-commercial use by the car's owner). Required Notice: see `NOTICE`.

## Why
Upstream's car-tested (MINI F54, BMW F30/F31 MOST cluster) iAP2 route-guidance hook for the NBT Evo mm-ipod. It replaces our
driver-detour `rgi_hook.so`: it works on `read()`/`write()` of the transport (no driver addresses), patches EVERY
IdentificationInformation (component 30 id 0x0F0E + message ids), injects 0x5200 after 1D02 AND AA05, and substitutes each
inbound 0x52xx with an empty PowerUpdate (0xAE01) so the driver's "Invalid packet" recv-buffer leak (which we also found,
tasks/CURRENT.md 2026-11-17) never triggers.

## Review (2026-11-19, whole `rghook.c` read)
- Plain build (no `RGHOOK_KOMBI`/`RGHOOK_MOST`): no sockets, no process spawning (`spawnle` in `rg_watch_once` is compiled
  out), no HMI/MOST code. File I/O only: state file `$RGHOOK_PPS`, log `$RGHOOK_LOG`, switch files under the plugin
  settings dirs (read-only existence checks).
- Exports `read` and `write` (the interposers); the rest of the `rg_*` helpers are for upstream's variants.
- Writes the state file (open/write/close/rename) on EVERY route-guidance message, from mm-ipod's reader thread. On this car
  point `RGHOOK_PPS` at RAM (`/dev/shmem`) — flash writes on that thread stalled mm-ipod on 2026-11-14.
- Not in upstream's tested list: our build NBTevo_Y24073J. Upstream says other mm-ipod builds are "untested", not unsupported
  (no addresses used).
