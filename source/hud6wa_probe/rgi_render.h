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

typedef struct { unsigned int fkt; unsigned int n; unsigned char p[404]; } Call;   /* 404: a 99-point polygon (6 + 4 x 99) */

/* Cluster profile: which HUDTerminal handles a cluster family uses and where the picture goes. Only the 6WA family is known
 * (NBT-AAD's 6WA layout, also the MINI F54 per st1ng); other clusters (6WB: picture plane 0x1F, own text handles, 126 x 77)
 * get a profile here once a tester confirms it — see docs/clusters.md. */
typedef struct { const char *name; unsigned pic_a, pic_b, frame, street, dist, unit; int x0, y0, x1, y1; } RrProfile;
extern const RrProfile RR_PROFILE_6WA;            /* the box is this car's (176 x 126); rr_profile_detect fits it to any size */
/* ImageGetSize answer for handle 5 (w x h; 0 x 0 or no answer = none) -> profile. 0 = unsupported cluster: draw nothing. */
int rr_profile_detect(unsigned w, unsigned h, RrProfile *out);
/* cluster.conf, one line "pic_a pic_b frame street dist unit x0 y0 x1 y1" (decimal) -> profile; 0 if malformed */
int rr_profile_parse(const char *text, unsigned n, RrProfile *out);
void rr_set_profile(const RrProfile *p);
const RrProfile *rr_profile(void);

/* Where the Mapbox pictures (st1ng's hudshapes, drawn in the 6WA box (70,20)-(170,120)) go on THIS car's 6WA: its picture
 * buffer is 176 x 126 (ImageGetSize, 2026-11-19), so an 88 x 88 box centred in it. One place to tune the size. */
#define RR_BOX_X0 44
#define RR_BOX_Y0 19
#define RR_BOX_X1 132
#define RR_BOX_Y1 107

#define RR_MAXMAN 64
typedef struct { unsigned char valid, glyph, exit_no; char street[48]; } RrMan;
typedef struct {
    RrMan man[RR_MAXMAN];
    int cur, shown, dist_m, fresh, last;
    unsigned int visible;
    char sval[8], sunit[4];
    char sig[384];                  /* state-file path: everything drawn except the distance */
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
void rr_set_context_colour(int colour);   /* fill colour index for the dim (context) tone; default 3 */
/* exposed for tests */
void rr_distance(int m, char *val, char *unit);
void rr_cluster_text(const unsigned char *utf8, unsigned int n, char *out, unsigned int cap);

#endif
