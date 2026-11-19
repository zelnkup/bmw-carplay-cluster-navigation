#!/usr/bin/env python3
"""Offline navigation-event -> 6WA HUDTerminal adapter (mock transport only; never touches a vehicle network).

Input: one JSON event per line, e.g.
  {"event": "maneuver", "app": "google_maps", "maneuver": "turn_right", "distance_m": 250, "street": "Main Street"}
  {"event": "maneuver", "app": "apple_maps", "cp_maneuver_type": 2, "distance_m": 80}   # CPManeuverType (iOS 17.4+)
  {"event": "distance", "distance_m": 120}
  {"event": "start", "app": "google_maps", "source_supports_route_guidance": true}
  {"event": "end"}
  {"event": "tick", "t_ms": 12000}            # simulated clock, drives the timeout
  {"event": "factory_nav", "active": true}    # BMW navigation owns the terminal: yield, it clears itself

Output: the HUDTerminal call batches the proven 6WA path would send (reports/cp6wa-oncar-20261112.md), built with the
same payload builders and allowlist as tools/hud6wa_probe/hud6wa_frames.py (handles 1..6, UTF-16BE text).

Usage: python3 -I tools/cp_guide/nav_adapter.py EVENTS.jsonl
"""
import json
import math
import os
import sys
import unicodedata

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hud6wa_probe"))
import hud6wa_frames as hf  # noqa: E402  (proven builders: text, poly, be16, call_ok, handle constants)
import glyphs as gl         # noqa: E402  (maneuver glyph library: bent arrows, roundabout, u-turn, arrive)

TIMEOUT_MS = 8000          # no event for this long while guiding -> clear
GLYPH = (50, 25, 126, 101)  # 76x76 centred in the 6WA picture buffer, 176 x 126 (ImageGetSize on the car, 2026-11-19); the old
                            # (70,20..170,120) box from another car sat 32 px right of centre, into the cluster's dimmed edge
STREET_CHARS, DIST_CHARS, PUNCT = 32, 8, "-./"

# Named maneuvers -> arrow angle in degrees (0 straight, + right). None = arrival flag.
NAMED = {"straight": 0, "continue": 0, "turn_left": -90, "turn_right": 90, "slight_left": -45, "slight_right": 45,
         "keep_left": -45, "keep_right": 45, "sharp_left": -135, "sharp_right": 135, "u_turn": -180,
         "arrive": None}

# Public CPManeuverType values (CarPlay, iOS 17.4+; numbering as in mib2q-carplay-rgi MANEUVER_MAPPING.md) -> angle.
CP_TYPES = {0: 0, 5: 0, 3: 0, 11: 0, 15: 0, 16: 0, 17: 0, 51: 0, 1: -90, 2: 90, 20: -90, 21: 90,
            13: -45, 49: -45, 52: -45, 22: -45, 14: 45, 50: 45, 53: 45, 23: 45, 8: 45, 9: 45,
            47: -135, 48: 135, 4: -180, 18: -180, 26: -180, 10: None, 12: None, 24: None, 25: None, 27: None}
CP_ROUNDABOUT = set(range(28, 47)) | {6, 7, 19}   # exit angle if sent; 28..46 = exit 1..19


def maneuver_angle(ev):
    """(angle or None for arrival, exit number or 0, is_roundabout). Unknown maneuvers
    fall back to straight: never guess a turn."""
    if "cp_maneuver_type" in ev:
        t = int(ev["cp_maneuver_type"])
        # Roundabout: prefer the explicit flag the real 0x5202 carries (tlv 0x0009,
        # VERIFIED 2026-11-14 capture) over the type-value heuristic, which wrongly
        # swept in non-roundabout types (e.g. 7). Fall back to the heuristic only
        # when the flag is absent (synthetic events).
        is_round = ev.get("roundabout")
        if is_round is None:
            is_round = t in CP_ROUNDABOUT
        if is_round:
            a = ev.get("exit_angle")
            ang = 0 if a is None else max(-180, min(180, int(a)))
            ang = 5 * int(round(ang / 5.0))   # 5-degree steps: the on-HU glyph table has one slot per step
            return (-180 if t == 19 else ang), (t - 27 if 28 <= t <= 46 else 0), True
        if t == 7:                            # ExitRoundabout (not flagged as a roundabout): its own glyph
            return "exit_round", 0, False
        return CP_TYPES.get(t, 0), 0, False
    name = ev.get("maneuver", "straight")
    is_round = "roundabout" in name or "rotary" in name
    return NAMED.get(name, 0), int(ev.get("exit", 0) or 0), is_round


def _rot(pts, deg, cx, cy):
    r = math.radians(deg)
    c, s = math.cos(r), math.sin(r)
    return [(cx + (x - cx) * c - (y - cy) * s, cy + (x - cx) * s + (y - cy) * c) for x, y in pts]


def arrow_polygons(angle):
    """Filled polygons in a 0..100 design box: a stem up to the junction, then a rotated shaft + head."""
    if angle is None:                                    # arrival: a pole and a flag
        return [[(45, 100), (52, 100), (52, 10), (45, 10)], [(52, 10), (85, 22), (52, 34)]]
    jx, jy = 50, 55
    stem = [(43, 100), (57, 100), (57, jy), (43, jy)]
    head = [(43, jy), (57, jy), (57, 25), (70, 25), (50, 3), (30, 25), (43, 25)]   # pointing up, base at junction
    return [stem, _rot(head, angle, jx, jy)]


def fit(polys, box=GLYPH, bounds=None):
    """Scale the design box into the glyph box, integers only, every coordinate inside the box. bounds: a fixed design
    frame (roundabouts share one so the ring keeps its size); default = this glyph's own extent."""
    x0, y0, x1, y1 = box
    if bounds:
        mnx, mny, mxx, mxy = bounds
    else:
        pts = [p for pl in polys for p in pl]
        mnx, mxx = min(p[0] for p in pts), max(p[0] for p in pts)
        mny, mxy = min(p[1] for p in pts), max(p[1] for p in pts)
    k = min((x1 - x0) / max(mxx - mnx, 1), (y1 - y0) / max(mxy - mny, 1))
    ox = x0 + ((x1 - x0) - (mxx - mnx) * k) / 2 - mnx * k
    oy = y0 + ((y1 - y0) - (mxy - mny) * k) / 2 - mny * k
    return [[(int(round(ox + x * k)), int(round(oy + y * k))) for x, y in pl] for pl in polys]


def glyph_polys(angle, exit_no, is_round):
    """The fitted polygons drawn for a classified maneuver (shared by the adapter and the on-HU table generator)."""
    return fit(gl.glyph_for(angle, exit_no, is_round), bounds=gl.ROUND_BOUNDS if is_round else None)


def cluster_text(s, n, punct=PUNCT):
    """ASCII the 6WA font can show: accents dropped (German umlauts as ae/oe/ue/ss), comma blanks text so never kept."""
    s = s.replace("ä", "ae").replace("ö", "oe").replace("ü", "ue").replace("Ä", "Ae").replace("Ö", "Oe")
    s = s.replace("Ü", "Ue").replace("ß", "ss")
    s = unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode()
    s = "".join(c if c.isalnum() or c == " " or c in punct else " " for c in s)
    return " ".join(s.split())[:n].rstrip()


def distance_texts(m):
    """(value, unit) for the cluster: 10 m steps under 1 km (5 m under 100), then km with one decimal under 10 km."""
    if m is None or m < 0:
        return "", ""
    if m < 100:
        return str(int(round(m / 5.0)) * 5), "m"
    if m < 1000:
        return str(int(round(m / 10.0)) * 10), "m"
    if m < 10000:
        return f"{m / 1000.0:.1f}", "km"
    return str(int(round(m / 1000.0))), "km"


class MockTerminal:
    """Records calls, enforces the probe's allowlist, answers every call. Never sends anything anywhere."""

    def __init__(self):
        self.calls, self.batches = [], []

    def submit(self, label, batch):
        for fkt, p in batch:
            if not hf.call_ok(fkt, p):
                raise AssertionError(f"{label}: call {fkt:03X} {p.hex()} breaks the 6WA allowlist")
        self.calls += batch
        self.batches.append((label, batch))


class Adapter:
    def __init__(self, terminal):
        self.t, self.active, self.supported, self.factory = terminal, False, True, False
        self.visible, self.fresh, self.shown, self.last_ms, self.now, self.app = hf.BUF_B, True, None, 0, 0, ""

    def _draw(self, angle, exit_no, is_round, dist, street):
        val, unit = distance_texts(dist)
        if exit_no:
            street = f"Exit {exit_no} {street}"
        street = cluster_text(street, STREET_CHARS)
        hidden = hf.BUF_A if self.visible == hf.BUF_B else hf.BUF_B
        b = [(hf.REMOVE_ALL, hf.be16(hidden)), (hf.SET_FILL, hf.be16(hidden) + bytes([hf.FILL]))]
        b += [(hf.ADD_POLY, hf.poly(hidden, i, pl)) for i, pl in enumerate(glyph_polys(angle, exit_no, is_round))]
        b += [(hf.SWITCH_VIS, hf.be16(self.visible) + hf.be16(hidden)), (hf.REMOVE_ALL, hf.be16(self.visible))]
        if self.fresh:
            b += [(hf.SET_VIS, hf.be16(h) + b"\x01") for h in (hf.STREET, hf.DIST, hf.UNIT)]
        b += [(hf.SET_TEXT, hf.text(hf.STREET, street)), (hf.SET_TEXT, hf.text(hf.DIST, val)),
              (hf.SET_TEXT, hf.text(hf.UNIT, unit)), (hf.SET_VIS, hf.be16(hf.FRAME) + b"\x01")]
        self.t.submit("maneuver", b)
        self.visible, self.fresh, self.shown = hidden, False, (angle, exit_no, val, unit, street)

    def _clear(self, why):
        if self.shown is not None and not self.factory:
            self.t.submit(f"clear ({why})", hf.clear_batch())
        self.visible, self.fresh, self.shown, self.active = hf.BUF_B, True, None, False

    def feed(self, ev):
        kind = ev.get("event", "maneuver")
        self.now = int(ev.get("t_ms", self.now))
        if kind == "tick":
            if self.active and self.now - self.last_ms >= TIMEOUT_MS:
                self._clear("timeout")
            return
        if kind == "factory_nav":
            self.factory = bool(ev.get("active"))
            if self.factory:                     # BMW route wins; the factory clears its own panel at route end
                self.visible, self.fresh, self.shown = hf.BUF_B, True, None
            return
        self.last_ms = self.now
        if kind == "start":
            self.app = ev.get("app", "")
            self.supported = bool(ev.get("source_supports_route_guidance", True))
            self.active = self.supported
            return
        if kind == "end":
            self._clear("end")
            return
        if kind == "lanes":
            return   # decoded for capture/correlation; lane-bar rendering not built yet
        if not self.supported or self.factory:
            return
        self.active = True
        if kind == "distance":
            if self.shown is None:
                return
            val, unit = distance_texts(ev.get("distance_m"))
            b = []
            if val != self.shown[2]:
                b.append((hf.SET_TEXT, hf.text(hf.DIST, val)))
            if unit != self.shown[3]:
                b.append((hf.SET_TEXT, hf.text(hf.UNIT, unit)))
            if b:
                self.t.submit("distance", b)
                self.shown = self.shown[:2] + (val, unit) + self.shown[4:]
            return
        angle, exit_no, is_round = maneuver_angle(ev)
        self._draw(angle, exit_no, is_round, ev.get("distance_m"), ev.get("street", ""))


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: nav_adapter.py EVENTS.jsonl")
    term = MockTerminal()
    a = Adapter(term)
    with open(sys.argv[1], encoding="utf-8") as f:
        for line in f:
            if line.strip():
                a.feed(json.loads(line))
    for label, batch in term.batches:
        print(f"# {label}: {len(batch)} calls")
        for fkt, p in batch:
            print(f"  {fkt:03X} {hf.NAMES.get(fkt, '?'):<19} {p.hex(' ')}")


if __name__ == "__main__":
    main()
