# carplay-6wa — Apple CarPlay turn-by-turn on the BMW 6WA cluster (NBT Evo)

Shows the **next maneuver arrow, street and distance** from Apple CarPlay navigation (Apple Maps, Google Maps) on the
**6WA instrument cluster** of BMW cars with an **NBT Evo** head unit — where BMW's own navigation would show it.
Nothing to start: arrows appear when a CarPlay route is active and clear when it ends.

> **STATUS: EXPERIMENTAL.** Tested on: BMW F22 with 6WA cluster, NBT Evo software NBTevo_Y24073J, iPhone wireless
> CarPlay with Google Maps (Nov 2026).
> **Use at your own risk.** You modify your own head unit. If something goes wrong, `uninstall.sh` restores stock; if you
> cannot reach the head unit any more, only BMW can reflash it. No warranty, no support for bench setups.

## What you need
- BMW with **NBT Evo** and wireless **Apple CarPlay**, and the **6WA** cluster (MOST `HUDTerminal`).
  Ethernet-KOMBI cars (G-series) are covered by other projects (see Credits).
- **Root SSH access** to the head unit (not provided here).
- A USB stick (FAT32/exFAT).
- Apps: Apple Maps and Google Maps send guidance; Waze on iOS does not (it reports no route guidance).

## How it works
Two small pieces, kept apart on purpose:

| Piece | Where it runs | What it does |
|---|---|---|
| `librghook.so` (from [nbtevo-carplay-advanced](https://github.com/st1ng/nbtevo-carplay-advanced), unmodified) | inside the CarPlay Wi-Fi `mm-ipod` | tells the iPhone the car has a guidance display, asks for route guidance, decodes it, writes the current maneuver to a small file in RAM; also stops a buffer leak in the stock iPhone driver that otherwise hangs it after ~10–20 guidance messages |
| `hud6wa_probe rg` (this project) | its own process | reads that file and draws the maneuver (Mapbox-style pictures, roundabouts with their exit), the lanes to take, street and distance on the 6WA over MOST (cluster handles 1–6 only) |

A wrapper replaces `/opt/conn/bin/carplay/mm-ipod` (the original is kept as `mm-ipod.real`). It adds the hook and starts
the renderer **only for the CarPlay Wi-Fi instance**; Bluetooth runs stock. It first test-loads the hook in a throwaway
process: if that fails, CarPlay runs exactly stock. No configuration files are changed.

## Lane arrows
At junctions where the map has lane data (mostly bigger roads), a row of small lane arrows appears under the maneuver
picture. The lanes to take are **white** (cluster colour 4); the others are **dimmed** (cluster colour 2). Colour 2 was
picked on a real 6WA against its black background: colour 3 (grey) was too dim to read, colour 5 is red. The 6WA has a
fixed palette, so the colours are slot numbers, not brightness values.

## Install
1. Copy the `carplay-6wa` folder to the root of the USB stick and plug it into the car.
2. SSH to the head unit, find the stick (`ls /mnt`), then:
   ```sh
   sh /mnt/<usb>/carplay-6wa/install.sh
   ```
   It refuses (and changes nothing) if another CarPlay mod already wraps `mm-ipod` or preloads a hook.
3. Power-cycle the head unit. Start a route in Apple Maps or Google Maps.

## Uninstall / disable
- Uninstall: `sh /mnt/<usb>/carplay-6wa/uninstall.sh`, then power-cycle.
- Disable without uninstalling (as root): `touch /opt/carplay-6wa/disable` (everything) or
  `touch /opt/carplay-6wa/disable_renderer` (no cluster drawing); delete the file to re-enable. Needs `mount -uw /fs/sda0`.

## Tested versions
The renderer prints what it runs on as its first line in `/tmp/carplay6wa.render.log`
(`HU software: …, DSI library … B`). If it finds a different internal layout in BMW's DSI library it stops with
`UNSUPPORTED HEAD-UNIT SOFTWARE` before sending anything. Please report your line (works or not) in an issue.

| HU software (`nbt_version.txt`) | DSI library | Cluster | App | Result |
|---|---|---|---|---|
| NBTevo_Y24073J | 299684 B | 6WA (BMW F22) | Google Maps | works (Nov 2026) |

## Other clusters
The renderer first asks the cluster for its picture size and only draws on the 6WA family. On any other cluster it logs
`UNSUPPORTED CLUSTER` in `/tmp/carplay6wa.render.log` and draws nothing (CarPlay is unaffected). 6WB owners: please open an
issue with that log — support needs a tester.

## If no arrows appear
Logs (RAM, reset at power-off): `/tmp/carplay6wa.rghook.log` (hook: identification patched / accepted / authenticated /
StartRouteGuidanceUpdates injected), `/tmp/carplay6wa.routeguidance` (current route), `/tmp/carplay6wa.render.log`
(renderer: `[most] registered`, `82/00 hosts: 1`, one line per maneuver). The renderer steps aside while BMW navigation
uses the cluster.

## Credits and license
- **st1ng / nbtevo-carplay-advanced** — `librghook.so` (iAP2 route guidance for the NBT Evo `mm-ipod`), included unmodified.
- **Mapbox** (mapbox-navigation-ios, ISC) — the maneuver and lane drawings, via st1ng's `hudshapes.h`.
- **nanaisu / NBT-AAD** (Sq00ky/Supra-iDrive6-Android-Auto) — DSI/MOST client and `HUDTerminal` layout, restated in the renderer.
- **ftojke / BMW-iDrive6-CarPlay-HUD**, **mib2q-carplay-rgi**, **mib2high-carplay-rgi** — protocol findings that guided this work.
- Licensed under the **PolyForm Noncommercial License 1.0.0** (`LICENSE`, `NOTICE`): free for personal, non-commercial use.
  Selling it, charging for installation or bundling it with paid products is not allowed.
