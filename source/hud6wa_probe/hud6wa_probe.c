/* hud6wa_probe.c -- standalone 6WA HUDTerminal probe for NBT EVO Y24073J, plain C on the M1 toolchain
 * (clang + arm-none-eabi-ld + import stub of the car's libc.so.3; _start in m1_1_start.S calls _exit, no teardown).
 *
 *   hud6wa_probe watch  [secs]  receive only: DSI attach + MOST client registration; no devctrl, no MOST message
 *   hud6wa_probe draw   [secs]  one arrow + "PROBE" "250" "m" on FB 82/00 handles 1..6 for secs (1..20), then clear
 *   hud6wa_probe play   [secs]  draw ONE batch of HUDTerminal calls read from stdin (to_calls.py), hold, clear
 *   hud6wa_probe stream [secs]  draw each blank-line-delimited batch from stdin as it arrives (live/replay), then clear
 *   hud6wa_probe rg     [secs] [file]  ON-HU guidance from librghook's state file (default /dev/shmem/routeguidance,
 *                               polled every 250 ms): draw the current maneuver, distance updates, clear at route end
 *   hud6wa_probe rgi    [secs]  ON-HU live guidance: stdin = /tmp/cp_rgi.capture (followed like tail -f), decode
 *                               0x5201/0x5202 here (rgi_render.c, no laptop), draw the current maneuver, clear at end
 *
 * DSI binding and MOST framing restated from Sq00ky/Supra-iDrive6-Android-Auto @ 9af2284 (src/dsi_client.cpp,
 * src/most/{client.cpp,msg.hpp,ids.hpp}, src/cluster/hudterm.hpp; PolyForm Noncommercial, personal use). Offsets
 * verified against this car's libsys_dsi_colibry.cpp-ne.so.8 (reports/cp6wa-oncar-20261112.md).
 * Calls are sent one at a time; each must be answered (ResultAck/Status) within 1500 ms or the run stops.
 * Factory navigation on 82/00 (any FB 82 traffic with the host that is neither ours nor the cluster's copy of an
 * answer to us) stops a draw before anything more is sent. Every exit is _exit(): never run C++ static destructors.
 */
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long usize;

extern void *dlopen(const char *, int);
extern long  read(int, void *, usize);
extern int   open(const char *, int, ...);
extern int   close(int);
extern void *dlsym(void *, const char *);
extern char *dlerror(void);
extern void *calloc(usize, usize);
extern long  write(int, const void *, usize);
extern int   pthread_create(int *, const void *, void *(*)(void *), void *);
extern int   pthread_mutex_init(void *, const void *);
extern int   pthread_mutex_lock(void *);
extern int   pthread_mutex_unlock(void *);
extern int   clock_gettime(int, void *);
extern int   nanosleep(const void *, void *);
extern void *signal(int, void (*)(int));
extern u32   alarm(u32);
extern void  _exit(int);

#define RTLD_NOW 2
#define CLOCK_MONOTONIC 2
#define LIB "/opt/sys/lib/libsys_dsi_colibry.cpp-ne.so.8"

/* freestanding ARM EABI aggregate-copy helper: clang emits __aeabi_memcpy8 for the
 * 256-byte Call struct copies. Byte-wise so it is safe at any alignment
 * (-mno-unaligned-access; an unaligned access reboots the HU). Defined, not
 * imported, so the build's undefined-symbol allowlist is unchanged. */
void __aeabi_memcpy8(void *d, const void *s, usize n);
void __aeabi_memcpy8(void *d, const void *s, usize n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; }

/* ---------- tiny output (no stdio) ---------- */
typedef struct { char b[2048]; u32 n; } Line;
static void ls(Line *l, const char *s) { while (*s && l->n < sizeof(l->b) - 1) l->b[l->n++] = *s++; }
static void lh(Line *l, u32 v, int digits) {
    for (int i = digits - 1; i >= 0; --i) { u32 d = (v >> (4 * i)) & 15; if (l->n < sizeof(l->b) - 1) l->b[l->n++] = "0123456789ABCDEF"[d]; }
}
static void ld(Line *l, u32 v) {                         /* decimal without division (no __aeabi_uidiv) */
    static const u32 p[] = { 1000000000u, 100000000u, 10000000u, 1000000u, 100000u, 10000u, 1000u, 100u, 10u, 1u };
    int started = 0;
    for (int i = 0; i < 10; ++i) {
        u32 d = 0; while (v >= p[i]) { v -= p[i]; ++d; }
        if (d || started || i == 9) { started = 1; if (l->n < sizeof(l->b) - 1) l->b[l->n++] = (char)('0' + d); }
    }
}
static void lend(Line *l) { l->b[l->n++] = '\n'; write(1, l->b, l->n); }
static void say(const char *s) { Line l; l.n = 0; ls(&l, s); lend(&l); }
static void quit(int rc) { Line l; l.n = 0; ls(&l, "[probe] exit "); ld(&l, (u32)rc); lend(&l); _exit(rc); }

static u32 nowMs(void) { long ts[2]; clock_gettime(CLOCK_MONOTONIC, ts); return (u32)ts[0] * 1000u + ((u32)ts[1] >> 20); }
static void sleepMs(u32 ms) { long ts[2]; ts[0] = 0; ts[1] = 0; while (ms >= 1000) { ms -= 1000; ts[0]++; } ts[1] = (long)ms * 1000000L; nanosleep(ts, 0); }
static u32 rd16(const u8 *p) { return (u32)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u32 be16(const u8 *p) { return (u32)((p[0] << 8) | p[1]); }

/* ---------- DSI binding (dsi_client.cpp) ---------- */
typedef void (*FnConnCtor)(void *);
typedef int  (*FnConnAdd)(void *, void *);
typedef void (*FnConnStart)(void *);
typedef void (*FnCliCtor)(void *, const char *, const char *, int, int);
typedef void (*FnSetNotif)(void *, u32);
typedef void (*FnStreamCtor)(void *, void *, void *, int, u32, u32, u32, const void *, const void *, int);
typedef void (*FnStreamWrite)(void *, const void *, u32);
typedef int  (*FnStreamSend)(void *, int);
typedef void (*FnStreamDtor)(void *);
static FnConnCtor connCtor; static FnConnAdd connAdd; static FnConnStart connStart; static FnCliCtor cliCtor;
static FnSetNotif setNotif; static FnStreamCtor streamCtor; static FnStreamWrite streamWrite;
static FnStreamSend streamSend; static FnStreamDtor streamDtor; static void *vtable;
static char *g_cli; static void *g_conn;
static u32 g_lock[16];                                   /* pthread_mutex_t, oversized; serialises every DSI send */

static int sendRequest(u32 id, const u8 *p, u32 n) {
    unsigned long long st[(0xBC + 64) / 8 + 1];          /* 8-aligned CDSIOStream */
    for (u32 i = 0; i < sizeof(st) / 8; ++i) st[i] = 0;
    pthread_mutex_lock(g_lock);
    streamCtor(st, g_cli + 0x130, g_cli + 0x138, *(int *)(g_cli + 0x140), 0x100u, 7u, id, g_cli + 0x110, g_cli + 0x118, 0);
    if (n) streamWrite(st, p, n);
    int rc = streamSend(st, -1);
    streamDtor(st);
    pthread_mutex_unlock(g_lock);
    if (rc != 0) { Line l; l.n = 0; ls(&l, "[dsi] send FAILED id="); lh(&l, id, 8); lend(&l); }
    return rc == 0;
}
static void notify(u32 attr) { pthread_mutex_lock(g_lock); setNotif(g_cli, attr); pthread_mutex_unlock(g_lock); }

/* ---------- shared state (callback thread writes, main reads) ---------- */
static volatile int g_stop, g_draw, g_play, g_stream, g_rgi, g_rg, g_size;
static volatile u32 g_client = 0xffffffffu, g_self, g_answered, g_comm = 0xffffffffu;
static volatile u32 g_host, g_hosts8200 = 0xffffffffu, g_nav, g_navMsgs, g_copies, g_fb82;
static volatile u32 g_wantFkt, g_wantHandle, g_result;   /* g_result: 0 waiting, 1 ok, 2 error */
static u32 g_recentFkt[32], g_recentH[32], g_recentMs[32], g_recentN;

static const char *opName(u32 op) {
    static const char *n[16] = { "Set/Start", "Get/Abort", "SetGet/StartResult", "Increment", "Decrement", "GetInterface",
        "StartResultAck", "AbortAck", "StartAck", "ErrorAck", "ProcessingAck", "Processing", "Status/Result", "ResultAck",
        "Interface", "Error" };
    return n[op & 15];
}

static int isRecent(u32 fkt, u32 h, u32 now) {
    for (u32 i = 0; i < 32; ++i)
        if (g_recentMs[i] && g_recentFkt[i] == fkt && g_recentH[i] == h && now - g_recentMs[i] <= 1500) return 1;
    return 0;
}

/* One FB 0x82 message: log it (hud6wa_frames.py --decode format), then answer matching / factory-nav detection. */
static void onFb82(const u8 *p, u32 len) {
    u32 dst = rd16(p + 12), src = rd16(p + 14), inst = p[17], fkt = rd16(p + 18), op = p[20] & 15, n = rd32(p + 28);
    u32 varTag = rd32(p + 4), varVal = rd32(p + 8), tx = rd32(p);
    if (n > len - 32) return;
    const u8 *d = p + 32;
    int seg = varTag == 2 && tx == 1 && varVal >= 19 && varVal <= 21;
    Line l; l.n = 0;
    ls(&l, seg ? "SEG " : "MOST "); ls(&l, "dst="); lh(&l, dst, 4); ls(&l, " src="); lh(&l, src, 4);
    ls(&l, " FB=82 inst="); lh(&l, inst, 2); ls(&l, " fkt="); lh(&l, fkt, 3); ls(&l, " op="); ls(&l, opName(op));
    ls(&l, "("); lh(&l, op, 1); ls(&l, ") len="); ld(&l, n); ls(&l, ":");
    for (u32 i = 0; i < n && i < 600; ++i) { ls(&l, " "); l.b[l.n] = 0; if (l.n < sizeof(l.b) - 3) { l.b[l.n++] = "0123456789abcdef"[d[i] >> 4]; l.b[l.n++] = "0123456789abcdef"[d[i] & 15]; } }
    lend(&l);
    ++g_fb82;
    if (inst != 0 || !g_host || seg) return;
    u32 now = nowMs(), h = n >= 2 ? be16(d) : 0xffffffffu;
    if (g_self && src == g_self) return;
    if (g_self && dst == g_self) {                       /* an answer to us */
        if (src != g_host || op == 0xA || op == 0xB) return;
        int byHandle = op != 0xF && n >= 2;
        if (fkt == g_wantFkt && (!byHandle || h == g_wantHandle) && g_result == 0)
            g_result = (op == 0xD || op == 0xC) ? 1 : 2;
        else if (op != 0xD && op != 0xC) g_result = 2;   /* an error that matches nothing still stops us */
        return;
    }
    if (src != g_host && dst != g_host) return;
    if (src == g_host && isRecent(fkt, h, now)) { ++g_copies; return; }
    ++g_navMsgs;
    if (!g_nav) { g_nav = 1; say("[probe] FACTORY NAVIGATION seen on 82/00"); }
}

static void onRegistry(const u8 *p, u32 len) {
    if (len < 4) return;
    u32 c = rd32(p), hosts = 0, host = 0;
    if (c == 0 || c > 1024 || 4 + c * 4 > len) return;
    for (u32 i = 0; i < c; ++i) {
        u32 a = rd16(p + 4 + i * 4), k = rd16(p + 6 + i * 4);
        if ((k >> 8) != 0x82) continue;
        Line l; l.n = 0; ls(&l, "[probe] registry: "); lh(&l, a, 4); ls(&l, " hosts FB 82/"); lh(&l, k & 0xff, 2); lend(&l);
        if (k == 0x8200) { ++hosts; if (!host && a >= 0x0100 && a <= 0x0FFF) host = a; }
    }
    g_host = host;
    g_hosts8200 = hosts;
}

/* vtable slot 3 / 4 / 7 replacements, called with this = our CDSIClient */
static void onAttached(void *self) {
    (void)self;
    say("[dsi] attached to MOSTSubSystem.DSYSMOSTServer 4.5: registering client hud6wa-probe");
    static const char name[] = "hud6wa-probe";
    u8 p[4 + 16]; u32 n = sizeof(name);                  /* putString: u32 length incl. NUL, bytes, pad to 4 */
    p[0] = (u8)n; p[1] = p[2] = p[3] = 0;
    for (u32 i = 0; i < n; ++i) p[4 + i] = (u8)name[i];
    u32 tot = 4 + n; while (tot & 3) p[tot++] = 0;
    sendRequest(0xF, p, tot);                            /* registerMOSTClientExt */
    static const u32 subs[] = { 0xC0000002u, 0xC0000003u, 0xC0000001u, 0x80000001u, 0x80000002u, 0x80000003u,
                                0x80000004u, 0x80000005u, 0x80000006u, 0x80000007u };
    for (u32 i = 0; i < 10; ++i) notify(subs[i]);
}
static void onDetached(void *self) { (void)self; say("[dsi] DETACHED from the server"); g_answered = 0; g_self = 0; }
static void onData(void *self, void *is) {
    (void)self;
    if (!is) return;
    const char *s = (const char *)is;
    u32 id = *(const u32 *)(s + 0x48), len = *(const u32 *)(s + 0x08);
    const u8 *p = *(const u8 *const *)(s + 0x10);
    if (!p) return;
    if (id == 0x80000001u) { if (len >= 32 && p[16] == 0x82) onFb82(p, len); return; }
    if (id == 0xC0000001u) { onRegistry(p, len); return; }
    if (id == 0x80000000u && len >= 6) {
        g_client = rd32(p); g_self = rd16(p + 4); g_answered = 1;
        Line l; l.n = 0; ls(&l, "[most] registered: clientId="); ld(&l, g_client); ls(&l, " address="); lh(&l, g_self, 4);
        if (g_client == 0xffffffffu) ls(&l, " (REFUSED)");
        lend(&l);
        if (g_client != 0xffffffffu) notify(0xC0000000u);
        return;
    }
    if (id == 0xC0000000u && len >= 4) {
        u32 v = rd32(p);
        if (v != g_comm) { g_comm = v; Line l; l.n = 0; ls(&l, "[most] comm state "); ld(&l, v); lend(&l); }
    }
}
static void *connThread(void *a) { (void)a; connStart(g_conn); say("[dsi] connection loop returned"); return 0; }

/* ---------- HUDTerminal calls (hudterm.hpp payloads; profile 6WA handles 1..6) ---------- */
#include "rgi_render.h"                              /* Call {fkt, n, p[256]}: <=58-pt polygon = 238B, 32-char street = 67B */
static u32 putH(u8 *p, u32 h) { p[0] = (u8)(h >> 8); p[1] = (u8)h; return 2; }
static Call c2(u32 fkt, u32 h) { Call c; c.fkt = fkt; c.n = putH(c.p, h); return c; }
static Call c3(u32 fkt, u32 h, u32 b) { Call c = c2(fkt, h); c.p[c.n++] = (u8)b; return c; }
static Call cSwitch(u32 hide, u32 show) { Call c = c2(0x20E, hide); c.n += putH(c.p + 2, show); return c; }
static Call cText(u32 h, const char *s) {                /* handle, 00, UTF-16BE, 00 00 */
    Call c = c3(0x210, h, 0);
    while (*s) { c.p[c.n++] = 0; c.p[c.n++] = (u8)*s++; }
    c.p[c.n++] = 0; c.p[c.n++] = 0; return c;
}
static Call cArrow(void) {
    static const short pts[14] = { 110, 120, 130, 120, 130, 60, 150, 60, 120, 20, 90, 60, 110, 60 };
    Call c = c2(0x21D, 5); c.p[c.n++] = 0; c.p[c.n++] = 0; c.p[c.n++] = 3; c.p[c.n++] = 7;
    for (u32 i = 0; i < 14; ++i) c.n += putH(c.p + c.n, (u32)(u16)pts[i]);
    return c;
}
static int handleOk(u32 h) { return h >= 1 && h <= 6; }
static int callOk(const Call *c) {                       /* payloadShapeOk + handle allowlist, re-checked before send */
    if (c->n < 2 || !handleOk(be16(c->p))) return 0;
    switch (c->fkt) {
        case 0x204: return c->n == 3 && c->p[2] <= 1;
        case 0x220: return c->n == 2;
        case 0x20E: return c->n == 4 && handleOk(be16(c->p + 2));
        case 0x21B: return c->n == 3;
        case 0x210: return c->n >= 5 && ((c->n - 3) & 1) == 0 && c->n <= 69 && c->p[2] == 0 && !c->p[c->n - 1] && !c->p[c->n - 2];
        case 0xC04: return (c->n == 2 || c->n == 3) && be16(c->p) >= 5;   /* ImageGetSize: read-only query */
        case 0x21D: return c->n >= 10 && c->p[4] == 3 && c->p[5] >= 1 && c->p[5] <= 99 && c->n == 6 + 4u * c->p[5];
    }
    return 0;
}

static u32 g_tag = 1;
/* processMessage (request 0x2): u32 clientId, then msg.hpp buildMessage (32-byte header, aligned), then data. */
static int sendCall(const Call *c) {
    u32 host = g_host, self = g_self;
    if (!callOk(c) || host < 0x0100 || host > 0x0FFF || self < 0x2000 || self > 0x2FFF || g_client == 0xffffffffu) {
        say("[probe] call refused by local checks"); return 0;
    }
    u8 r[4 + 32 + 256]; u32 i;
    for (i = 0; i < sizeof(r); ++i) r[i] = 0;
    u8 *m = r + 4;
    r[0] = (u8)g_client; r[1] = (u8)(g_client >> 8); r[2] = (u8)(g_client >> 16); r[3] = (u8)(g_client >> 24);
    m[4] = 1;                                            /* txType 0, varTag 1, varVal 0 */
    m[12] = (u8)host; m[13] = (u8)(host >> 8); m[14] = (u8)self; m[15] = (u8)(self >> 8);
    m[16] = 0x82; m[17] = 0x00; m[18] = (u8)c->fkt; m[19] = (u8)(c->fkt >> 8); m[20] = 6;   /* StartResultAck */
    m[24] = (u8)g_tag; m[25] = (u8)(g_tag >> 8); ++g_tag;
    m[28] = (u8)c->n;
    for (i = 0; i < c->n; ++i) m[32 + i] = c->p[i];
    u32 h = be16(c->p), k = g_recentN++ & 31;
    g_recentFkt[k] = c->fkt; g_recentH[k] = h; g_recentMs[k] = nowMs();
    g_wantFkt = c->fkt; g_wantHandle = h; g_result = 0;
    Line l; l.n = 0; ls(&l, "[probe] send "); lh(&l, c->fkt, 3); ls(&l, " h"); ld(&l, h); ls(&l, " len "); ld(&l, c->n); lend(&l);
    if (!sendRequest(0x2, r, 4 + 32 + c->n)) return 0;
    for (u32 t = 0; t < 150 && g_result == 0; ++t) sleepMs(10);
    if (g_result == 1) return 1;
    say(g_result == 2 ? "[probe] ERROR answer: stopping" : "[probe] no answer in 1500 ms: stopping");
    return 0;
}

static int sendAll(const Call *cs, u32 n, int stopOnNav) {
    for (u32 i = 0; i < n; ++i) {
        if (stopOnNav && g_nav) { say("[probe] factory navigation took the terminal: nothing more sent"); return 0; }
        if (!sendCall(&cs[i])) return 0;
    }
    return 1;
}

static void onSignal(int s) { (void)s; g_stop = 1; }

static u32 parseSecs(const char *s, u32 dflt) {
    if (!s) return dflt;
    u32 v = 0;
    for (; *s; ++s) { if (*s < '0' || *s > '9' || v > 100000) return 0; v = v * 10 + (u32)(*s - '0'); }
    return v;
}
static int streq(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }

static void *need(void *h, const char *n) {
    void *p = dlsym(h, n);
    Line l; l.n = 0; ls(&l, "[dsi] dlsym "); ls(&l, n); ls(&l, p ? " ok" : " MISSING"); lend(&l);
    return p;
}

/* play mode: read HUDTerminal calls from stdin, one per line, as
 *   "<fkt-hex> <payload-hex>"   e.g.  21d 000500000304008000780080...
 * (payload hex may contain spaces; they are ignored). Fills out[], returns the
 * count. callOk() re-validates each before it is sent, so a malformed line can
 * only be dropped or rejected, never widen what reaches the cluster. The host
 * serializer is src/cp_guide/to_calls.py. */
static u32 hexv(u8 c) {
    if (c >= '0' && c <= '9') return (u32)(c - '0');
    if (c >= 'a' && c <= 'f') return (u32)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (u32)(c - 'A' + 10);
    return 16;
}
/* parse one "<fkt-hex> <payload-hex>" line (len bytes, no newline) into *c; 0 if not a call. */
static int parse_call_line(const u8 *s, u32 len, Call *c) {
    u32 i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    u32 fkt = 0, got = 0;                                 /* fkt token: hex until space */
    while (i < len && s[i] != ' ' && s[i] != '\t') {
        u32 v = hexv(s[i]); if (v > 15) return 0; fkt = (fkt << 4) | v; got = 1; i++;
    }
    if (!got) return 0;
    c->fkt = fkt; c->n = 0;                               /* payload: hex byte pairs */
    while (i < len && c->n < sizeof c->p) {
        if (s[i] == ' ' || s[i] == '\t') { i++; continue; }
        u32 hi = hexv(s[i++]); if (hi > 15) break;
        u32 lo = (i < len) ? hexv(s[i++]) : 16; if (lo > 15) break;
        c->p[c->n++] = (u8)((hi << 4) | lo);
    }
    return 1;
}

/* play: read ALL of stdin, parse every line into out[]. */
static u32 read_calls(Call *out, u32 max) {
    static u8 buf[8192]; u32 len = 0; long r;
    while (len < sizeof buf && (r = read(0, buf + len, sizeof buf - len)) > 0) len += (u32)r;
    u32 i = 0, nc = 0;
    while (i < len && nc < max) {
        u32 ls = i; while (i < len && buf[i] != '\n') i++;
        u32 le = i; if (le > ls && buf[le - 1] == '\r') le--;
        if (le > ls) { Call c; if (parse_call_line(buf + ls, le - ls, &c)) out[nc++] = c; }
        if (i < len) i++;                                 /* skip the \n */
    }
    return nc;
}

/* stream: batches of call-lines separated by a blank line, read incrementally so a
 * live `tail -f` feed draws each maneuver as it arrives (read() blocks between them). */
static u8 g_rb[8192]; static u32 g_rblen, g_rbpos; static int g_rbeof;
static int rb_fill(void) {                               /* 1 if new bytes read, 0 at EOF/full */
    if (g_rbeof) return 0;
    if (g_rbpos > 0) { u32 k = g_rblen - g_rbpos; for (u32 j = 0; j < k; j++) g_rb[j] = g_rb[g_rbpos + j]; g_rblen = k; g_rbpos = 0; }
    if (g_rblen >= sizeof g_rb) return 0;                 /* full: flush what we have */
    long r = read(0, g_rb + g_rblen, sizeof g_rb - g_rblen);
    if (r <= 0) { g_rbeof = 1; return 0; }
    g_rblen += (u32)r; return 1;
}
/* read one batch into out[]; returns #calls, or -1 at clean EOF with nothing left. */
static int read_batch(Call *out, u32 max) {
    u32 nc = 0; int sawline = 0;
    for (;;) {
        u32 nl = g_rbpos; while (nl < g_rblen && g_rb[nl] != '\n') nl++;
        if (nl >= g_rblen) {                             /* no full line buffered */
            if (rb_fill()) continue;                     /* got more → retry */
            if (g_rbpos < g_rblen) {                      /* EOF/full: trailing partial line */
                u32 ls = g_rbpos, le = g_rblen; if (le > ls && g_rb[le - 1] == '\r') le--;
                if (le > ls && nc < max) { Call c; if (parse_call_line(g_rb + ls, le - ls, &c)) { out[nc++] = c; sawline = 1; } }
                g_rbpos = g_rblen;
            }
            return sawline ? (int)nc : -1;
        }
        u32 ls = g_rbpos, le = nl; if (le > ls && g_rb[le - 1] == '\r') le--;
        g_rbpos = nl + 1;                                 /* consume line + \n */
        if (le <= ls) { if (sawline) return (int)nc; else continue; }   /* blank = batch end */
        sawline = 1;
        if (nc < max) { Call c; if (parse_call_line(g_rb + ls, le - ls, &c)) out[nc++] = c; }
    }
}

/* rgi: one '\n'-terminated line from stdin; 0 when stopped or past g_end. stdin is the capture FILE
 * (< /tmp/cp_rgi.capture): at EOF it waits 200 ms and reads again, following cp_listen's appends like
 * tail -f. A partial last line stays buffered until its newline arrives; an over-long line is dropped. */
static u32 g_end;
static int read_line(u8 **s, u32 *n) {
    for (;;) {
        u32 nl = g_rbpos; while (nl < g_rblen && g_rb[nl] != '\n') nl++;
        if (nl < g_rblen) { *s = g_rb + g_rbpos; *n = nl - g_rbpos; g_rbpos = nl + 1; return 1; }
        if (g_rbpos > 0) { u32 k = g_rblen - g_rbpos; for (u32 j = 0; j < k; j++) g_rb[j] = g_rb[g_rbpos + j]; g_rblen = k; g_rbpos = 0; }
        if (g_rblen >= sizeof g_rb) { g_rblen = 0; continue; }
        long r = read(0, g_rb + g_rblen, sizeof g_rb - g_rblen);
        if (r > 0) { g_rblen += (u32)r; continue; }
        if (g_stop || g_nav || (int)(nowMs() - g_end) >= 0) return 0;
        sleepMs(200);
    }
}
static RrState g_rr;

int main(int argc, char **argv) {
    g_draw = argc >= 2 && streq(argv[1], "draw");
    g_play = argc >= 2 && streq(argv[1], "play");
    g_stream = argc >= 2 && streq(argv[1], "stream");
    g_rgi = argc >= 2 && streq(argv[1], "rgi");
    g_rg = argc >= 2 && streq(argv[1], "rg");
    g_size = argc >= 2 && streq(argv[1], "size");
    if (argc < 2 || (!g_draw && !g_play && !g_stream && !g_rgi && !g_rg && !g_size && !streq(argv[1], "watch"))) { say("usage: hud6wa_probe watch|draw|play|stream|rgi|rg|size [secs]"); _exit(2); }
    int oneshot = g_draw || g_play;                       /* bounded 1..20; stream/watch 1..600 */
    u32 secs = parseSecs(argc >= 3 ? argv[2] : 0, oneshot ? 10 : 60);
    if (secs < 1 || secs > (oneshot ? 20u : g_rg ? 86400u : g_rgi ? 7200u : 600u)) { say("secs out of range (draw/play 1..20, watch/stream 1..600, rgi 1..7200, rg 1..86400)"); _exit(2); }
    if (!g_rgi && !g_rg) signal(1, onSignal);                     /* rgi runs detached on the HU: survive the ssh hangup */
    signal(2, onSignal); signal(15, onSignal);
    alarm(secs + 40);                                    /* hard cap: SIGALRM kills us even if we hang */
    pthread_mutex_init(g_lock, 0);
    say(g_rg ? "[probe] mode rg (librghook state file)" : g_rgi ? "[probe] mode rgi (on-HU decode of cp_listen JSONL from stdin)" : g_stream ? "[probe] mode stream (maneuver batches from stdin until EOF)" :
        g_play ? "[probe] mode play (HUDTerminal calls from stdin)" :
        g_draw ? "[probe] mode draw" : "[probe] mode watch (receive only: no MOST message is sent)");

    void *lib = dlopen(LIB, RTLD_NOW);
    if (!lib) { Line l; l.n = 0; ls(&l, "[dsi] dlopen failed: "); ls(&l, dlerror()); lend(&l); quit(1); }
    connCtor = (FnConnCtor)need(lib, "_ZN14CDSIConnectionC1Ev");
    connAdd = (FnConnAdd)need(lib, "_ZN14CDSIConnection3addER10CDSIClient");
    connStart = (FnConnStart)need(lib, "_ZN14CDSIConnection5startEv");
    cliCtor = (FnCliCtor)need(lib, "_ZN10CDSIClientC1EPKcS1_ii");
    setNotif = (FnSetNotif)need(lib, "_ZN10CDSIClient15setNotificationEj");
    streamCtor = (FnStreamCtor)need(lib, "_ZN11CDSIOStreamC1ER11CDSIChannelP13CDSITCPSocketiN3DSI11RequestTypeENS4_7CommandEjRK8SPartyIDS9_i");
    streamWrite = (FnStreamWrite)need(lib, "_ZN11CDSIOStream5writeEPKvj");
    streamSend = (FnStreamSend)need(lib, "_ZN11CDSIOStream4sendEi");
    streamDtor = (FnStreamDtor)need(lib, "_ZN11CDSIOStreamD1Ev");
    vtable = need(lib, "_ZTV10CDSIClient");
    if (!connCtor || !connAdd || !connStart || !cliCtor || !setNotif || !streamCtor || !streamWrite || !streamSend ||
        !streamDtor || !vtable) quit(1);

    g_conn = calloc(1, 0x8c + 64);
    g_cli = (char *)calloc(1, 0x14c + 64);
    void **vt = (void **)calloc(8, sizeof(void *));      /* this build's CDSIClient vtable: exactly 8 slots */
    if (!g_conn || !g_cli || !vt) quit(1);
    connCtor(g_conn);
    cliCtor(g_cli, "DSYSMOSTServer", "MOSTSubSystem", 4, 5);
    for (u32 i = 0; i < 8; ++i) vt[i] = ((void **)((char *)vtable + 8))[i];
    vt[3] = (void *)onAttached; vt[4] = (void *)onDetached; vt[7] = (void *)onData;
    *(void ***)g_cli = vt;
    { Line l; l.n = 0; ls(&l, "[dsi] CDSIConnection::add -> "); ld(&l, (u32)connAdd(g_conn, g_cli)); lend(&l); }
    int th;
    if (pthread_create(&th, 0, connThread, 0) != 0) { say("[dsi] thread FAILED"); quit(1); }

    /* wait for registration and the registry */
    for (u32 t = 0; t < 1500 && !g_stop && !(g_answered && g_hosts8200 != 0xffffffffu); ++t) sleepMs(10);
    if (!g_answered || g_client == 0xffffffffu) { say("[probe] not registered: nothing sent"); quit(1); }
    { Line l; l.n = 0; ls(&l, "[probe] 82/00 hosts: "); ld(&l, g_hosts8200 == 0xffffffffu ? 0 : g_hosts8200); ls(&l, ", using ");
      lh(&l, g_host, 4); lend(&l); }

    if (g_size) {        /* read-only: ImageGetSize for the picture buffers 5 and 6, both answer forms; nothing is drawn */
        Call q[4]; q[0] = c2(0xC04, 5); q[1] = c3(0xC04, 5, 1); q[2] = c2(0xC04, 6); q[3] = c3(0xC04, 6, 1);
        for (u32 i = 0; i < 4; ++i) { g_result = 0; sendCall(&q[i]); sleepMs(300); }
        say("[probe] size: answers are the fkt=C04 lines above ([handle][w][h] or [handle][01][w][h], u16 big-endian)");
        quit(0);
    }
    if (!g_draw && !g_play && !g_stream && !g_rgi && !g_rg) {
        for (u32 t = 0; t < secs * 10 && !g_stop; ++t) sleepMs(100);
        Line l; l.n = 0; ls(&l, "[probe] watch done: FB82 msgs "); ld(&l, g_fb82); ls(&l, ", factory-nav msgs "); ld(&l, g_navMsgs);
        ls(&l, g_nav ? " (factory navigation WAS active)" : " (no factory navigation seen)"); lend(&l);
        quit(0);
    }

    if (g_hosts8200 != 1 || !g_host || g_self < 0x2000 || g_self > 0x2FFF) { say("[probe] need exactly one 82/00 ring-node host and an app address: nothing sent"); quit(1); }
    for (u32 t = 0; t < 30 && !g_stop; ++t) sleepMs(100);  /* 3 s quiet: factory traffic would show here */
    if (g_nav || g_stop) { say("[probe] factory navigation active or stopped: nothing sent"); quit(1); }

    /* shared clear batch (used by one-shot and after a stream ends) */
    Call draw[32], clr[8]; u32 n = 0;
    clr[n++] = c3(0x204, 1, 0); clr[n++] = c2(0x220, 5); clr[n++] = c2(0x220, 6); clr[n++] = cSwitch(5, 6);
    clr[n++] = cText(2, ""); clr[n++] = cText(3, ""); clr[n++] = cText(4, ""); clr[n++] = c3(0x204, 2, 0);
    for (u32 i = 0; i < 8; ++i) if (!callOk(&clr[i])) quit(9);

    if (g_rg) {
        /* Poll librghook's state file (it is rewritten, via rename, on every route guidance message). Draw only on a change.
         * The file lives in RAM (/dev/shmem); a missing file = no session yet. Bounded by secs; clears on the way out. */
        const char *path = argc >= 4 ? argv[3] : "/dev/shmem/routeguidance";
        static char cur[4096], prev[4096]; u32 plen = 0, updates = 0, clears = 0, end = nowMs() + secs * 1000u;
        rr_init(&g_rr);
        while (!g_stop && !g_nav && (int)(nowMs() - end) < 0) {
            sleepMs(250);
            int fd = open(path, 0);
            if (fd < 0) continue;
            long r = read(fd, cur, sizeof cur - 1);
            close(fd);
            if (r <= 0) continue;
            u32 n = (u32)r, same = n == plen;
            for (u32 i = 0; same && i < n; ++i) same = cur[i] == prev[i];
            if (same) continue;
            for (u32 i = 0; i < n; ++i) prev[i] = cur[i];
            plen = n;
            int clear = 0, nd = rr_state_text(&g_rr, cur, n, draw, 32, &clear);
            if (clear) { g_result = 0; if (!sendAll(clr, 8, 0)) break; ++clears; say("[probe] rg: route ended: cleared"); continue; }
            if (nd <= 0) continue;
            int ok = 1; for (int i = 0; i < nd; ++i) if (!callOk(&draw[i])) { ok = 0; break; }
            if (!ok) { say("[probe] rg: bad batch skipped"); continue; }
            g_result = 0;
            if (!sendAll(draw, (u32)nd, 1)) break;
            ++updates;
            Line l; l.n = 0; ls(&l, "[probe] rg: maneuver "); ld(&l, (u32)g_rr.shown); ls(&l, " glyph "); ld(&l, g_rr.man[g_rr.shown].glyph);
            ls(&l, " '"); ls(&l, g_rr.man[g_rr.shown].street); ls(&l, "' "); ls(&l, g_rr.sval); ls(&l, g_rr.sunit); lend(&l);
        }
        { Line l; l.n = 0; ls(&l, "[probe] rg: "); ld(&l, updates); ls(&l, " update(s), "); ld(&l, clears); ls(&l, " clear(s)"); lend(&l); }
        g_result = 0;
        int cleared = g_nav ? 1 : sendAll(clr, 8, 0);
        say(cleared ? "[probe] CLEARED" : "[probe] CLEAR NOT CONFIRMED: run the rollback ladder");
        quit(cleared ? 0 : 3);
    }

    if (g_rgi) {
        /* Decode every forwarded frame here; draw only when the current maneuver (or its distance) changes.
         * Bounded by secs (alarm) / EOF / stop / factory navigation; clears on the way out. */
        rr_init(&g_rr);
        u32 frames = 0, drawn = 0; g_end = nowMs() + secs * 1000u;
        static u8 fr[2048];
        for (;;) {
            u8 *ln; u32 n;
            if (g_stop || g_nav || !read_line(&ln, &n)) break;   /* stop / factory nav / secs elapsed */
            u32 fl = rr_jsonl_frame((const char *)ln, n, fr, sizeof fr);
            if (!fl) continue;
            ++frames;
            int nd = rr_feed(&g_rr, fr, fl, draw, 32);
            if (nd <= 0) continue;
            int ok = 1; for (int i = 0; i < nd; ++i) if (!callOk(&draw[i])) { ok = 0; break; }
            if (!ok) { say("[probe] rgi: bad batch skipped"); continue; }
            g_result = 0;
            if (!sendAll(draw, (u32)nd, 1)) break;
            ++drawn;
            Line l; l.n = 0; ls(&l, "[probe] rgi: maneuver "); ld(&l, (u32)g_rr.shown); ls(&l, " dist '"); ls(&l, g_rr.sval);
            ls(&l, g_rr.sunit); ls(&l, "' (frames "); ld(&l, frames); ls(&l, ")"); lend(&l);
        }
        { Line l; l.n = 0; ls(&l, "[probe] rgi: "); ld(&l, frames); ls(&l, " frame(s), "); ld(&l, drawn); ls(&l, " update(s)"); lend(&l); }
        g_result = 0;
        int cleared = g_nav ? 1 : sendAll(clr, 8, 0);
        say(cleared ? "[probe] CLEARED" : "[probe] CLEAR NOT CONFIRMED: run the rollback ladder");
        quit(cleared ? 0 : 3);
    }

    if (g_stream) {
        /* Draw each maneuver batch (blank-line-delimited) as it arrives on stdin. For a
         * live `tail -f` feed, read_batch blocks between maneuvers so the current one
         * stays shown. Ends at EOF, factory-nav, or stop; clears on the way out. */
        u32 shown = 0;
        for (;;) {
            if (g_stop || g_nav) break;
            int nd = read_batch(draw, 32);
            if (nd < 0) break;                           /* EOF: feed closed */
            if (nd == 0) continue;                       /* stray blank line */
            int ok = 1; for (int i = 0; i < nd; ++i) if (!callOk(&draw[i])) { ok = 0; break; }
            if (!ok) { say("[probe] stream: bad batch skipped"); continue; }
            g_result = 0;
            if (!sendAll(draw, (u32)nd, 1)) break;       /* nav took the terminal / send failed */
            ++shown;
        }
        { Line l; l.n = 0; ls(&l, "[probe] stream: drew "); ld(&l, shown); ls(&l, " maneuver(s)"); lend(&l); }
        g_result = 0;
        int cleared = g_nav ? 1 : sendAll(clr, 8, 0);
        say(cleared ? "[probe] CLEARED" : "[probe] CLEAR NOT CONFIRMED: run the rollback ladder");
        quit(cleared ? 0 : 3);
    }

    /* one-shot: play (calls from stdin) or draw (hardcoded test arrow) */
    u32 nd = 0;
    if (g_play) {
        nd = read_calls(draw, 32);                       /* HUDTerminal calls from stdin (to_calls.py) */
        if (nd == 0) { say("[probe] play: no valid calls on stdin"); quit(2); }
        { Line l; l.n = 0; ls(&l, "[probe] play: "); ld(&l, nd); ls(&l, " call(s) from stdin"); lend(&l); }
    } else {
        draw[nd++] = c2(0x220, 5); draw[nd++] = c3(0x21B, 5, 4); draw[nd++] = cArrow(); draw[nd++] = cSwitch(6, 5);
        draw[nd++] = c2(0x220, 6); draw[nd++] = c3(0x204, 2, 1); draw[nd++] = c3(0x204, 3, 1); draw[nd++] = c3(0x204, 4, 1);
        draw[nd++] = cText(2, "PROBE"); draw[nd++] = cText(3, "250"); draw[nd++] = cText(4, "m"); draw[nd++] = c3(0x204, 1, 1);
    }
    for (u32 i = 0; i < nd; ++i) if (!callOk(&draw[i])) quit(9);

    int drawn = sendAll(draw, nd, 1);
    say(drawn ? "[probe] DRAWN: look at the 6WA and the HUD now" : "[probe] draw incomplete");
    for (u32 t = 0; drawn && t < secs * 10 && !g_stop && !g_nav; ++t) sleepMs(100);
    if (g_nav) { say("[probe] factory navigation took the terminal: it clears it itself"); quit(drawn ? 0 : 4); }
    g_result = 0;
    int cleared = sendAll(clr, 8, 0);
    say(cleared ? "[probe] CLEARED" : "[probe] CLEAR NOT CONFIRMED: run the rollback ladder");
    quit(!cleared ? 3 : drawn ? 0 : 4);
    return 0;
}
