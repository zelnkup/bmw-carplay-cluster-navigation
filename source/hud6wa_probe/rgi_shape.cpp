// rgi_shape.cpp — glue from our C renderer to st1ng's hudshapes.h (vendored unedited, Mapbox drawings, ISC + PolyForm NC).
// Freestanding C++: no exceptions, no RTTI, no libc but memset (rgi_mem.c). Big state is static (the renderer is one thread).
extern "C" void *memset(void *, int, unsigned long);
extern "C" void *memcpy(void *, const void *, unsigned long);
#include "../../third_party/nbtevo-carplay-advanced/src/hudshapes.h"
#include "rgi_shape.h"

static hud::Shape g_s;
static hud::Polys g_m;

extern "C" int rr_shape(unsigned type, int angle, unsigned side, unsigned jtype, const int *jang, unsigned njang,
                        const RrLane *lanes, unsigned nlanes, int x0, int y0, int x1, int y1, RrPoly *out, int max) {
  g_s = hud::shapeOf(type, angle, side, jtype, jang, njang);
  if (nlanes) {
    hud::Lane L[8];
    unsigned n = nlanes > 8 ? 8 : nlanes;
    for (unsigned i = 0; i < n; i++) {
      L[i].status = lanes[i].status;
      L[i].hl = lanes[i].hl;
      L[i].na = lanes[i].na > 4 ? 4 : lanes[i].na;
      for (unsigned a = 0; a < 4; a++) L[i].ang[a] = lanes[i].ang[a];
    }
    hud::addLanes(g_s, L, n, side == 1);
  }
  hud::mergePieces(g_s, g_m);
  hud::Box b = {x0, y0, x1, y1};
  int k = 0;
  for (unsigned i = 0; i < g_m.n && k < max; i++) {
    hud::Poly &q = g_m.q[i];
    unsigned n = hud::toBoxPoly(q.p, q.n, b);
    if (n < 3) continue;
    RrPoly &o = out[k++];
    o.tone = q.t;
    o.n = (unsigned char)n;
    for (unsigned j = 0; j < n; j++) { o.p[j][0] = q.p[j].x; o.p[j][1] = q.p[j].y; }
  }
  return k;
}
