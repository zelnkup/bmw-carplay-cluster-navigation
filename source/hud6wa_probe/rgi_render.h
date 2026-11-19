/* rgi_render — pure on-HU port of cp_guide (iap2_rgi_decode + nav_adapter + glyphs):
 * raw iAP2 0x52xx frames in, 6WA HUDTerminal call batches out. No I/O, no libc, no
 * division helpers, no struct copies (freestanding ARM build). Host-tested against the
 * Python pipeline byte-for-byte (src/cp_guide/test_rgi_render.py).
 *
 * Which maneuver is shown: 0x5202 frames are the route's maneuver LIST (indexed, sent in a
 * burst); the one on screen is the first entry of 0x5201 tlv 0x000D (current maneuver list),
 * index 0 until that is seen. Distance = 0x5201 tlv 0x000A (metres). Both tlv ids follow the
 * Apple RouteGuidanceUpdate field order, which the real capture confirms for 0x0007/0x0008/
 * 0x0011 — 0x000A/0x000D themselves are INFERRED until a moving capture shows them. */
#ifndef RGI_RENDER_H
#define RGI_RENDER_H

typedef struct { unsigned int fkt; unsigned int n; unsigned char p[256]; } Call;

#define RR_MAXMAN 64
typedef struct { unsigned char valid, glyph, exit_no; char street[48]; } RrMan;
typedef struct {
    RrMan man[RR_MAXMAN];
    int cur, shown, dist_m, fresh, last;
    unsigned int visible;
    char sval[8], sunit[4];
} RrState;

void rr_init(RrState *s);
/* store one frame's maneuver (0x5202) / current+distance (0x5201) without drawing; returns msg id or 0 */
unsigned int rr_store(RrState *s, const unsigned char *f, unsigned int len);
/* full draw batch for maneuver idx (nav_adapter.Adapter._draw); returns #calls (0 if idx unknown) */
int rr_draw(RrState *s, int idx, Call *out, int max);
/* store + decide: returns #calls to send now (0 = nothing changed) */
int rr_feed(RrState *s, const unsigned char *f, unsigned int len, Call *out, int max);
/* one cp_listen JSONL line ({"...","hex":"4040..."}) -> frame bytes; returns length or 0 */
unsigned int rr_jsonl_frame(const char *line, unsigned int n, unsigned char *frame, unsigned int cap);
/* librghook's state file (route_state, maneuver_index/type/exit_angle/junction_type/road, dist_to_maneuver) -> calls to send
 * now; *clear = 1 when the route ended (send the clear batch). Loading/Locating (3/4) keeps the last picture. */
int rr_state_text(RrState *s, const char *text, unsigned int n, Call *out, int max, int *clear);
/* exposed for tests */
void rr_distance(int m, char *val, char *unit);
void rr_cluster_text(const unsigned char *utf8, unsigned int n, char *out, unsigned int cap);

#endif
