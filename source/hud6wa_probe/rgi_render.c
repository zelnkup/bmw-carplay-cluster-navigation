/* rgi_render.c — see rgi_render.h. Freestanding: byte-wise, no libc, no __aeabi_* calls. */
#include "rgi_render.h"
#include "glyph_table.h"
#include "rgi_shape.h"

typedef unsigned char u8; typedef unsigned int u32;

enum { SET_VIS = 0x204, SWITCH_VIS = 0x20E, SET_TEXT = 0x210, SET_FILL = 0x21B, ADD_POLY = 0x21D, REMOVE_ALL = 0x220 };
enum { FRAME = 1, STREET = 2, DIST = 3, UNIT = 4, BUF_A = 5, BUF_B = 6, FILL = 4, STREET_CHARS = 32 };

static u32 be16(const u8 *p) { return (u32)((p[0] << 8) | p[1]); }

/* shift-subtract unsigned divide (no __aeabi_uidiv in the freestanding build) */
static u32 udiv(u32 a, u32 b, u32 *rem) {
    u32 q = 0, r = 0;
    for (int i = 31; i >= 0; --i) { r = (r << 1) | ((a >> i) & 1u); if (r >= b) { r -= b; q |= 1u << i; } }
    if (rem) *rem = r;
    return q;
}
static u32 put_dec(char *o, u32 v) {                  /* decimal, returns length */
    char t[10]; u32 n = 0;
    do { u32 r; v = udiv(v, 10, &r); t[n++] = (char)('0' + r); } while (v);
    for (u32 i = 0; i < n; ++i) o[i] = t[n - 1 - i];
    return n;
}
static void scpy(char *d, const char *s, u32 cap) { u32 i = 0; while (s[i] && i + 1 < cap) { d[i] = s[i]; ++i; } d[i] = 0; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }

void rr_init(RrState *s) {
    u8 *p = (u8 *)s;
    for (u32 i = 0; i < sizeof *s; ++i) p[i] = 0;
    s->shown = -1; s->dist_m = -1; s->fresh = 1; s->visible = BUF_B; s->last = -1;
}

/* nav_adapter.distance_texts; half-even where Python's round() ties (exact for integer metres).
 * ponytail: 1..10 km uses half-up on the tenth; Python formats the binary double, so an exact
 * x.x5 km can differ by one tenth. Harmless on a live display; match it if it ever matters. */
void rr_distance(int m, char *val, char *unit) {
    val[0] = 0; unit[0] = 0;
    if (m < 0) return;
    u32 r, q, n = 0;
    if (m < 100) { q = udiv((u32)m + 2, 5, 0) * 5; n = put_dec(val, q); scpy(unit, "m", 4); }
    else if (m < 1000) {
        q = udiv((u32)m, 10, &r); if (r > 5 || (r == 5 && (q & 1))) ++q;
        n = put_dec(val, q * 10); scpy(unit, "m", 4);
    } else if (m < 10000) {
        q = udiv((u32)m + 50, 100, 0);                 /* tenths of km */
        u32 whole = udiv(q, 10, &r);
        n = put_dec(val, whole); val[n++] = '.'; val[n++] = (char)('0' + r); scpy(unit, "km", 4);
    } else {
        q = udiv((u32)m, 1000, &r); if (r > 500 || (r == 500 && (q & 1))) ++q;
        n = put_dec(val, q); scpy(unit, "km", 4);
    }
    val[n] = 0;
}

/* nav_adapter.cluster_text minus the length cut: fold to ASCII, keep [A-Za-z0-9 -./], others -> space,
 * collapse spaces. Unmapped codepoints >= U+0180 are dropped (Python's NFKD+ascii-ignore drops them too,
 * except rare compatibility forms like ligatures/fullwidth — ponytail: extend FOLD if one shows up). */
static int keep(u8 c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '/'; }
void rr_cluster_text(const u8 *s, u32 n, char *out, u32 cap) {
    u32 o = 0, i = 0; int sp = 1;                      /* sp: last emitted was a space (or start) */
    while (i < n && s[i]) {
        u32 cp, k; u8 b = s[i];
        if (b < 0x80) { cp = b; k = 1; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; k = 2; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; k = 3; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; k = 4; }
        else { ++i; continue; }
        if (i + k > n) break;
        int bad = 0;
        for (u32 j = 1; j < k; ++j) { if ((s[i + j] & 0xC0) != 0x80) { bad = 1; break; } cp = (cp << 6) | (s[i + j] & 0x3F); }
        if (bad) { ++i; continue; }
        i += k;
        const char *f; char one[2];
        if (cp < 0x80) { one[0] = (char)cp; one[1] = 0; f = one; }
        else if (cp >= 0xA0 && cp < 0x180) f = FOLD[cp - 0xA0];
        else continue;
        for (; *f; ++f) {
            u8 c = (u8)*f;
            if (keep(c)) { if (o + 1 < cap) out[o++] = (char)c; sp = 0; }
            else if (!sp) { if (o + 1 < cap) out[o++] = ' '; sp = 1; }
        }
    }
    while (o && out[o - 1] == ' ') --o;
    out[o] = 0;
}

/* nav_adapter.maneuver_angle + glyphs.glyph_for -> glyph_table slot; *exit_no set for roundabouts */
static int cp_angle(int t, int *arrive) {
    *arrive = 0;
    switch (t) {
        case 1: case 20: return -90;   case 2: case 21: return 90;
        case 13: case 49: case 52: case 22: return -45;
        case 14: case 50: case 53: case 23: case 8: case 9: return 45;
        case 47: return -135;          case 48: return 135;
        case 4: case 18: case 26: return -180;
        case 10: case 12: case 24: case 25: case 27: *arrive = 1; return 0;
    }
    return 0;                                           /* incl. 0,3,5,11,15,16,17,51 and unknown: straight */
}
static u8 glyph_slot(int has_type, int t, int has_round, int is_round, int has_exit, int exit_angle, u8 *exit_no) {
    *exit_no = 0;
    if (!has_type) return 1;                            /* no type: straight, never guess a turn */
    if (!has_round) is_round = (t >= 28 && t <= 46) || t == 6 || t == 7 || t == 19;
    if (is_round) {
        int a = has_exit ? exit_angle : 0;
        if (a < -180) a = -180;
        if (a > 180) a = 180;
        a = (int)(udiv((u32)(a + 180 + 2), 5, 0) * 5) - 180;    /* 5-degree steps (nav_adapter does the same) */
        if (t == 19) a = -180;
        if (t >= 28 && t <= 46) *exit_no = (u8)(t - 27);
        return (u8)(GLYPH_ROUND0 + udiv((u32)(a + 180), 5, 0));
    }
    if (t == 7) return GLYPH_EXIT_ROUND;                /* ExitRoundabout (not flagged as a roundabout) */
    int arrive, ang = cp_angle(t, &arrive);
    if (arrive) return GLYPH_ARRIVE;
    static const short A[9] = { 0, -45, 45, -90, 90, -135, 135, -180, 180 };
    for (u8 i = 0; i < 9; ++i) if (A[i] == ang) return (u8)(1 + i);
    return 1;
}

/* walk TLVs [be16 len][be16 id][value] from off; calls back per tlv */
typedef struct { const u8 *v; u32 n; int has; } Tlv;
static void tlvs(const u8 *f, u32 len, Tlv *want, u32 nwant) {
    for (u32 i = 0; i < nwant; ++i) want[i].has = 0;
    u32 off = 6;
    while (off + 4 <= len) {
        u32 tl = be16(f + off), id = be16(f + off + 2);
        if (tl < 4 || off + tl > len) break;
        if (id < nwant) { want[id].v = f + off + 4; want[id].n = tl - 4; want[id].has = 1; }
        off += tl;
    }
}

unsigned int rr_store(RrState *s, const u8 *f, u32 len) {
    if (len < 6 || f[0] != 0x40 || f[1] != 0x40) return 0;
    u32 id = be16(f + 4);
    Tlv t[0x0E];
    if (id == 0x5202) {
        tlvs(f, len, t, 0x0C);
        if (!t[1].has || t[1].n < 2) return 0;
        u32 idx = be16(t[1].v);
        if (idx >= RR_MAXMAN) return 0;
        RrMan *m = &s->man[idx];
        m->glyph = glyph_slot(t[3].has && t[3].n >= 1, t[3].has && t[3].n ? t[3].v[0] : 0,
                              t[9].has && t[9].n >= 1, t[9].has && t[9].n && t[9].v[0],
                              t[0x0B].has && t[0x0B].n >= 2, t[0x0B].has && t[0x0B].n >= 2 ? (short)be16(t[0x0B].v) : 0,
                              &m->exit_no);
        if (t[4].has) rr_cluster_text(t[4].v, t[4].n, m->street, sizeof m->street);
        else m->street[0] = 0;
        m->valid = 1; s->last = (int)idx;
        return id;
    }
    if (id == 0x5201) {
        tlvs(f, len, t, 0x0E);
        if (t[0x0D].has && t[0x0D].n >= 2) s->cur = (int)be16(t[0x0D].v);        /* INFERRED: current maneuver list */
        if (t[0x0A].has && t[0x0A].n >= 1 && t[0x0A].n <= 8) {                   /* INFERRED: metres to maneuver */
            u32 v = 0, big = 0;
            for (u32 i = 0; i < t[0x0A].n; ++i) { if (v >> 24) big = 1; v = (v << 8) | t[0x0A].v[i]; }
            s->dist_m = (big || v > 0x7fffffffu) ? 0x7fffffff : (int)v;
        }
        return id;
    }
    return 0;
}

/* ---- HUDTerminal call builders (hud6wa_frames.py) ---- */
static Call *c_h(Call *c, u32 fkt, u32 h) { c->fkt = fkt; c->p[0] = (u8)(h >> 8); c->p[1] = (u8)h; c->n = 2; return c; }
static void c_b(Call *c, u32 b) { c->p[c->n++] = (u8)b; }
static void c_text(Call *c, u32 h, const char *s) {
    c_h(c, SET_TEXT, h); c_b(c, 0);
    while (*s) { c_b(c, 0); c_b(c, (u8)*s++); }
    c_b(c, 0); c_b(c, 0);
}

int rr_draw(RrState *s, int idx, Call *out, int max) {
    if (idx < 0 || idx >= RR_MAXMAN || !s->man[idx].valid || max < 18) return 0;   /* <= 6 polygons + 12 calls */
    RrMan *m = &s->man[idx];
    char street[64]; u32 k = 0;
    if (m->exit_no) {
        const char *e = "Exit "; while (*e) street[k++] = *e++;
        k += put_dec(street + k, m->exit_no);
        if (m->street[0]) street[k++] = ' ';
    }
    for (const char *p = m->street; *p && k < sizeof street - 1; ++p) street[k++] = *p;
    if (k > STREET_CHARS) k = STREET_CHARS;
    while (k && street[k - 1] == ' ') --k;
    street[k] = 0;
    char val[8], unit[4]; rr_distance(s->dist_m, val, unit);

    u32 vis = s->visible, hid = vis == BUF_B ? BUF_A : BUF_B;
    int n = 0;
    c_h(&out[n++], REMOVE_ALL, hid);
    c_b(c_h(&out[n++], SET_FILL, hid), FILL);
    u32 off = G_IDX[m->glyph].off;
    for (u32 i = 0; i < G_IDX[m->glyph].npoly; ++i) {
        Call *c = c_h(&out[n++], ADD_POLY, hid);
        u32 np = G_IDX[m->glyph].len[i];
        c_b(c, i >> 8); c_b(c, i); c_b(c, 3); c_b(c, np);
        for (u32 j = 0; j < 2 * np; ++j) { u32 v = G_PTS[2 * off + j]; c_b(c, v >> 8); c_b(c, v); }
        off += np;
    }
    Call *sw = c_h(&out[n++], SWITCH_VIS, vis); c_b(sw, hid >> 8); c_b(sw, hid);
    c_h(&out[n++], REMOVE_ALL, vis);
    if (s->fresh) for (u32 h = STREET; h <= UNIT; ++h) c_b(c_h(&out[n++], SET_VIS, h), 1);
    c_text(&out[n++], STREET, street); c_text(&out[n++], DIST, val); c_text(&out[n++], UNIT, unit);
    c_b(c_h(&out[n++], SET_VIS, FRAME), 1);
    s->visible = hid; s->fresh = 0; s->shown = idx;
    scpy(s->sval, val, sizeof s->sval); scpy(s->sunit, unit, sizeof s->sunit);
    return n;
}

int rr_feed(RrState *s, const u8 *f, u32 len, Call *out, int max) {
    u32 id = rr_store(s, f, len);
    if (id == 0x5202) return s->last == s->cur ? rr_draw(s, s->cur, out, max) : 0;
    if (id != 0x5201) return 0;
    if (s->shown != s->cur) return rr_draw(s, s->cur, out, max);   /* new current maneuver */
    char val[8], unit[4]; rr_distance(s->dist_m, val, unit);        /* same maneuver: distance text only */
    int n = 0;
    if (!seq(val, s->sval) && n < max) { c_text(&out[n++], DIST, val); scpy(s->sval, val, sizeof s->sval); }
    if (!seq(unit, s->sunit) && n < max) { c_text(&out[n++], UNIT, unit); scpy(s->sunit, unit, sizeof s->sunit); }
    return n;
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
unsigned int rr_jsonl_frame(const char *l, u32 n, u8 *frame, u32 cap) {
    static const char key[] = "\"hex\":\"";
    for (u32 i = 0; i + 7 <= n; ++i) {
        u32 j = 0; while (j < 7 && l[i + j] == key[j]) ++j;
        if (j < 7) continue;
        u32 k = 0, p = i + 7;
        while (p + 1 < n && k < cap) {
            int hi = hexv(l[p]), lo = hexv(l[p + 1]);
            if (hi < 0 || lo < 0) break;
            frame[k++] = (u8)((hi << 4) | lo); p += 2;
        }
        return k;
    }
    return 0;
}

/* ---- librghook state file (third_party/.../rghook.c publish(): "key:type:value" lines) ---- */
static int field(const char *t, u32 n, const char *key, const char **v, u32 *vn) {
    for (u32 i = 0; i < n;) {
        u32 e = i; while (e < n && t[e] != '\n') ++e;
        u32 k = 0; while (key[k] && i + k < e && t[i + k] == key[k]) ++k;
        if (!key[k] && i + k < e && t[i + k] == ':') {
            u32 j = i + k + 1; while (j < e && t[j] != ':') ++j;        /* skip the type ("n" or "") */
            if (j < e) { *v = t + j + 1; *vn = e - j - 1; return 1; }
        }
        i = e + 1;
    }
    return 0;
}
static int num(const char *t, u32 n, const char *key, int *out) {
    const char *v; u32 vn; if (!field(t, n, key, &v, &vn) || !vn) return 0;
    int neg = v[0] == '-', x = 0; u32 i = neg;
    if (i >= vn) return 0;
    for (; i < vn && v[i] >= '0' && v[i] <= '9'; ++i) x = x * 10 + (v[i] - '0');
    *out = neg ? -x : x; return 1;
}
/* a signed decimal at *p (advances it); 0 if none */
static int snum(const char *v, u32 vn, u32 *p, int *out) {
    u32 i = *p; int neg = 0, x = 0, got = 0;
    if (i < vn && v[i] == '-') { neg = 1; ++i; }
    for (; i < vn && v[i] >= '0' && v[i] <= '9'; ++i) { x = x * 10 + (v[i] - '0'); got = 1; }
    *p = i; *out = neg ? -x : x; return got;
}
/* "status/hl/a,b;status/hl/a" (rghook.c publish) -> lanes, left to right */
static u32 parse_lanes(const char *v, u32 vn, RrLane *L, u32 max) {
    u32 n = 0, i = 0;
    while (i < vn && n < max) {
        RrLane *l = &L[n]; int x; l->na = 0; l->hl = 1000; l->status = 0;
        if (!snum(v, vn, &i, &x)) break;
        l->status = (unsigned)x;
        if (i < vn && v[i] == '/') { ++i; if (snum(v, vn, &i, &x)) l->hl = x; }
        if (i < vn && v[i] == '/') {
            ++i;
            while (l->na < 4 && snum(v, vn, &i, &l->ang[l->na])) { ++l->na; if (i < vn && v[i] == ',') ++i; else break; }
        }
        ++n;
        while (i < vn && v[i] != ';') ++i;
        if (i < vn) ++i;
    }
    return n;
}
static RrPoly g_poly[48];
const RrProfile RR_PROFILE_6WA = { "6WA", BUF_A, BUF_B, FRAME, STREET, DIST, UNIT, RR_BOX_X0, RR_BOX_Y0, RR_BOX_X1, RR_BOX_Y1 };
static RrProfile g_prof = { "6WA", BUF_A, BUF_B, FRAME, STREET, DIST, UNIT, RR_BOX_X0, RR_BOX_Y0, RR_BOX_X1, RR_BOX_Y1 };
void rr_set_profile(const RrProfile *p) { g_prof = *p; }
const RrProfile *rr_profile(void) { return &g_prof; }
int rr_profile_detect(unsigned w, unsigned h, RrProfile *out) {
    if (w < 64 || h < 48 || w > 1024 || h > 1024) return 0;        /* no picture buffer 5 (or nonsense): not a 6WA-family cluster */
    *out = RR_PROFILE_6WA;
    int side = (int)((w < h ? w : h) * 7u / 10u);                   /* 70 % of the short side, centred: 176 x 126 -> 88 at (44,19) */
    out->x0 = ((int)w - side) >> 1; out->y0 = ((int)h - side) >> 1; out->x1 = out->x0 + side; out->y1 = out->y0 + side;
    return 1;
}
int rr_profile_parse(const char *t, unsigned n, RrProfile *out) {
    int v[10]; u32 i = 0, k = 0;
    while (k < 10) {
        while (i < n && (t[i] == ' ' || t[i] == '\t')) ++i;
        if (!snum(t, n, &i, &v[k])) return 0;
        ++k;
    }
    for (k = 0; k < 6; ++k) if (v[k] < 1 || v[k] > 0xFF) return 0;
    if (v[6] < 0 || v[7] < 0 || v[8] <= v[6] || v[9] <= v[7] || v[8] > 1024 || v[9] > 1024) return 0;
    out->name = "cluster.conf";
    out->pic_a = (unsigned)v[0]; out->pic_b = (unsigned)v[1]; out->frame = (unsigned)v[2];
    out->street = (unsigned)v[3]; out->dist = (unsigned)v[4]; out->unit = (unsigned)v[5];
    out->x0 = v[6]; out->y0 = v[7]; out->x1 = v[8]; out->y1 = v[9];
    return 1;
}
static int g_ctx = 2;                       /* context-tone colour index: 2 picked by the owner on the 6WA (3 = too dim grey, 5 = red) */
void rr_set_context_colour(int c) { g_ctx = c; }

/* Mapbox pictures (st1ng's hudshapes via rgi_shape) from librghook's state file. Redraws when the picture or the street
 * changes, else only the distance text. 3/4 Loading/Locating keep the last picture; any other non-guiding state clears. */
int rr_state_text(RrState *s, const char *t, u32 n, Call *out, int max, int *clear) {
    *clear = 0;
    int rs = 0, idx, type = 0, ang = 0, side = 0, jt = 0, dist;
    num(t, n, "route_state", &rs);
    if (!(rs == 1 || rs == 5 || rs == 6)) {
        if (rs != 3 && rs != 4 && s->shown >= 0) { *clear = 1; s->shown = -1; s->fresh = 1; s->visible = g_prof.pic_b; s->sig[0] = 0; }
        return 0;
    }
    if (!num(t, n, "maneuver_index", &idx) || idx < 0) return 0;
    num(t, n, "maneuver_type", &type); num(t, n, "maneuver_exit_angle", &ang);
    num(t, n, "maneuver_driving_side", &side); num(t, n, "maneuver_junction_type", &jt);
    int jang[8]; u32 nj = 0; const char *v; u32 vn;
    if (field(t, n, "maneuver_junction_angles", &v, &vn))
        for (u32 i = 0; nj < 8 && snum(v, vn, &i, &jang[nj]);) { ++nj; if (i < vn && v[i] == ',') ++i; else break; }
    RrLane lanes[8]; u32 nl = 0;
    if (field(t, n, "lanes", &v, &vn) && vn) nl = parse_lanes(v, vn, lanes, 8);
    char street[48]; street[0] = 0;
    if (field(t, n, "maneuver_road", &v, &vn) && vn) rr_cluster_text((const u8 *)v, vn, street, sizeof street);
    else if (field(t, n, "current_road", &v, &vn)) rr_cluster_text((const u8 *)v, vn, street, sizeof street);
    { u32 k = 0; while (street[k]) ++k; if (k > 32) k = 32; while (k && street[k - 1] == ' ') --k; street[k] = 0; } /* 6WA: <= 32 */
    /* parked / before the first update the iPhone sends 0 with no text: show no distance rather than "0 m" */
    const char *dsv; u32 dsn;
    int has_ds = field(t, n, "dist_to_maneuver_string", &dsv, &dsn) && dsn;
    s->dist_m = num(t, n, "dist_to_maneuver", &dist) && (dist > 0 || has_ds) ? dist : -1;
    /* signature of everything drawn except the distance */
    char sig[sizeof s->sig]; u32 k = 0;
    const char *keys[] = { "maneuver_type", "maneuver_exit_angle", "maneuver_driving_side", "maneuver_junction_type",
                           "maneuver_junction_angles", "lanes" };
    for (u32 q = 0; q < 6; ++q) {
        if (field(t, n, keys[q], &v, &vn)) for (u32 i = 0; i < vn && k + 2 < sizeof sig; ++i) sig[k++] = v[i];
        if (k + 2 < sizeof sig) sig[k++] = '|';
    }
    for (const char *p = street; *p && k + 1 < sizeof sig; ++p) sig[k++] = *p;
    sig[k] = 0;
    if (s->shown == idx && seq(sig, s->sig)) {                           /* same picture: distance text only */
        char val[8], unit[4]; rr_distance(s->dist_m, val, unit);
        int c = 0;
        if (!seq(val, s->sval) && c < max) { c_text(&out[c++], g_prof.dist, val); scpy(s->sval, val, sizeof s->sval); }
        if (!seq(unit, s->sunit) && c < max) { c_text(&out[c++], g_prof.unit, unit); scpy(s->sunit, unit, sizeof s->sunit); }
        return c;
    }
    int np = rr_shape((unsigned)type, ang, (unsigned)side, (unsigned)jt, jang, nj, lanes, nl,
                      g_prof.x0, g_prof.y0, g_prof.x1, g_prof.y1, g_poly, 48);
    if (np <= 0 || max < 2 * np + 12) return 0;
    char val[8], unit[4]; rr_distance(s->dist_m, val, unit);
    u32 vis = s->visible == g_prof.pic_a ? g_prof.pic_a : g_prof.pic_b, hid = vis == g_prof.pic_b ? g_prof.pic_a : g_prof.pic_b;
    int c = 0, cur = -1;
    c_h(&out[c++], REMOVE_ALL, hid);
    for (int i = 0; i < np; ++i) {
        int col = g_poly[i].tone ? g_ctx : FILL;
        if (col != cur) { c_b(c_h(&out[c++], SET_FILL, hid), (u32)col); cur = col; }
        Call *pc = c_h(&out[c++], ADD_POLY, hid);
        c_b(pc, (u32)i >> 8); c_b(pc, (u32)i); c_b(pc, 3); c_b(pc, g_poly[i].n);
        for (u32 j = 0; j < g_poly[i].n; ++j)
            for (u32 xy = 0; xy < 2; ++xy) { u32 w = (u32)(unsigned short)g_poly[i].p[j][xy]; c_b(pc, w >> 8); c_b(pc, w); }
    }
    Call *sw = c_h(&out[c++], SWITCH_VIS, vis); c_b(sw, hid >> 8); c_b(sw, hid);
    c_h(&out[c++], REMOVE_ALL, vis);
    if (s->fresh) { c_b(c_h(&out[c++], SET_VIS, g_prof.street), 1); c_b(c_h(&out[c++], SET_VIS, g_prof.dist), 1);
                    c_b(c_h(&out[c++], SET_VIS, g_prof.unit), 1); }
    c_text(&out[c++], g_prof.street, street); c_text(&out[c++], g_prof.dist, val); c_text(&out[c++], g_prof.unit, unit);
    c_b(c_h(&out[c++], SET_VIS, g_prof.frame), 1);
    s->visible = hid; s->fresh = 0; s->shown = idx;
    scpy(s->sig, sig, sizeof s->sig); scpy(s->sval, val, sizeof s->sval); scpy(s->sunit, unit, sizeof s->sunit);
    return c;
}
