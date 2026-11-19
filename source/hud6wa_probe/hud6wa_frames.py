#!/usr/bin/env python3
"""Host-only builder/checker for the one-arrow 6WA HUDTerminal probe. No I/O beyond stdout.

Builds the exact MOST HUDTerminal (FBlock 0x82, inst 0x00) call sequence the on-device probe would send
(draw: one straight arrow + "250 m"; then clear), checks every call against the same allowlist the
Supra driver enforces, and prints the DSI processMessage body for review. It cannot send anything.

Protocol facts are restated (not copied) from Sq00ky/Supra-iDrive6-Android-Auto @ 9af2284, milestone-7:
  src/cluster/hudterm.hpp:43-52,159-169,233-253   function ids, payload shapes, limits
  src/cluster/hud6wa_profile.hpp:44-55             6WA instances 1..6, fill 4, glyph box 70,20..170,120
  src/cluster/hud6wa_display.cpp:66-101            draw/clear batch order (car-tested on F20 6WA)
  src/most/msg.hpp:128-143                         processMessage body layout (little-endian header)
TextareaSetText (0x210) encoding is NOT settled by raw data (reports/hudterminal-protocol-validation.md):
  H-BE  handle, 00, UTF-16BE chars, 00 00 (answer drops the 00 00)   - merged hudterm.hpp:17-18; this builder
  H-LE  handle, n_chars, n x UTF-16LE                                 - issue #52 comments (F60, F20)
--decode classifies captured 0x210 frames against both; only a non-empty frame can decide.
Usage: python3 -I hud6wa_frames.py [--street TEXT] [--dist TEXT] [--unit TEXT] [--host HEX]
       python3 -I hud6wa_frames.py --decode CAPTURE.log     (nbt-aad / test_controls "MOST ..." lines)
"""
import argparse
import collections
import re
import struct

FBLOCK, INST, OP_START_RESULT_ACK = 0x82, 0x00, 0x6
SET_VIS, SWITCH_VIS, SET_TEXT, SET_FILL, ADD_POLY, REMOVE_ALL = 0x204, 0x20E, 0x210, 0x21B, 0x21D, 0x220
NAMES = {SET_VIS: "SetVisibility", SWITCH_VIS: "SwitchVisibility", SET_TEXT: "TextareaSetText",
         SET_FILL: "ImageSetFillColor", ADD_POLY: "ImageAddPolygon", REMOVE_ALL: "ImageRemoveAllItems",
         0x219: "ImageSetLineWidth", 0x227: "PotentiometerSetValue"}   # last two: decode only, never sent
FRAME, STREET, DIST, UNIT, BUF_A, BUF_B, FILL = 1, 2, 3, 4, 5, 6, 4      # hud6wa_profile.hpp:44-55
ALLOWED_HANDLES = {FRAME, STREET, DIST, UNIT, BUF_A, BUF_B}               # never 7..10, 22 (factory layers)
MAX_POINTS, MAX_TEXT = 99, 32
# Straight-on arrow, 60 x 100 centred on (120,70): the box the Supra author saw drawn whole on a 6WA.
ARROW = [(110, 120), (130, 120), (130, 60), (150, 60), (120, 20), (90, 60), (110, 60)]


def be16(x):
    return struct.pack(">H", x & 0xFFFF)


def text(h, s):
    if len(s) > MAX_TEXT or any(not (0x20 <= ord(c) < 0x7F) or c == "," for c in s):
        raise ValueError(f"text {s!r}: <=32 printable ASCII, no comma (a comma blanks the 6WA text)")
    return be16(h) + b"\x00" + b"".join(be16(ord(c)) for c in s) + b"\x00\x00"


def poly(h, idx, pts):
    return be16(h) + be16(idx) + bytes([3, len(pts)]) + b"".join(be16(x) + be16(y) for x, y in pts)


def shape_ok(fkt, p):
    """Mirror of hudterm.hpp payloadShapeOk for the functions this probe uses."""
    n = len(p)
    if fkt == SET_VIS:
        return n == 3 and p[2] <= 1
    if fkt in (SET_FILL,):
        return n == 3
    if fkt == REMOVE_ALL:
        return n == 2
    if fkt == SWITCH_VIS:
        return n == 4
    if fkt == SET_TEXT:
        return 5 <= n <= 3 + 2 * (MAX_TEXT + 1) and (n - 3) % 2 == 0 and p[2] == 0 and p[-2:] == b"\x00\x00"
    if fkt == ADD_POLY:
        return n >= 10 and p[4] in (2, 3) and 1 <= p[5] <= MAX_POINTS and n == 6 + 4 * p[5]
    return False


def call_ok(fkt, p):
    hs = [struct.unpack(">H", p[:2])[0]] + ([struct.unpack(">H", p[2:4])[0]] if fkt == SWITCH_VIS else [])
    return shape_ok(fkt, p) and all(h in ALLOWED_HANDLES for h in hs)


def draw_batch(street, dist, unit):
    """hud6wa_display.cpp showBatch with fresh_=true, visible buffer = 6 (where factory nav leaves it)."""
    return [(REMOVE_ALL, be16(BUF_A)), (SET_FILL, be16(BUF_A) + bytes([FILL])),
            (ADD_POLY, poly(BUF_A, 0, ARROW)), (SWITCH_VIS, be16(BUF_B) + be16(BUF_A)),
            (REMOVE_ALL, be16(BUF_B)),
            (SET_VIS, be16(STREET) + b"\x01"), (SET_VIS, be16(DIST) + b"\x01"), (SET_VIS, be16(UNIT) + b"\x01"),
            (SET_TEXT, text(STREET, street)), (SET_TEXT, text(DIST, dist)), (SET_TEXT, text(UNIT, unit)),
            (SET_VIS, be16(FRAME) + b"\x01")]


def clear_batch():
    """hud6wa_display.cpp clearBatch: the state factory nav leaves at the end of a route."""
    return [(SET_VIS, be16(FRAME) + b"\x00"), (REMOVE_ALL, be16(BUF_A)), (REMOVE_ALL, be16(BUF_B)),
            (SWITCH_VIS, be16(BUF_A) + be16(BUF_B)),
            (SET_TEXT, text(STREET, "")), (SET_TEXT, text(DIST, "")), (SET_TEXT, text(UNIT, "")),
            (SET_VIS, be16(STREET) + b"\x00")]


def most_body(dst, fkt, data, tag):
    """msg.hpp buildMessage: txType, varTag=1, varVal, dst, src(0: server fills from registration), fblock, inst,
    fkt, op, hdrTag, len, data. The DSI request (id 0x2) prepends the u32 clientId."""
    # putLe32 aligns to 4: op at +20, 3 pad bytes, hdrTag at +24, len at +28, data at +32 (msg.hpp parseView)
    return (struct.pack("<IIIHHBBHB3xII", 0, 1, 0, dst, 0, FBLOCK, INST, fkt, OP_START_RESULT_ACK, tag, len(data))
            + data)


def classify_text(p):
    """Which 0x210 encodings a payload (call, or the cluster's echo) fits: set of 'BE', 'LE'; 'empty' if it can't tell."""
    if len(p) < 3:
        return {"malformed"}
    if len(p) == 3 and p[2] == 0:
        return {"empty"}                       # 'h 00' under both: the one case the F20 capture has
    fits = set()
    body = p[3:]
    if p[2] == 0 and len(body) % 2 == 0:       # BE, terminator optional (the echo leaves it out)
        if body[-2:] == b"\x00\x00":
            body = body[:-2]
        if body and all(body[i] == 0 and 0x20 <= body[i + 1] < 0x7F for i in range(0, len(body), 2)):
            fits.add("BE")
    n = p[2]
    if n and len(p) == 3 + 2 * n and all(0x20 <= p[i] < 0x7F and p[i + 1] == 0 for i in range(3, len(p), 2)):
        fits.add("LE")
    return fits or {"neither"}


MOST_LINE = re.compile(r"src=(?P<src>[0-9A-Fa-f]{4}).*FB=82 inst=00 fkt=(?P<fkt>[0-9A-Fa-f]{3}) "
                       r"op=\S*\((?P<op>[0-9A-F])\).*len=(?P<len>\d+): (?P<hex>[0-9a-f ]+)")


def decode(path):
    """Summarise FB 82/00 lines of a capture: hosts, ops, handles per function, and the 0x210 verdicts."""
    hosts, dsts, ops, handles, verdicts = set(), set(), collections.Counter(), collections.defaultdict(set), []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = MOST_LINE.search(line)
            if not m:
                continue
            p = bytes.fromhex(m["hex"])
            if len(p) != int(m["len"]):
                continue                       # a truncated dump line (48-byte cap) proves nothing
            fkt = int(m["fkt"], 16)
            hosts.add(m["src"])
            dsts.update(re.findall(r"dst=([0-9A-Fa-f]{4})", line))
            ops[m["op"]] += 1
            handles[fkt].add(struct.unpack(">H", p[:2])[0] if len(p) >= 2 else -1)
            if fkt == SET_TEXT:
                verdicts.append((tuple(sorted(classify_text(p))), p.hex(" ")))
    print(f"FB82/00 frames: {sum(ops.values())}  src: {sorted(hosts)}  dst: {sorted(dsts)}  ops: {dict(ops)}")
    for fkt in sorted(handles):
        print(f"  {fkt:03X} {NAMES.get(fkt, '?'):<19} handles {sorted(handles[fkt])}")
    tally = collections.Counter(v for v, _ in verdicts)
    print(f"0x210 frames: {len(verdicts)}  verdicts: {dict(tally)}")
    for v, h in verdicts:
        if v != ("empty",):
            print(f"  {'/'.join(v):<8} {h}")
    decided = {v for v in tally if v in (("BE",), ("LE",))}
    print("text encoding:", "UNDECIDED (no non-empty 0x210 frame)" if not decided else
          "CONFLICT" if len(decided) > 1 else f"{decided.pop()[0]} supported by capture")


def selfcheck():
    # upstream vectors, tests/test_hudterm.cpp:61-62 @ 9af2284 (synthetic, not car captures)
    assert text(3, "12") == bytes.fromhex("0003 00 0031 0032 0000") and text(2, "") == bytes.fromhex("0002 00 0000")
    assert classify_text(text(DIST, "250")) == {"BE"}
    assert classify_text(text(DIST, "250")[:-2]) == {"BE"}          # the cluster's echo (merged hudterm.hpp:18)
    assert classify_text(be16(DIST) + b"\x03" + "250".encode("utf-16-le")) == {"LE"}
    assert classify_text(bytes.fromhex("0003 00")) == {"empty"}       # the only 0x210 shape in the F20 capture
    assert classify_text(be16(DIST) + b"\x00" + "25".encode("utf-16-le")) == {"neither"}
    for fkt, p in draw_batch("PROBE", "250", "m") + clear_batch():
        assert call_ok(fkt, p), (hex(fkt), p.hex())
    assert not call_ok(SET_VIS, be16(7) + b"\x01"), "handle 7 is a factory layer and must be refused"
    assert not call_ok(0x205, be16(1)), "IsVisible is not on the send list"
    assert not call_ok(ADD_POLY, poly(BUF_A, 0, ARROW * 15)), "over 99 points must be refused"
    try:
        text(DIST, "2,5")
        raise AssertionError("comma must be refused")
    except ValueError:
        pass
    assert text(DIST, "") == b"\x00\x03\x00\x00\x00"
    assert len(most_body(0x0101, SET_VIS, be16(1) + b"\x01", 1)) == 32 + 3   # msg.hpp parseView: 32-byte header
    xs, ys = [x for x, _ in ARROW], [y for _, y in ARROW]
    assert (min(xs), max(xs), min(ys), max(ys)) == (90, 150, 20, 120)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--street", default="PROBE")
    ap.add_argument("--dist", default="250")
    ap.add_argument("--unit", default="m")
    ap.add_argument("--host", type=lambda s: int(s, 16), default=0x0101,
                    help="82/00 host for the printout only (hex). 0101 = F20 Y24073J capture; the probe on the car "
                         "takes it from the MOST registry and never from this value")
    ap.add_argument("--decode", metavar="CAPTURE", help="classify a captured log instead of printing the batches")
    a = ap.parse_args()
    if not 0x0100 <= a.host <= 0x0FFF:
        ap.error("host must be a ring node 0x0100..0x0FFF")
    selfcheck()
    if a.decode:
        decode(a.decode)
        return
    tag = 1
    for label, batch in (("DRAW", draw_batch(a.street, a.dist, a.unit)), ("CLEAR", clear_batch())):
        print(f"# {label}: {len(batch)} calls, each StartResultAck to {a.host:04X} 82.00, expect ResultAck (op D)")
        for fkt, p in batch:
            assert call_ok(fkt, p)
            print(f"{fkt:03X} {NAMES[fkt]:<19} h{struct.unpack('>H', p[:2])[0]:<2} {p.hex(' ')}")
            print(f"    body {most_body(a.host, fkt, p, tag).hex()}")
            tag += 1
    print("selfcheck OK")


if __name__ == "__main__":
    main()
