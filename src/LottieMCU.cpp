/*
 * LottieMCU - minimal-footprint Lottie animation player for microcontrollers
 *
 * Version 1.1.0
 * Copyright (c) 2026 Jobit Joseph <jobitjoseph1@gmail.com>
 * https://github.com/jobitjoseph/LottieMCU
 *
 * SPDX-License-Identifier: MIT
 * Released under the MIT License - see the LICENSE file for details.
 */
#include "LottieMCU.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

#if defined(__AVR__)
#include <avr/pgmspace.h>
#define LM_RB(p) pgm_read_byte(p)
#else
#define LM_RB(p) (*(const uint8_t *)(p))
#endif

namespace {

inline uint16_t rdU16(const uint8_t *p) { return LM_RB(p) | (uint16_t(LM_RB(p + 1)) << 8); }
inline int16_t rdI16(const uint8_t *p) { return (int16_t)rdU16(p); }
inline float rdF32(const uint8_t *p) {
  uint32_t v = LM_RB(p) | (uint32_t(LM_RB(p + 1)) << 8) |
               (uint32_t(LM_RB(p + 2)) << 16) | (uint32_t(LM_RB(p + 3)) << 24);
  float f;
  memcpy(&f, &v, 4);
  return f;
}

// Size of a property record (see tools/lottie2mcu.py for the layout).
const uint8_t *skipProp(const uint8_t *p) {
  uint8_t kind = LM_RB(p);
  if (kind == 2) return p + 1;
  uint16_t dims = rdU16(p + 1);
  p += 7;
  if (kind == 0) return p + 2 * dims;
  uint16_t nk = rdU16(p);
  p += 2;
  for (uint16_t i = 0; i < nk; i++) p += 3 + ((LM_RB(p + 2) & 2) ? 6 : 0) + 2 * dims;
  return p;
}

inline float bez1(float u, float p1, float p2) {
  float v = 1.0f - u;
  return 3.0f * v * v * u * p1 + 3.0f * v * u * u * p2 + u * u * u;
}

// Cubic-bezier easing (0,0)-(ox,oy)-(ix,iy)-(1,1): find u with x(u)=x, return y(u).
float ease(float x, float ox, float oy, float ix, float iy) {
  float u = x;
  for (int i = 0; i < 6; i++) {  // Newton
    float err = bez1(u, ox, ix) - x;
    if (fabsf(err) < 1e-4f) return bez1(u, oy, iy);
    float v = 1.0f - u;
    float d = 3.0f * v * v * ox + 6.0f * v * u * (ix - ox) + 3.0f * u * u * (1.0f - ix);
    if (fabsf(d) < 1e-5f) break;
    u -= err / d;
    if (u < 0.0f || u > 1.0f) break;
  }
  float lo = 0.0f, hi = 1.0f;  // bisection fallback
  for (int i = 0; i < 18; i++) {
    u = 0.5f * (lo + hi);
    if (bez1(u, ox, ix) < x) lo = u; else hi = u;
  }
  return bez1(0.5f * (lo + hi), oy, iy);
}

inline int floor16(int32_t v) { return v >= 0 ? (int)(v >> 4) : -(int)((-v + 15) >> 4); }

inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t a5) {  // a5: 0..32
  uint32_t b = (bg | ((uint32_t)bg << 16)) & 0x07E0F81Fu;
  uint32_t f = (fg | ((uint32_t)fg << 16)) & 0x07E0F81Fu;
  uint32_t r = ((f * a5 + b * (32u - a5)) >> 5) & 0x07E0F81Fu;
  return (uint16_t)(r | (r >> 16));
}

const float kZero2[2] = {0, 0};
const float kScale[2] = {100, 100};
const float kHundred[1] = {100};
const float kOne[1] = {1};
const float kBlack[3] = {0, 0, 0};
const float KAPPA = 0.5522847f;


}  // namespace

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------
// large = a working table that may live in PSRAM; the strip buffer and scanline
// buffers are not "large" and stay in internal RAM for speed.
void *LottieMCU::alloc(uint32_t bytes, bool large) {
  if (bytes == 0) bytes = 4;
  void *p = nullptr;
#if defined(ARDUINO_ARCH_ESP32)
  bool psram = _cfg.usePSRAM && psramFound();
  if (large && psram) {
    p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) { _ramUsed += bytes; _psramUsed += bytes; return p; }
  }
  p = malloc(bytes);
  if (!p && psram) {  // internal RAM exhausted: PSRAM is better than failing
    p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) { _ramUsed += bytes; _psramUsed += bytes; return p; }
  }
#else
  (void)large;
  p = malloc(bytes);
#endif
  if (p) _ramUsed += bytes;
  return p;
}

void LottieMCU::freeAll() {
  void *bufs[] = {_pathOffs, _nodes, _trimVals, _pts, _contours, _items, _curve, _verts,
                  _cross, _band, _cov, _cov2, _clips, _grads, _imgOffs, _imgs};
  for (void *b : bufs) free(b);  // free() also releases heap_caps (PSRAM) blocks
  _pathOffs = nullptr; _nodes = nullptr; _trimVals = nullptr; _pts = nullptr;
  _contours = nullptr; _items = nullptr; _curve = nullptr; _verts = nullptr;
  _cross = nullptr; _band = nullptr; _cov = nullptr; _cov2 = nullptr; _clips = nullptr;
  _grads = nullptr; _imgOffs = nullptr; _imgs = nullptr;
  _ramUsed = _psramUsed = 0;
}

void LottieMCU::end() {
  freeAll();
  _data = nullptr;
  _status = NOT_STARTED;
}

bool LottieMCU::begin(const uint8_t *data, uint16_t width, uint16_t height, const Config &cfg) {
  end();
  _cfg = cfg;
  if (_cfg.maxPathVerts < 8) _cfg.maxPathVerts = 8;  // rect/ellipse need 8
  if (_cfg.subSamples != 1 && _cfg.subSamples != 2 && _cfg.subSamples != 8) _cfg.subSamples = 4;
  if (_cfg.bandHeight == 0) _cfg.bandHeight = 1;
  if (_cfg.maxCurvePoints < 16) _cfg.maxCurvePoints = 16;
  _status = ERR_FORMAT;
  if (!data || LM_RB(data) != 'L' || LM_RB(data + 1) != 'M' || LM_RB(data + 2) != 'C' ||
      LM_RB(data + 3) != '1')
    return false;
  _data = data;
  _w = width; _h = height;
  _cw = rdU16(data + 4); _ch = rdU16(data + 6);
  _fr = rdF32(data + 8); _ip = rdF32(data + 12); _op = rdF32(data + 16);
  _nNodes = rdU16(data + 20); _nPaths = rdU16(data + 22);
  _nTrims = rdU16(data + 24); _nItems = rdU16(data + 26); _nClips = rdU16(data + 28);
  _nImages = rdU16(data + 30);
  uint32_t len = rdU16(data + 32) | ((uint32_t)rdU16(data + 34) << 16);

  _status = ERR_NOMEM;
  _pathOffs = (uint32_t *)alloc(sizeof(uint32_t) * _nPaths, true);
  _imgOffs = (uint32_t *)alloc(sizeof(uint32_t) * _nImages, true);
  if (!_pathOffs || !_imgOffs) { freeAll(); return false; }

  // Walk the file once to index the sections.
  const uint8_t *p = data + 36;
  _offNodes = 36;
  for (uint16_t i = 0; i < _nNodes; i++) {
    uint8_t flags = LM_RB(p + 16);
    p += 17;
    int nprops = 5 + ((flags & 1) ? 1 : 0) + ((flags & 2) ? 2 : 0) + ((flags & 4) ? 2 : 0);
    for (int k = 0; k < nprops; k++) p = skipProp(p);
  }
  for (uint16_t i = 0; i < _nPaths; i++) {
    _pathOffs[i] = (uint32_t)(p - data);
    uint8_t type = LM_RB(p + 4);
    p += 6;
    int nprops = type == 0 ? 1 : (type == 1 ? 3 : 2);
    for (int k = 0; k < nprops; k++) p = skipProp(p);
  }
  _offTrims = (uint32_t)(p - data);
  for (uint16_t i = 0; i < _nTrims; i++) {
    p += 4;
    for (int k = 0; k < 3; k++) p = skipProp(p);
  }
  _offClips = (uint32_t)(p - data);
  for (uint16_t i = 0; i < _nClips; i++) p += 4 + 3 * rdU16(p + 2);
  for (uint16_t i = 0; i < _nImages; i++) {  // u16 w, h, f32 kx, ky, i16 ox, oy, u8 flags, pad
    _imgOffs[i] = (uint32_t)(p - data);
    uint32_t px = (uint32_t)rdU16(p) * rdU16(p + 2);
    p += 18 + px * 2 + ((LM_RB(p + 16) & 1) ? px : 0);
  }
  _offItems = (uint32_t)(p - data);
  for (uint16_t i = 0; i < _nItems; i++) {
    uint8_t type = LM_RB(p);
    p += 8;
    if (type & 4) { p += 2; continue; }  // image: u16 image index
    if (!(type & 2)) p = skipProp(p);   // colour
    p = skipProp(p);                     // opacity
    if (type & 1) p = skipProp(p);       // width
    if (type & 2) {                      // gradient
      p += 3;
      for (int k = 0; k < 3; k++) p = skipProp(p);
    }
    p += 2 + 2 * rdU16(p);
  }
  if ((uint32_t)(p - data) != len) { _status = ERR_FORMAT; freeAll(); return false; }

  // hot per-pixel buffers first, in internal RAM
  _band = (uint16_t *)alloc(sizeof(uint16_t) * _w * _cfg.bandHeight, false);
  _cov = (uint16_t *)alloc(sizeof(uint16_t) * (_w + 1), false);
  _cov2 = (uint16_t *)alloc(sizeof(uint16_t) * (_w + 1), false);
  _cross = (Cross *)alloc(sizeof(Cross) * _cfg.maxCrossings, false);
  // working tables (PSRAM when enabled)
  _nodes = (Node *)alloc(sizeof(Node) * _nNodes, true);
  _trimVals = (float *)alloc(sizeof(float) * 3 * _nTrims, true);
  _pts = (int16_t *)alloc(sizeof(int16_t) * 2 * _cfg.maxPoints, true);
  _contours = (Contour *)alloc(sizeof(Contour) * _cfg.maxContours, true);
  _items = (Item *)alloc(sizeof(Item) * _cfg.maxItems, true);
  _curve = (float *)alloc(sizeof(float) * 4 * _cfg.maxCurvePoints, true);
  _verts = (float *)alloc(sizeof(float) * 6 * _cfg.maxPathVerts, true);
  _clips = (Item *)alloc(sizeof(Item) * _nClips, true);
  _grads = (Grad *)alloc(sizeof(Grad) * _cfg.maxGradients, true);
  _imgs = (ImgInst *)alloc(sizeof(ImgInst) * _cfg.maxImages, true);
  if (!_nodes || !_trimVals || !_pts || !_contours || !_items || !_curve || !_verts ||
      !_cross || !_band || !_cov || !_cov2 || !_clips || !_grads || !_imgs) {
    freeAll();
    return false;
  }
  memset(_cov, 0, sizeof(uint16_t) * (_w + 1));
  memset(_cov2, 0, sizeof(uint16_t) * (_w + 1));
  // Default placement: fit inside the output, centred.
  float s = fminf((float)_w / (_cw ? _cw : 1), (float)_h / (_ch ? _ch : 1));
  setTransform(s, (_w - _cw * s) * 0.5f, (_h - _ch * s) * 0.5f);
  _status = OK;
  return true;
}

uint32_t LottieMCU::ramNeeded(const Config &c, uint16_t width) {
  uint32_t verts = c.maxPathVerts < 8 ? 8 : c.maxPathVerts;
  return (uint32_t)c.maxPoints * 4 + (uint32_t)c.maxContours * sizeof(Contour) +
         (uint32_t)c.maxItems * sizeof(Item) + (uint32_t)c.maxCurvePoints * 16 + verts * 24 +
         (uint32_t)c.maxCrossings * sizeof(Cross) + (uint32_t)c.maxGradients * sizeof(Grad) +
         (uint32_t)c.maxImages * sizeof(ImgInst) +
         (uint32_t)width * 2 * c.bandHeight + (uint32_t)(width + 1) * 4;
}

void LottieMCU::setTransform(float scale, float offsetX, float offsetY) {
  _root[0] = scale; _root[1] = 0; _root[2] = 0; _root[3] = scale;
  _root[4] = offsetX; _root[5] = offsetY;
}

// ---------------------------------------------------------------------------
// property evaluation
// ---------------------------------------------------------------------------
const uint8_t *LottieMCU::evalProp(const uint8_t *p, float t, float *out, uint16_t cap,
                                   const float *def, uint16_t *dimsOut) {
  uint8_t kind = LM_RB(p);
  if (kind == 2) {
    if (def) for (uint16_t i = 0; i < cap; i++) out[i] = def[i];
    if (dimsOut) *dimsOut = def ? cap : 0;
    return p + 1;
  }
  uint16_t dims = rdU16(p + 1);
  float sc = rdF32(p + 3);
  p += 7;
  uint16_t n = dims;
  if (n > cap) { n = cap; _stats.overflowVerts = true; }
  if (dimsOut) *dimsOut = n;
  if (kind == 0) {
    for (uint16_t i = 0; i < n; i++) out[i] = rdI16(p + 2 * i) * sc;
    return p + 2 * dims;
  }
  uint16_t nk = rdU16(p);
  p += 2;
  const uint8_t *a = nullptr, *b = nullptr;  // keys around t
  float ta = 0, tb = 0;
  for (uint16_t i = 0; i < nk; i++) {
    float kt = rdI16(p) * 0.25f;
    uint8_t fl = LM_RB(p + 2);
    if (kt <= t) { a = p; ta = kt; }
    else if (!b) { b = p; tb = kt; }
    p += 3 + ((fl & 2) ? 6 : 0) + 2 * dims;
  }
  const uint8_t *k = a ? a : b;
  if (!k) { for (uint16_t i = 0; i < n; i++) out[i] = 0; return p; }
  uint8_t fl = LM_RB(k + 2);
  const uint8_t *va = k + 3 + ((fl & 2) ? 6 : 0);
  if (!a || !b || (fl & 1) || tb <= ta) {  // before first, after last, or hold
    for (uint16_t i = 0; i < n; i++) out[i] = rdI16(va + 2 * i) * sc;
    return p;
  }
  float e = (t - ta) / (tb - ta);
  if (fl & 2)
    e = ease(e, LM_RB(a + 3) * (1.0f / 255), rdI16(a + 5) * (1.0f / 4096),
             LM_RB(a + 4) * (1.0f / 255), rdI16(a + 7) * (1.0f / 4096));
  const uint8_t *vb = b + 3 + ((LM_RB(b + 2) & 2) ? 6 : 0);
  for (uint16_t i = 0; i < n; i++) {
    float x0 = rdI16(va + 2 * i) * sc, x1 = rdI16(vb + 2 * i) * sc;
    out[i] = x0 + (x1 - x0) * e;
  }
  return p;
}

void LottieMCU::evalNodes(float frame) {
  const uint8_t *p = _data + _offNodes;
  for (uint16_t i = 0; i < _nNodes; i++) {
    Node &N = _nodes[i];
    int16_t parent = rdI16(p), opParent = rdI16(p + 2);
    N.tOff = rdF32(p + 4);
    float vin = rdF32(p + 8), vout = rdF32(p + 12);
    uint8_t flags = LM_RB(p + 16);
    p += 17;
    float t = frame - N.tOff;
    float an[2], pos[2], sc[2], r, o;
    p = evalProp(p, t, an, 2, kZero2);
    if (flags & 1) {
      p = evalProp(p, t, &pos[0], 1, kZero2);
      p = evalProp(p, t, &pos[1], 1, kZero2);
    } else {
      p = evalProp(p, t, pos, 2, kZero2);
    }
    p = evalProp(p, t, sc, 2, kScale);
    p = evalProp(p, t, &r, 1, kZero2);
    p = evalProp(p, t, &o, 1, kHundred);

    float rad = r * (float)(M_PI / 180.0);
    float cs = cosf(rad), sn = sinf(rad);
    float sx = sc[0] * 0.01f, sy = sc[1] * 0.01f;
    float sk = 0, sa = 0;
    if (flags & 2) {
      p = evalProp(p, t, &sk, 1, kZero2);
      p = evalProp(p, t, &sa, 1, kZero2);
    }
    if (flags & 4) {  // 3D X/Y rotation, orthographic: foreshortens the axes
      float rx, ry;
      p = evalProp(p, t, &rx, 1, kZero2);
      p = evalProp(p, t, &ry, 1, kZero2);
      sx *= cosf(ry * (float)(M_PI / 180.0));
      sy *= cosf(rx * (float)(M_PI / 180.0));
    }
    float L[6] = {cs * sx, sn * sx, -sn * sy, cs * sy, 0, 0};
    if (flags & 2) {  // skew: L = R(r) * R(sa) * Shear(-tan(sk)) * R(-sa) * S
      float k = tanf(-sk * (float)(M_PI / 180.0));
      float ca = cosf(sa * (float)(M_PI / 180.0)), sa_ = sinf(sa * (float)(M_PI / 180.0));
      // K = R(sa) * [1 k; 0 1] * R(-sa)
      float k00 = 1 - k * ca * sa_, k01 = k * ca * ca;
      float k10 = -k * sa_ * sa_, k11 = 1 + k * ca * sa_;
      // M = R(r) * K * S   (column-major a b c d)
      float r00 = cs, r01 = -sn, r10 = sn, r11 = cs;
      float m00 = r00 * k00 + r01 * k10, m01 = r00 * k01 + r01 * k11;
      float m10 = r10 * k00 + r11 * k10, m11 = r10 * k01 + r11 * k11;
      L[0] = m00 * sx; L[1] = m10 * sx; L[2] = m01 * sy; L[3] = m11 * sy;
    }
    L[4] = pos[0] - (L[0] * an[0] + L[2] * an[1]);
    L[5] = pos[1] - (L[1] * an[0] + L[3] * an[1]);
    const float *P = (parent >= 0 && parent < (int)i) ? _nodes[parent].m : _root;
    N.m[0] = P[0] * L[0] + P[2] * L[1];
    N.m[1] = P[1] * L[0] + P[3] * L[1];
    N.m[2] = P[0] * L[2] + P[2] * L[3];
    N.m[3] = P[1] * L[2] + P[3] * L[3];
    N.m[4] = P[0] * L[4] + P[2] * L[5] + P[4];
    N.m[5] = P[1] * L[4] + P[3] * L[5] + P[5];
    float op = o * 0.01f;
    if (opParent >= 0 && opParent < (int)i) op *= _nodes[opParent].op;
    N.op = op < 0 ? 0 : (op > 1 ? 1 : op);
    N.vis = (frame >= vin && frame < vout) ? 1 : 0;
  }
  p = _data + _offTrims;
  for (uint16_t i = 0; i < _nTrims; i++) {
    float t = frame - rdF32(p);
    p += 4;
    const float s0 = 0, e0 = 100, o0 = 0;
    p = evalProp(p, t, &_trimVals[3 * i + 0], 1, &s0);
    p = evalProp(p, t, &_trimVals[3 * i + 1], 1, &e0);
    p = evalProp(p, t, &_trimVals[3 * i + 2], 1, &o0);
  }
}

// ---------------------------------------------------------------------------
// geometry: paths -> flattened polygons in output pixel space
// ---------------------------------------------------------------------------
void LottieMCU::addPoly(const float *xy, int n, bool orient) {
  if (n < 3 || !_cur) return;
  int8_t want = orient ? 1 : _orient;  // +1: positive area, -1: negative area
  if (_nContours >= _cfg.maxContours || _nPts + n > _cfg.maxPoints) {
    _stats.overflowPoints = true;
    return;
  }
  bool rev = false;
  if (want) {  // stroke pieces / clip polygons need one winding direction to union correctly
    float area = 0;
    for (int i = 0, j = n - 1; i < n; j = i++)
      area += xy[2 * j] * xy[2 * i + 1] - xy[2 * i] * xy[2 * j + 1];
    rev = want > 0 ? area < 0 : area > 0;
  }
  Contour &c = _contours[_nContours++];
  c.start = _nPts;
  c.n = n;
  c.ymin = 32767; c.ymax = -32768;
  for (int k = 0; k < n; k++) {
    int i = rev ? n - 1 - k : k;
    float fx = xy[2 * i] * 16.0f, fy = xy[2 * i + 1] * 16.0f;
    fx = fx < -32000 ? -32000 : (fx > 32000 ? 32000 : fx);
    fy = fy < -32000 ? -32000 : (fy > 32000 ? 32000 : fy);
    int16_t x = (int16_t)lroundf(fx), y = (int16_t)lroundf(fy);
    _pts[2 * _nPts] = x;
    _pts[2 * _nPts + 1] = y;
    _nPts++;
    if (y < c.ymin) c.ymin = y;
    if (y > c.ymax) c.ymax = y;
    if (x < _cur->x0) _cur->x0 = x;
    if (x > _cur->x1) _cur->x1 = x;
  }
  if (c.ymin < _cur->y0) _cur->y0 = c.ymin;
  if (c.ymax > _cur->y1) _cur->y1 = c.ymax;
}

void LottieMCU::addCircle(float cx, float cy, float r) {
  float buf[48];
  int n = (int)(r * 1.5f) + 6;
  if (n > 24) n = 24;
  if (n < 6) n = 6;
  for (int i = 0; i < n; i++) {
    float a = i * (float)(2.0 * M_PI) / n;
    buf[2 * i] = cx + r * cosf(a);
    buf[2 * i + 1] = cy + r * sinf(a);
  }
  addPoly(buf, n, true);
}

int LottieMCU::flatten(const float *v, int nv, bool closed, const float *m, float *out) {
  if (nv <= 0) return 0;
  const int cap = _cfg.maxCurvePoints;
  const float tol = _cfg.tolerance > 0.01f ? _cfg.tolerance : 0.01f;
#define TX(x, y) (m[0] * (x) + m[2] * (y) + m[4])
#define TY(x, y) (m[1] * (x) + m[3] * (y) + m[5])
  int cnt = 0;
  out[cnt * 2] = TX(v[0], v[1]);
  out[cnt * 2 + 1] = TY(v[0], v[1]);
  cnt++;
  int segs = closed ? nv : nv - 1;
  for (int s = 0; s < segs; s++) {
    const float *a = v + 6 * s, *b = v + 6 * ((s + 1) % nv);
    // a: [vx vy ix iy ox oy]; control points use a.out and b.in
    float x0 = a[0], y0 = a[1], x1 = a[0] + a[4], y1 = a[1] + a[5];
    float x2 = b[0] + b[2], y2 = b[1] + b[3], x3 = b[0], y3 = b[1];
    float P0x = TX(x0, y0), P0y = TY(x0, y0), P1x = TX(x1, y1), P1y = TY(x1, y1);
    float P2x = TX(x2, y2), P2y = TY(x2, y2), P3x = TX(x3, y3), P3y = TY(x3, y3);
    bool last = closed && s == segs - 1;
    int n = 1;
    if (a[4] != 0 || a[5] != 0 || b[2] != 0 || b[3] != 0) {
      float ddx = fabsf(P0x - 2 * P1x + P2x), ddy = fabsf(P0y - 2 * P1y + P2y);
      float d1 = sqrtf(ddx * ddx + ddy * ddy);
      ddx = fabsf(P1x - 2 * P2x + P3x); ddy = fabsf(P1y - 2 * P2y + P3y);
      float d2 = sqrtf(ddx * ddx + ddy * ddy);
      float dd = d1 > d2 ? d1 : d2;
      n = (int)ceilf(sqrtf(0.75f * dd / tol));
      if (n < 1) n = 1;
      if (n > 32) n = 32;
    }
    for (int k = 1; k <= n; k++) {
      if (last && k == n) break;  // closing point equals the first point
      if (cnt >= cap) { _stats.overflowCurve = true; return cnt; }
      float u = (float)k / n, w = 1 - u;
      float c0 = w * w * w, c1 = 3 * w * w * u, c2 = 3 * w * u * u, c3 = u * u * u;
      out[cnt * 2] = c0 * P0x + c1 * P1x + c2 * P2x + c3 * P3x;
      out[cnt * 2 + 1] = c0 * P0y + c1 * P1y + c2 * P2y + c3 * P3y;
      cnt++;
    }
  }
#undef TX
#undef TY
  return cnt;
}

void LottieMCU::strokePolyline(const float *p, int n, bool closed, float h, uint8_t cap,
                               uint8_t join, float miter) {
  if (n < 2) return;
  int segs = closed ? n : n - 1;
  float q[8];
  // one quad per segment
  for (int s = 0; s < segs; s++) {
    float ax = p[2 * s], ay = p[2 * s + 1];
    int e = (s + 1) % n;
    float bx = p[2 * e], by = p[2 * e + 1];
    float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f) continue;
    dx /= len; dy /= len;
    if (!closed && cap == 2) {  // square caps extend the end segments
      if (s == 0) { ax -= dx * h; ay -= dy * h; }
      if (s == segs - 1) { bx += dx * h; by += dy * h; }
    }
    float nx = -dy * h, ny = dx * h;
    q[0] = ax + nx; q[1] = ay + ny; q[2] = bx + nx; q[3] = by + ny;
    q[4] = bx - nx; q[5] = by - ny; q[6] = ax - nx; q[7] = ay - ny;
    addPoly(q, 4, true);
  }
  // joins
  for (int i = closed ? 0 : 1; i < (closed ? n : n - 1); i++) {
    int ip = (i - 1 + n) % n, in = (i + 1) % n;
    float vx = p[2 * i], vy = p[2 * i + 1];
    float d1x = vx - p[2 * ip], d1y = vy - p[2 * ip + 1];
    float d2x = p[2 * in] - vx, d2y = p[2 * in + 1] - vy;
    float l1 = sqrtf(d1x * d1x + d1y * d1y), l2 = sqrtf(d2x * d2x + d2y * d2y);
    if (l1 < 1e-4f || l2 < 1e-4f) continue;
    d1x /= l1; d1y /= l1; d2x /= l2; d2y /= l2;
    float cross = d1x * d2y - d1y * d2x, dot = d1x * d2x + d1y * d2y;
    if (dot > 0 && fabsf(cross) * h < 0.15f) continue;  // gap smaller than a pixel fraction
    if (join == 1) { addCircle(vx, vy, h); continue; }
    float sg = cross > 0 ? -h : h;  // outer side of the turn
    float o1x = vx - d1y * sg, o1y = vy + d1x * sg;
    float o2x = vx - d2y * sg, o2y = vy + d2x * sg;
    float mlen = 1.0f / sqrtf(fmaxf(1e-6f, 0.5f * (1 + dot)));  // miter length / h
    if (join == 0 && mlen <= miter) {
      float mx = -(d1y + d2y), my = d1x + d2x;  // sum of normals
      float ml = sqrtf(mx * mx + my * my);
      if (ml > 1e-6f) {
        mx = mx / ml * mlen * sg; my = my / ml * mlen * sg;
        float t4[8] = {vx, vy, o1x, o1y, vx + mx, vy + my, o2x, o2y};
        addPoly(t4, 4, true);
        continue;
      }
    }
    float t3[6] = {vx, vy, o1x, o1y, o2x, o2y};  // bevel
    addPoly(t3, 3, true);
  }
  if (!closed && cap == 1) {
    addCircle(p[0], p[1], h);
    addCircle(p[2 * (n - 1)], p[2 * (n - 1) + 1], h);
  }
}

void LottieMCU::emit(const float *pts, int n, bool closed, bool stroke, float halfW,
                     uint8_t cap, uint8_t join, float miter) {
  if (stroke) strokePolyline(pts, n, closed, halfW, cap, join, miter);
  else addPoly(pts, n, false);
}

void LottieMCU::buildPath(uint16_t idx, float frame, bool stroke, float halfW, uint8_t cap,
                          uint8_t join, float miter) {
  const uint8_t *q = _data + _pathOffs[idx];
  int16_t node = rdI16(q), trim = rdI16(q + 2);
  uint8_t type = LM_RB(q + 4), flags = LM_RB(q + 5);
  q += 6;
  if (node < 0 || node >= (int)_nNodes) return;
  const Node &N = _nodes[node];
  if (!N.vis) return;
  float t = frame - N.tOff;
  float *v = _verts;
  int nv = 0;
  bool closed = true;
  if (type == 0) {
    uint16_t dims = 0;
    evalProp(q, t, v, _cfg.maxPathVerts * 6, nullptr, &dims);
    nv = dims / 6;
    closed = flags & 1;
  } else {
    float pos[2], sz[2], rr = 0;
    q = evalProp(q, t, pos, 2, kZero2);
    q = evalProp(q, t, sz, 2, kZero2);
    float hw = fabsf(sz[0]) * 0.5f, hh = fabsf(sz[1]) * 0.5f;
    float cx = pos[0], cy = pos[1];
    memset(v, 0, sizeof(float) * 48);
    if (type == 1) {
      evalProp(q, t, &rr, 1, kZero2);
      if (rr > hw) rr = hw;
      if (rr > hh) rr = hh;
      if (rr <= 0) {
        float c[8] = {cx + hw, cy - hh, cx + hw, cy + hh, cx - hw, cy + hh, cx - hw, cy - hh};
        for (int i = 0; i < 4; i++) { v[6 * i] = c[2 * i]; v[6 * i + 1] = c[2 * i + 1]; }
        nv = 4;
      } else {
        float k = rr * KAPPA;
        // vx, vy, ix, iy, ox, oy
        const float r8[8][6] = {
            {cx + hw, cy - hh + rr, 0, -k, 0, 0}, {cx + hw, cy + hh - rr, 0, 0, 0, k},
            {cx + hw - rr, cy + hh, k, 0, 0, 0},  {cx - hw + rr, cy + hh, 0, 0, -k, 0},
            {cx - hw, cy + hh - rr, 0, k, 0, 0},  {cx - hw, cy - hh + rr, 0, 0, 0, -k},
            {cx - hw + rr, cy - hh, -k, 0, 0, 0}, {cx + hw - rr, cy - hh, 0, 0, k, 0}};
        memcpy(v, r8, sizeof(r8));
        nv = 8;
      }
    } else {
      float kx = hw * KAPPA, ky = hh * KAPPA;
      const float e4[4][6] = {{cx, cy - hh, -kx, 0, kx, 0}, {cx + hw, cy, 0, -ky, 0, ky},
                              {cx, cy + hh, kx, 0, -kx, 0}, {cx - hw, cy, 0, ky, 0, -ky}};
      memcpy(v, e4, sizeof(e4));
      nv = 4;
    }
  }
  if (nv < 1) return;
  float *A = _curve, *B = _curve + 2 * _cfg.maxCurvePoints;
  int n = flatten(v, nv, closed, N.m, A);
  if (n < 2) return;

  if (trim < 0 || trim >= (int)_nTrims) {
    emit(A, n, closed, stroke, halfW, cap, join, miter);
    return;
  }
  // trim paths: keep the [start, end] fraction of the length, shifted by offset
  float s = _trimVals[3 * trim] * 0.01f, e = _trimVals[3 * trim + 1] * 0.01f;
  float o = _trimVals[3 * trim + 2] / 360.0f;
  s = s < 0 ? 0 : (s > 1 ? 1 : s);
  e = e < 0 ? 0 : (e > 1 ? 1 : e);
  if (s > e) { float tmp = s; s = e; e = tmp; }
  if (e - s <= 1e-4f) return;
  if (e - s >= 0.9999f) { emit(A, n, closed, stroke, halfW, cap, join, miter); return; }
  int segs = closed ? n : n - 1;
  float total = 0;
  for (int i = 0; i < segs; i++) {
    int j = (i + 1) % n;
    total += sqrtf((A[2 * j] - A[2 * i]) * (A[2 * j] - A[2 * i]) +
                   (A[2 * j + 1] - A[2 * i + 1]) * (A[2 * j + 1] - A[2 * i + 1]));
  }
  if (total <= 1e-4f) return;
  float a0 = s + o;
  a0 -= floorf(a0);
  float a1 = a0 + (e - s);
  float ranges[4] = {a0, a1 > 1 ? 1.0f : a1, 0, a1 > 1 ? a1 - 1 : 0};
  for (int r = 0; r < (a1 > 1 ? 2 : 1); r++) {
    float d0 = ranges[2 * r] * total, d1 = ranges[2 * r + 1] * total;
    int m = 0;
    float acc = 0;
    for (int i = 0; i < segs && m < _cfg.maxCurvePoints - 1; i++) {
      int j = (i + 1) % n;
      float dx = A[2 * j] - A[2 * i], dy = A[2 * j + 1] - A[2 * i + 1];
      float L = sqrtf(dx * dx + dy * dy);
      float next = acc + L;
      if (next >= d0 && acc <= d1 && L > 0) {
        if (m == 0) {
          float u = (d0 - acc) / L;
          u = u < 0 ? 0 : u;
          B[0] = A[2 * i] + dx * u; B[1] = A[2 * i + 1] + dy * u; m = 1;
        }
        if (next <= d1) {
          B[2 * m] = A[2 * j]; B[2 * m + 1] = A[2 * j + 1]; m++;
        } else {
          float u = (d1 - acc) / L;
          B[2 * m] = A[2 * i] + dx * u; B[2 * m + 1] = A[2 * i + 1] + dy * u; m++;
          break;
        }
      }
      acc = next;
    }
    if (m >= 2) emit(B, m, false, stroke, halfW, cap, join, miter);
  }
}

bool LottieMCU::clipEmpty(int16_t clip) const {
  for (int guard = 0; clip >= 0 && clip < (int)_nClips && guard < 64; guard++) {
    const Item &c = _clips[clip];
    if (c.c1 == c.c0) return true;
    clip = c.clip;
  }
  return false;
}

static uint16_t to565(const float *c) {
  uint8_t rgb[3];
  for (int k = 0; k < 3; k++) {
    float v = c[k] * 255.0f + 0.5f;
    rgb[k] = v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
  }
  return ((rgb[0] & 0xF8) << 8) | ((rgb[1] & 0xFC) << 3) | (rgb[2] >> 3);
}

void LottieMCU::rgbCol(const float *c, Item &it) { it.color = to565(c); }

// 64-entry colour/alpha table from Lottie stops: [pos r g b]*nc [pos a]*na
void LottieMCU::buildGradient(Grad &g, const float *v, uint8_t nc, uint8_t na) {
  for (int i = 0; i < 64; i++) {
    float t = i / 63.0f, c[3];
    int k = 0;
    while (k < nc - 1 && v[4 * (k + 1)] < t) k++;
    if (k >= nc - 1 || t <= v[4 * k]) {
      for (int j = 0; j < 3; j++) c[j] = v[4 * k + 1 + j];
    } else {
      float span = v[4 * (k + 1)] - v[4 * k];
      float u = span > 1e-6f ? (t - v[4 * k]) / span : 0;
      for (int j = 0; j < 3; j++) c[j] = v[4 * k + 1 + j] + (v[4 * k + 5 + j] - v[4 * k + 1 + j]) * u;
    }
    g.col[i] = to565(c);
    float a = 1;
    if (na) {
      const float *av = v + 4 * nc;
      int m = 0;
      while (m < na - 1 && av[2 * (m + 1)] < t) m++;
      if (m >= na - 1 || t <= av[2 * m]) a = av[2 * m + 1];
      else {
        float span = av[2 * (m + 1)] - av[2 * m];
        float u = span > 1e-6f ? (t - av[2 * m]) / span : 0;
        a = av[2 * m + 1] + (av[2 * m + 3] - av[2 * m + 1]) * u;
      }
    }
    a = a < 0 ? 0 : (a > 1 ? 1 : a);
    g.a[i] = (uint8_t)(a * 255.0f + 0.5f);
  }
}

// Image: its rectangle becomes a polygon (so edges, masks and mattes reuse the
// normal coverage path) plus a screen -> texel mapping for sampling.
bool LottieMCU::buildImage(uint16_t idx, const Node &N, Item &it) {
  const uint8_t *q = _data + _imgOffs[idx];
  uint16_t w = rdU16(q), h = rdU16(q + 2);
  float kx = rdF32(q + 4), ky = rdF32(q + 8);
  int16_t ox = rdI16(q + 12), oy = rdI16(q + 14);
  uint8_t fl = LM_RB(q + 16);
  const float *m = N.m;
  float det = m[0] * m[3] - m[1] * m[2];
  if (!w || !h || kx <= 0 || ky <= 0 || fabsf(det) < 1e-12f) return false;
  float x0 = ox / kx, y0 = oy / ky, x1 = (ox + w) / kx, y1 = (oy + h) / ky;
  const float c[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
  float quad[8];
  for (int i = 0; i < 4; i++) {
    quad[2 * i] = m[0] * c[2 * i] + m[2] * c[2 * i + 1] + m[4];
    quad[2 * i + 1] = m[1] * c[2 * i] + m[3] * c[2 * i + 1] + m[5];
  }
  it.c0 = _nContours;
  it.x0 = it.y0 = 32767;
  it.x1 = it.y1 = -32768;
  _cur = &it;
  addPoly(quad, 4, false);
  _cur = nullptr;
  it.c1 = _nContours;
  if (it.c1 == it.c0) return false;
  it.x0 = floor16(it.x0); it.x1 = floor16(it.x1);
  it.y0 = floor16(it.y0); it.y1 = floor16(it.y1);
  if (it.x1 < 0 || it.y1 < 0 || it.x0 >= _w || it.y0 >= _h) {
    _nContours = it.c0;
    _nPts = _contours[it.c0].start;
    return false;
  }
  ImgInst &im = _imgs[_nImgs];
  im.rgb = q + 18;
  im.alpha = (fl & 1) ? q + 18 + 2 * (uint32_t)w * h : nullptr;
  im.w = w; im.h = h;
  float ia = m[3] / det, ib = -m[2] / det, ic = -m[1] / det, id = m[0] / det;
  im.ua = kx * ia; im.ub = kx * ib; im.uc = -kx * (ia * m[4] + ib * m[5]) - ox;
  im.va = ky * ic; im.vb = ky * id; im.vc = -ky * (ic * m[4] + id * m[5]) - oy;
  float a = N.op * 255.0f + 0.5f;
  it.alpha = a > 255 ? 255 : (uint8_t)a;
  it.rule = 0;
  it.grad = -1;
  it.img = _nImgs++;
  it.color = 0;
  return true;
}

void LottieMCU::buildGeometry(float frame) {
  _nPts = 0; _nContours = 0; _nItemsUsed = 0; _nGrads = 0; _nImgs = 0;
  // clip polygons (masks, track mattes): add = positive area, subtract = negative
  const uint8_t *p = _data + _offClips;
  for (uint16_t i = 0; i < _nClips; i++) {
    Item &cl = _clips[i];
    int16_t parent = rdI16(p);
    uint16_t n = rdU16(p + 2);
    p += 4;
    cl.clip = parent < (int)i ? parent : -1;
    cl.rule = 2;
    cl.c0 = _nContours;
    cl.x0 = cl.y0 = 32767;
    cl.x1 = cl.y1 = -32768;
    _cur = &cl;
    for (uint16_t k = 0; k < n; k++, p += 3) {
      uint16_t pi = rdU16(p);
      _orient = (int8_t)LM_RB(p + 2) < 0 ? -1 : 1;
      if (pi < _nPaths) buildPath(pi, frame, false, 0, 0, 0, 0);
    }
    _orient = 0;
    _cur = nullptr;
    cl.c1 = _nContours;
    cl.x0 = floor16(cl.x0); cl.x1 = floor16(cl.x1);
    cl.y0 = floor16(cl.y0); cl.y1 = floor16(cl.y1);
  }
  p = _data + _offItems;
  for (uint16_t i = 0; i < _nItems; i++) {
    uint8_t type = LM_RB(p), flags = LM_RB(p + 1);
    int16_t node = rdI16(p + 2);
    float miter = rdU16(p + 4) * 0.01f;
    int16_t clip = rdI16(p + 6);
    p += 8;
    const Node &N = _nodes[node >= 0 && node < (int)_nNodes ? node : 0];
    if (type & 4) {  // image layer
      uint16_t idx = rdU16(p);
      p += 2;
      if (!N.vis || idx >= _nImages || N.op <= 0.004f) continue;
      if (clip >= (int)_nClips) clip = -1;
      if (clipEmpty(clip)) continue;
      if (_nItemsUsed >= _cfg.maxItems) { _stats.overflowItems = true; continue; }
      if (_nImgs >= _cfg.maxImages) { _stats.overflowImages = true; continue; }
      Item &it = _items[_nItemsUsed];
      it.clip = clip;
      if (buildImage(idx, N, it)) _nItemsUsed++;
      continue;
    }
    float t = frame - N.tOff;
    float col[3] = {0.5f, 0.5f, 0.5f}, o, w = 0, gs[2], ge[2];
    uint8_t gkind = 0, gnc = 0, gna = 0;
    const uint8_t *stops = nullptr;
    if (!(type & 2)) p = evalProp(p, t, col, 3, kBlack);
    p = evalProp(p, t, &o, 1, kHundred);
    if (type & 1) p = evalProp(p, t, &w, 1, kOne);
    if (type & 2) {
      gkind = LM_RB(p); gnc = LM_RB(p + 1); gna = LM_RB(p + 2);
      p += 3;
      p = evalProp(p, t, gs, 2, kZero2);
      p = evalProp(p, t, ge, 2, kZero2);
      stops = p;
      p = skipProp(p);
    }
    uint16_t nrefs = rdU16(p);
    const uint8_t *refs = p + 2;
    p += 2 + 2 * nrefs;

    if (!N.vis) continue;
    if (_nItemsUsed >= _cfg.maxItems) { _stats.overflowItems = true; continue; }
    if (clip >= (int)_nClips) clip = -1;
    if (clipEmpty(clip)) continue;
    float alpha = o * 0.01f * N.op;
    if (alpha <= 0.004f) continue;
    float halfW = 0;
    if (type & 1) {
      float det = N.m[0] * N.m[3] - N.m[1] * N.m[2];
      halfW = w * 0.5f * sqrtf(fabsf(det));
      if (halfW < 0.05f) continue;
    }
    Item &it = _items[_nItemsUsed];
    it.c0 = _nContours;
    it.x0 = it.y0 = 32767;
    it.x1 = it.y1 = -32768;
    _cur = &it;
    for (uint16_t r = 0; r < nrefs; r++) {
      uint16_t pi = rdU16(refs + 2 * r);
      if (pi < _nPaths)
        buildPath(pi, frame, (type & 1) != 0, halfW, flags & 3, (flags >> 2) & 3, miter);
    }
    _cur = nullptr;
    it.c1 = _nContours;
    if (it.c1 == it.c0) continue;
    rgbCol(col, it);
    it.alpha = (uint8_t)(alpha > 1 ? 255 : alpha * 255.0f + 0.5f);
    it.rule = (type & 1) ? 0 : (flags & 1);
    it.clip = clip;
    it.grad = -1;
    it.img = -1;
    if (stops) {
      uint16_t dims = 0, need = 4 * gnc + 2 * gna;
      evalProp(stops, t, _verts, _cfg.maxPathVerts * 6, nullptr, &dims);
      if (gnc && dims >= need) {
        if (_nGrads < _cfg.maxGradients) {
          Grad &g = _grads[_nGrads];
          buildGradient(g, _verts, gnc, gna);
          float sx = N.m[0] * gs[0] + N.m[2] * gs[1] + N.m[4], sy = N.m[1] * gs[0] + N.m[3] * gs[1] + N.m[5];
          float ex = N.m[0] * ge[0] + N.m[2] * ge[1] + N.m[4], ey = N.m[1] * ge[0] + N.m[3] * ge[1] + N.m[5];
          float dx = ex - sx, dy = ey - sy, l2 = dx * dx + dy * dy;
          g.sx = sx - 0.5f; g.sy = sy - 0.5f;  // sample at pixel centres
          g.radial = gkind == 2;
          if (l2 < 1e-6f) l2 = 1e-6f;
          if (g.radial) { g.ux = 1.0f / sqrtf(l2); g.uy = 0; }
          else { g.ux = dx / l2; g.uy = dy / l2; }
          it.grad = _nGrads++;
        } else {  // out of gradient slots: fall back to the average stop colour
          _stats.overflowGradients = true;
          float avg[3] = {0, 0, 0};
          for (int k = 0; k < gnc; k++)
            for (int j = 0; j < 3; j++) avg[j] += _verts[4 * k + 1 + j] / gnc;
          rgbCol(avg, it);
        }
      }
    }
    it.x0 = floor16(it.x0); it.x1 = floor16(it.x1);
    it.y0 = floor16(it.y0); it.y1 = floor16(it.y1);
    if (it.x1 < 0 || it.y1 < 0 || it.x0 >= _w || it.y0 >= _h) {
      _nContours = it.c0;  // off-screen: drop
      _nPts = _contours[it.c0].start;
      continue;
    }
    _nItemsUsed++;
  }
  _stats.points = _nPts; _stats.contours = _nContours; _stats.items = _nItemsUsed;
}

// ---------------------------------------------------------------------------
// rasteriser: scanline polygon fill with vertical super-sampling and exact
// horizontal coverage, blended into an RGB565 strip
// ---------------------------------------------------------------------------
void LottieMCU::addSpan(uint16_t *cov, int32_t xa, int32_t xb, int &minx, int &maxx) {
  const int32_t W16 = (int32_t)_w * 16;
  if (xa < 0) xa = 0;
  if (xb > W16) xb = W16;
  if (xb <= xa) return;
  const uint16_t wu = 16 / _cfg.subSamples, full = 256 / _cfg.subSamples;
  int p0 = xa >> 4, p1 = xb >> 4;
  if (p0 == p1) {
    cov[p0] += (xb - xa) * wu;
  } else {
    cov[p0] += (16 - (xa & 15)) * wu;
    for (int x = p0 + 1; x < p1; x++) cov[x] += full;
    if (p1 < _w) cov[p1] += (xb & 15) * wu;
  }
  if (p0 < minx) minx = p0;
  int pe = p1 < _w ? p1 : _w - 1;
  if (pe > maxx) maxx = pe;
}

// Accumulate the anti-aliased coverage (0..256) of one pixel row into cov.
// rule: 0 non-zero, 1 even-odd, 2 negative winding (clips)
void LottieMCU::coverRow(const Item &it, int y, uint16_t *cov, int &minx, int &maxx) {
  const uint8_t ss = _cfg.subSamples;
  for (uint8_t s = 0; s < ss; s++) {
    int32_t sy = (int32_t)y * 16 + ((2 * s + 1) * 8) / ss;
    uint16_t nc = 0;
    for (uint16_t c = it.c0; c < it.c1; c++) {
      const Contour &C = _contours[c];
      if (sy < C.ymin || sy >= C.ymax) continue;
      const int16_t *pt = _pts + 2 * C.start;
      int32_t x0 = pt[2 * (C.n - 1)], y0 = pt[2 * (C.n - 1) + 1];
      for (uint16_t k = 0; k < C.n; k++) {
        int32_t x1 = pt[2 * k], y1 = pt[2 * k + 1];
        if ((y0 <= sy && sy < y1) || (y1 <= sy && sy < y0)) {
          if (nc < _cfg.maxCrossings) {
            float fx = x0 + (float)(sy - y0) * (float)(x1 - x0) / (float)(y1 - y0);
            _cross[nc].x = (int16_t)lroundf(fx);
            _cross[nc].d = y1 > y0 ? 1 : -1;
            nc++;
          } else {
            _stats.overflowCross = true;
          }
        }
        x0 = x1; y0 = y1;
      }
    }
    if (nc < 2) continue;
    for (uint16_t i = 1; i < nc; i++) {  // insertion sort by x
      Cross cr = _cross[i];
      int j = i - 1;
      while (j >= 0 && _cross[j].x > cr.x) { _cross[j + 1] = _cross[j]; j--; }
      _cross[j + 1] = cr;
    }
    int wind = 0;
    int32_t xs = 0;
    for (uint16_t i = 0; i < nc; i++) {
      bool before = it.rule == 0 ? wind != 0 : (it.rule == 1 ? (wind & 1) : wind < 0);
      wind += _cross[i].d;
      bool after = it.rule == 0 ? wind != 0 : (it.rule == 1 ? (wind & 1) : wind < 0);
      if (!before && after) xs = _cross[i].x;
      else if (before && !after) addSpan(cov, xs, _cross[i].x, minx, maxx);
    }
  }
}

void LottieMCU::rasterRow(const Item &it, int y, uint16_t *row) {
  if (y < it.y0 || y > it.y1) return;
  int minx = _w, maxx = -1;
  coverRow(it, y, _cov, minx, maxx);
  if (maxx < minx) return;
  // multiply by every clip in the chain (mask, matte, enclosing precomp masks)
  int16_t clip = it.clip;
  for (int guard = 0; clip >= 0 && guard < 64; guard++) {
    const Item &cl = _clips[clip];
    int cmin = _w, cmax = -1;
    if (y >= cl.y0 && y <= cl.y1) coverRow(cl, y, _cov2, cmin, cmax);
    for (int x = minx; x <= maxx; x++) {
      uint32_t m = (x >= cmin && x <= cmax) ? _cov2[x] : 0;
      if (m > 256) m = 256;
      uint32_t c = _cov[x] > 256 ? 256 : _cov[x];
      _cov[x] = (uint16_t)((c * m) >> 8);
    }
    for (int x = cmin; x <= cmax; x++) _cov2[x] = 0;
    clip = cl.clip;
  }
  if (it.img >= 0) { blendImageRow(it, y, minx, maxx, row); return; }
  const uint16_t a = it.alpha + 1;
  if (it.grad >= 0) {
    const Grad &g = _grads[it.grad];
    float fy = y - g.sy;
    for (int x = minx; x <= maxx; x++) {
      uint16_t c = _cov[x];
      if (!c) continue;
      _cov[x] = 0;
      if (c > 255) c = 255;
      float fx = x - g.sx;
      float t = g.radial ? sqrtf(fx * fx + fy * fy) * g.ux : fx * g.ux + fy * g.uy;
      int idx = (int)(t * 63.0f + 0.5f);
      idx = idx < 0 ? 0 : (idx > 63 ? 63 : idx);
      uint16_t a8 = (c * a) >> 8;
      a8 = (a8 * (g.a[idx] + 1)) >> 8;
      uint8_t a5 = (uint8_t)((a8 + 4) >> 3);
      if (a5 >= 32) row[x] = g.col[idx];
      else if (a5) row[x] = blend565(row[x], g.col[idx], a5);
    }
    return;
  }
  const uint16_t color = it.color;
  for (int x = minx; x <= maxx; x++) {
    uint16_t c = _cov[x];
    if (!c) continue;
    _cov[x] = 0;
    if (c > 255) c = 255;
    uint8_t a5 = (uint8_t)((((c * a) >> 8) + 4) >> 3);
    if (a5 >= 32) row[x] = color;
    else if (a5) row[x] = blend565(row[x], color, a5);
  }
}

// Sample the image for each covered pixel (bilinear or nearest) and blend.
void LottieMCU::blendImageRow(const Item &it, int y, int minx, int maxx, uint16_t *row) {
  const ImgInst &im = _imgs[it.img];
  const int w = im.w, h = im.h;
  const uint16_t ia = it.alpha + 1;
  float fx0 = minx + 0.5f, fy = y + 0.5f;
  float u = im.ua * fx0 + im.ub * fy + im.uc;
  float v = im.va * fx0 + im.vb * fy + im.vc;
  for (int x = minx; x <= maxx; x++, u += im.ua, v += im.va) {
    uint16_t c = _cov[x];
    if (!c) continue;
    _cov[x] = 0;
    if (c > 255) c = 255;
    uint16_t col;
    uint16_t ta;
    if (_cfg.bilinear) {
      float uu = u - 0.5f, vv = v - 0.5f;
      int xi = (int)floorf(uu), yi = (int)floorf(vv);
      uint32_t fx = (uint32_t)((uu - xi) * 256.0f), fyw = (uint32_t)((vv - yi) * 256.0f);
      int xa = xi < 0 ? 0 : (xi >= w ? w - 1 : xi), xb = xi + 1 < 0 ? 0 : (xi + 1 >= w ? w - 1 : xi + 1);
      int ya = yi < 0 ? 0 : (yi >= h ? h - 1 : yi), yb = yi + 1 < 0 ? 0 : (yi + 1 >= h ? h - 1 : yi + 1);
      uint32_t i00 = (uint32_t)ya * w + xa, i10 = (uint32_t)ya * w + xb;
      uint32_t i01 = (uint32_t)yb * w + xa, i11 = (uint32_t)yb * w + xb;
      uint16_t c00 = rdU16(im.rgb + 2 * i00), c10 = rdU16(im.rgb + 2 * i10);
      uint16_t c01 = rdU16(im.rgb + 2 * i01), c11 = rdU16(im.rgb + 2 * i11);
      uint32_t w00 = (256 - fx) * (256 - fyw), w10 = fx * (256 - fyw);
      uint32_t w01 = (256 - fx) * fyw, w11 = fx * fyw;
      uint32_t r = ((c00 >> 11) * w00 + (c10 >> 11) * w10 + (c01 >> 11) * w01 + (c11 >> 11) * w11) >> 16;
      uint32_t g = (((c00 >> 5) & 63) * w00 + ((c10 >> 5) & 63) * w10 + ((c01 >> 5) & 63) * w01 +
                    ((c11 >> 5) & 63) * w11) >> 16;
      uint32_t b = ((c00 & 31) * w00 + (c10 & 31) * w10 + (c01 & 31) * w01 + (c11 & 31) * w11) >> 16;
      col = (uint16_t)((r << 11) | (g << 5) | b);
      ta = im.alpha ? (uint16_t)((LM_RB(im.alpha + i00) * w00 + LM_RB(im.alpha + i10) * w10 +
                                  LM_RB(im.alpha + i01) * w01 + LM_RB(im.alpha + i11) * w11) >> 16)
                    : 255;
    } else {
      int xi = (int)floorf(u), yi = (int)floorf(v);
      xi = xi < 0 ? 0 : (xi >= w ? w - 1 : xi);
      yi = yi < 0 ? 0 : (yi >= h ? h - 1 : yi);
      uint32_t i = (uint32_t)yi * w + xi;
      col = rdU16(im.rgb + 2 * i);
      ta = im.alpha ? LM_RB(im.alpha + i) : 255;
    }
    uint16_t a8 = (c * ia) >> 8;
    a8 = (a8 * (ta + 1)) >> 8;
    uint8_t a5 = (uint8_t)((a8 + 4) >> 3);
    if (a5 >= 32) row[x] = col;
    else if (a5) row[x] = blend565(row[x], col, a5);
  }
}

bool LottieMCU::renderFrame(float frame) {
  if (_status != OK || !_flush) return false;
  _stats = Stats();
  evalNodes(frame);
  buildGeometry(frame);
  const uint16_t bh = _cfg.bandHeight;
  for (uint16_t by = 0; by < _h; by += bh) {
    uint16_t h = (_h - by < bh) ? (_h - by) : bh;
    uint32_t n = (uint32_t)_w * h;
    for (uint32_t i = 0; i < n; i++) _band[i] = _bg;
    for (uint16_t i = 0; i < _nItemsUsed; i++) {
      const Item &it = _items[i];
      if (it.y1 < by || it.y0 >= by + h) continue;
      for (uint16_t r = 0; r < h; r++) rasterRow(it, by + r, _band + (uint32_t)r * _w);
    }
    if (_swap)
      for (uint32_t i = 0; i < n; i++) _band[i] = (_band[i] >> 8) | (_band[i] << 8);
    _flush(0, by, _w, h, _band, _user);
  }
  return true;
}

bool LottieMCU::update(uint32_t nowMs) {
  if (_status != OK) return false;
  if (!_started) { _startMs = nowMs; _started = true; }
  float len = _op - _ip;
  if (len <= 0 || _fr <= 0) return renderFrame(_ip);
  float f = (float)(nowMs - _startMs) * _fr * 0.001f;
  f = fmodf(f, len);
  return renderFrame(_ip + f);
}
