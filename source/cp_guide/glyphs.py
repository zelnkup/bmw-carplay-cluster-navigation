#!/usr/bin/env python3
"""Maneuver glyph library for the 6WA cluster — filled polygons only (the
HUDTerminal renderer draws filled polygons; no strokes, single fill colour).

Each glyph is a list of polygons; each polygon is a list of (x, y) points in a
loose ~0..100 design space (nav_adapter.fit() normalises bounds into the glyph
box, so exact range doesn't matter — only relative shape). y is screen-down,
"forward" is up (toward y=0).

Design goal: proper nav-style glyphs, not one rotated arrow.
  - bent_arrow(theta): one parametric shaft→bend→head arrow that reads correctly
    for straight (0), slight (±45), turn (±90) and sharp (±135).
  - uturn, roundabout(exit), arrive: dedicated shapes.
Pick via glyph_for().
"""
import math

ROUND_R_OUT = 30.0
ROUND_R_IN = 17.0


# The 6WA leaves a 1-px gap where two polygons only touch (seen on the car 2026-11-19), so joined pieces must OVERLAP:
# a round joint at each bend (_joint) and the incoming piece pushed INTO the wider head / ring (_quad end extension).
OVERLAP = 3.0


def _quad(ax, ay, bx, by, w, ext_end=0.0):
    """A filled rectangle = a thick segment from (ax,ay) to (bx,by), width w; ext_end pushes the end further along."""
    dx, dy = bx - ax, by - ay
    L = math.hypot(dx, dy) or 1.0
    bx, by = bx + dx / L * ext_end, by + dy / L * ext_end
    px, py = -dy / L * w / 2.0, dx / L * w / 2.0
    return [(ax + px, ay + py), (bx + px, by + py), (bx - px, by - py), (ax - px, ay - py)]


def _joint(cx, cy, w):
    """Round joint (octagon, diameter w) covering the seam where two segments meet at a bend."""
    r = w / 2.0 / math.cos(math.pi / 8)
    return [(cx + r * math.cos(math.pi / 8 + k * math.pi / 4), cy + r * math.sin(math.pi / 8 + k * math.pi / 4)) for k in range(8)]


def _head(cx, cy, dx, dy, half, length):
    """Arrowhead triangle: base centred at (cx,cy) perpendicular to (dx,dy)."""
    px, py = -dy, dx
    return [(cx + px * half, cy + py * half), (cx - px * half, cy - py * half),
            (cx + dx * length, cy + dy * length)]


def bent_arrow(theta_deg, shaft_w=17.0, head_half=17.0, head_len=26.0):
    """Shaft up from the bottom, bend toward theta (0=up, +=right), then a head.
    theta 0 → straight, ±45 → slight, ±90 → turn, ±135 → sharp."""
    r = math.radians(theta_deg)
    dx, dy = math.sin(r), -math.cos(r)
    base = (50.0, 96.0)
    bend = (50.0, 50.0)
    seg = 30.0
    if abs(theta_deg) > 100:      # sharp: bend high, shaft moved away from the side it turns back to (no blob at the shaft)
        side = 1.0 if theta_deg > 0 else -1.0
        base, bend, seg = (50.0 - side * 14.0, 96.0), (50.0 - side * 14.0, 34.0), 40.0
    hbase = (bend[0] + dx * seg, bend[1] + dy * seg)
    polys = []
    if abs(theta_deg) < 1e-6:                              # straight: one shaft to the head
        polys.append(_quad(base[0], base[1], hbase[0], hbase[1], shaft_w, OVERLAP))
    else:
        polys.append(_quad(base[0], base[1], bend[0], bend[1], shaft_w))   # vertical shaft
        polys.append(_quad(bend[0], bend[1], hbase[0], hbase[1], shaft_w, OVERLAP)) # bent segment, into the head
        polys.append(_joint(bend[0], bend[1], shaft_w))
    polys.append(_head(hbase[0], hbase[1], dx, dy, head_half, head_len))
    return polys


def uturn(to_left=True):
    """Shaft up, 180° over the top, head pointing back down."""
    up_x = 62.0 if not to_left else 38.0
    dn_x = 38.0 if not to_left else 62.0
    top = 30.0
    w = 15.0
    polys = [
        _quad(up_x, 96.0, up_x, top, w),         # up shaft
        _quad(up_x, top, dn_x, top, w),          # top bar
        _quad(dn_x, top, dn_x, 66.0, w, OVERLAP),  # down shaft, into the head
        _joint(up_x, top, w), _joint(dn_x, top, w),
    ]
    polys.append(_head(dn_x, 66.0, 0.0, 1.0, 16.0, 22.0))  # head pointing down
    return polys


def _ring(cx, cy, r_out, r_in, segs=28, start=0.0, end=360.0):
    """Filled annulus sector as one concave polygon (outer arc CCW, inner arc back)."""
    a0, a1 = math.radians(start), math.radians(end)
    outer = [(cx + r_out * math.sin(a0 + (a1 - a0) * i / segs),
              cy - r_out * math.cos(a0 + (a1 - a0) * i / segs)) for i in range(segs + 1)]
    inner = [(cx + r_in * math.sin(a0 + (a1 - a0) * i / segs),
              cy - r_in * math.cos(a0 + (a1 - a0) * i / segs)) for i in range(segs, -1, -1)]
    return outer + inner


def roundabout(exit_angle_deg=0.0):
    """A ring, an entry stub at the bottom, and an exit arrow at exit_angle
    (0 = straight across/top, + = right). Entry is at the bottom (180°). Exits behind ±150° would run into the entry
    stub: drawn at ±150 (a full 180 = U-turn, which in right-hand traffic leaves on the left: -150)."""
    if abs(exit_angle_deg) >= 179.5:
        exit_angle_deg = -150.0
    exit_angle_deg = max(-150.0, min(150.0, exit_angle_deg))
    cx, cy = 50.0, 46.0
    polys = [_ring(cx, cy, ROUND_R_OUT, ROUND_R_IN)]
    # entry stub from the bottom up to the ring
    polys.append(_quad(cx, 96.0, cx, cy + ROUND_R_OUT, 13.0, OVERLAP))   # into the ring
    # exit arrow pointing outward at exit_angle
    r = math.radians(exit_angle_deg)
    dx, dy = math.sin(r), -math.cos(r)
    sx, sy = cx + dx * (ROUND_R_IN - 2), cy + dy * (ROUND_R_IN - 2)
    ex, ey = cx + dx * (ROUND_R_OUT + 10), cy + dy * (ROUND_R_OUT + 10)
    polys.append(_quad(sx, sy, ex, ey, 12.0, OVERLAP))                   # into the head
    polys.append(_head(ex, ey, dx, dy, 14.0, 15.0))
    return polys


def exit_roundabout():
    """Leave the roundabout: the ring low, no entry stub, one arrow straight up out of its top."""
    cx, cy, ro, ri = 50.0, 70.0, 24.0, 13.0
    return [_ring(cx, cy, ro, ri), _quad(cx, cy - ri + 2, cx, 30.0, 13.0, OVERLAP), _head(cx, 30.0, 0.0, -1.0, 16.0, 22.0)]


def _bounds(polys):
    xs = [x for p in polys for x, _ in p]; ys = [y for p in polys for _, y in p]
    return min(xs), min(ys), max(xs), max(ys)


# One frame for every roundabout so the ring keeps its size whatever the exit (fit() scales by this, not by each glyph)
ROUND_BOUNDS = tuple(f(v) for f, v in zip((min, min, max, max),
                     zip(*[_bounds(roundabout(a)) for a in range(-180, 181, 5)])))


def arrive():
    """Destination: a pin/flag — pole + pennant."""
    polys = [_quad(40.0, 96.0, 40.0, 14.0, 9.0)]          # pole
    polys.append([(41.0, 14.0), (84.0, 26.0), (41.0, 40.0)])  # pennant triangle, overlapping the pole (edge at 44.5)
    return polys


def glyph_for(angle, exit_no=0, is_roundabout=False):
    """Pick a glyph from the adapter's (angle, exit, roundabout) classification.
    angle None = arrival; |angle|=180 = U-turn; roundabout → ring+exit; else bent arrow."""
    if is_roundabout:
        return roundabout(0.0 if angle is None else float(angle))
    if angle == "exit_round":
        return exit_roundabout()
    if angle is None:
        return arrive()
    if abs(angle) >= 170:
        return uturn(to_left=angle < 0)
    return bent_arrow(float(angle))
