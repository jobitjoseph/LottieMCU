/*
 * LottieMCU - minimal-footprint Lottie animation player for microcontrollers
 *
 * Lottie JSON is converted on the PC (tools/lottie2mcu.py) into a compact
 * binary that is played straight from flash. Frames are rasterised with
 * anti-aliasing into a small strip buffer (a few display lines at a time),
 * so no full framebuffer is needed. Output is RGB565 through a callback, so
 * any display library works (TFT_eSPI, LovyanGFX, Adafruit_GFX, ...).
 *
 * Version 1.1.0
 * Copyright (c) 2026 Jobit Joseph <jobitjoseph1@gmail.com>
 * https://github.com/jobitjoseph/LottieMCU
 *
 * SPDX-License-Identifier: MIT
 * Released under the MIT License - see the LICENSE file for details.
 */
#pragma once
#include <Arduino.h>

#define LOTTIEMCU_VERSION "1.1.0"
#define LOTTIEMCU_VERSION_MAJOR 1
#define LOTTIEMCU_VERSION_MINOR 1
#define LOTTIEMCU_VERSION_PATCH 0

// Working-memory limits. Everything is allocated once in begin(); nothing is
// allocated per frame. Defaults suit ESP32 / RP2040 / STM32F4 / SAMD51 class
// boards (~55 KB at 240 px width); 32 KB-RAM boards get a ~14 KB profile.
// LottieMCU::ramNeeded() reports the exact figure for a config and width.
// If stats() reports an overflow, raise the matching limit.
#if defined(__AVR__) || defined(__SAMD21G18A__) || defined(__SAMD21E18A__) || \
    defined(__SAMD21J18A__) || defined(ARDUINO_ARCH_RENESAS) || defined(LOTTIEMCU_SMALL)
#define LOTTIEMCU_SMALL_DEFAULTS 1
#endif
struct LottieMCUConfig {
#ifdef LOTTIEMCU_SMALL_DEFAULTS
  uint16_t bandHeight    = 4;
  uint16_t maxPoints     = 1024;
  uint16_t maxContours   = 256;
  uint16_t maxItems      = 48;
  uint16_t maxCurvePoints= 128;
  uint16_t maxPathVerts  = 32;
  uint16_t maxCrossings  = 64;
  uint8_t  maxGradients  = 2;
  uint8_t  maxImages     = 2;
#else
  uint16_t bandHeight    = 16;    // display lines rendered per flush
  uint16_t maxPoints     = 4096;  // polygon points per frame (after flattening/stroking)
  uint16_t maxContours   = 1024;  // polygons per frame
  uint16_t maxItems      = 192;   // fills/strokes per frame
  uint16_t maxCurvePoints= 512;   // points of one flattened sub-path
  uint16_t maxPathVerts  = 256;   // bezier vertices of one path (converter prints the need)
  uint16_t maxCrossings  = 128;   // edge crossings on one scanline of one item
  uint8_t  maxGradients  = 8;     // gradient fills/strokes per frame
  uint8_t  maxImages     = 8;     // image layers drawn per frame
#endif
  uint8_t  subSamples    = 4;     // vertical anti-aliasing samples: 1, 2, 4 or 8
  float    tolerance     = 0.3f;  // curve flattening tolerance in pixels
  bool     bilinear      = true;  // smooth image sampling (false: nearest pixel, faster)
  // ESP32 with PSRAM: put the large working tables in PSRAM to save internal
  // RAM (the strip buffer and scanline buffers always stay in fast internal RAM).
  // Falls back to internal RAM if no PSRAM is present. Ignored on other boards.
  bool     usePSRAM      = true;
};

class LottieMCU {
public:
  typedef LottieMCUConfig Config;

  // Called once per strip: pixels holds w*h RGB565 values, row-major.
  typedef void (*FlushFn)(int16_t x, int16_t y, uint16_t w, uint16_t h,
                          const uint16_t *pixels, void *user);

  enum Status : uint8_t { OK = 0, ERR_FORMAT, ERR_NOMEM, NOT_STARTED };

  // Heap bytes begin() will allocate for this config/width, excluding the
  // per-animation tables (nodes*36 + paths*4 + trims*12 + clips*20 bytes).
  static uint32_t ramNeeded(const Config &cfg, uint16_t width);
  // Heap bytes allocated by the last successful begin(): total, and the part in PSRAM.
  uint32_t ramUsed() const { return _ramUsed; }
  uint32_t psramUsed() const { return _psramUsed; }

  LottieMCU() {}
  ~LottieMCU() { end(); }

  // data: output of lottie2mcu.py (PROGMEM/flash array or a RAM buffer).
  // width/height: output size; the animation is scaled to fit and centred.
  bool begin(const uint8_t *data, uint16_t width, uint16_t height,
             const Config &cfg = Config());
  void end();

  void setFlush(FlushFn fn, void *user = nullptr) { _flush = fn; _user = user; }
  void setBackground(uint16_t rgb565) { _bg = rgb565; }
  void setSwapBytes(bool swap) { _swap = swap; }       // for displays wanting big-endian 565
  // Optional custom placement: scale and top-left offset in output pixels.
  void setTransform(float scale, float offsetX, float offsetY);

  // Render one frame (Lottie frame number, fractional allowed) and flush it.
  bool renderFrame(float frame);
  // Render the frame for the given time in ms since start(), looping.
  bool update(uint32_t nowMs);
  void restart() { _startMs = 0; _started = false; }

  // Animation info
  uint16_t animWidth()  const { return _cw; }
  uint16_t animHeight() const { return _ch; }
  float frameRate()     const { return _fr; }
  float firstFrame()    const { return _ip; }
  float lastFrame()     const { return _op; }
  float totalFrames()   const { return _op - _ip; }
  uint32_t durationMs() const { return _fr > 0 ? (uint32_t)((_op - _ip) * 1000.0f / _fr) : 0; }
  Status status()       const { return _status; }

  // Per-frame diagnostics: if overflow is set, raise the matching Config limit.
  struct Stats {
    uint16_t points, contours, items;
    bool overflowPoints, overflowCurve, overflowVerts, overflowCross, overflowItems,
         overflowGradients, overflowImages;
  };
  const Stats &stats() const { return _stats; }

private:
  struct Node    { float m[6]; float op; float tOff; uint8_t vis; };
  struct Contour { uint16_t start, n; int16_t ymin, ymax; };
  // a filled/stroked shape, or a clip (mask / matte) polygon set
  struct Item    { uint16_t c0, c1; uint16_t color; uint8_t alpha, rule;
                   int16_t x0, y0, x1, y1; int16_t clip; int8_t grad, img; };
  struct Grad    { uint16_t col[64]; uint8_t a[64]; float sx, sy, ux, uy; uint8_t radial; };
  struct Cross   { int16_t x; int8_t d; };
  // an image placed this frame: texture + screen->texel mapping
  struct ImgInst { const uint8_t *rgb, *alpha; uint16_t w, h; float ua, ub, uc, va, vb, vc; };

  const uint8_t *evalProp(const uint8_t *p, float t, float *out, uint16_t cap,
                          const float *def, uint16_t *dimsOut = nullptr);
  void evalNodes(float frame);
  void buildGeometry(float frame);
  void buildPath(uint16_t idx, float frame, bool stroke, float halfW,
                 uint8_t cap, uint8_t join, float miter);
  int  flatten(const float *v, int nv, bool closed, const float *m, float *out);
  void emit(const float *pts, int n, bool closed, bool stroke, float halfW,
            uint8_t cap, uint8_t join, float miter);
  void strokePolyline(const float *pts, int n, bool closed, float h,
                      uint8_t cap, uint8_t join, float miter);
  void addPoly(const float *xy, int n, bool orient);
  void addCircle(float cx, float cy, float r);
  void rasterRow(const Item &it, int y, uint16_t *row);
  void coverRow(const Item &it, int y, uint16_t *cov, int &minx, int &maxx);
  void addSpan(uint16_t *cov, int32_t xa, int32_t xb, int &minx, int &maxx);
  bool clipEmpty(int16_t clip) const;
  void rgbCol(const float *c, Item &it);
  void buildGradient(Grad &g, const float *v, uint8_t nc, uint8_t na);
  void freeAll();
  void *alloc(uint32_t bytes, bool large);
  bool buildImage(uint16_t idx, const Node &N, Item &it);
  void blendImageRow(const Item &it, int y, int minx, int maxx, uint16_t *row);

  Config _cfg;
  const uint8_t *_data = nullptr;
  uint16_t _w = 0, _h = 0, _cw = 0, _ch = 0;
  float _fr = 30, _ip = 0, _op = 0;
  uint16_t _nNodes = 0, _nPaths = 0, _nTrims = 0, _nItems = 0, _nClips = 0, _nImages = 0;
  uint32_t _offNodes = 0, _offTrims = 0, _offClips = 0, _offItems = 0;
  uint32_t *_imgOffs = nullptr;
  ImgInst *_imgs = nullptr; uint8_t _nImgs = 0;
  Item *_clips = nullptr;
  int8_t _orient = 0;        // forced winding for clip polygons (0 = keep)
  uint32_t *_pathOffs = nullptr;
  float _root[6] = {1, 0, 0, 1, 0, 0};

  Node *_nodes = nullptr;
  float *_trimVals = nullptr;
  int16_t *_pts = nullptr;  uint16_t _nPts = 0;
  Contour *_contours = nullptr; uint16_t _nContours = 0;
  Item *_items = nullptr; uint16_t _nItemsUsed = 0;
  float *_curve = nullptr;   // 2 halves of maxCurvePoints xy pairs
  float *_verts = nullptr;   // maxPathVerts * 6
  Cross *_cross = nullptr;
  uint16_t *_band = nullptr;
  uint16_t *_cov = nullptr;
  uint16_t *_cov2 = nullptr; // clip coverage
  Grad *_grads = nullptr; uint8_t _nGrads = 0;
  Item *_cur = nullptr;      // item being built

  FlushFn _flush = nullptr; void *_user = nullptr;
  uint16_t _bg = 0; bool _swap = false;
  uint32_t _startMs = 0; bool _started = false;
  Status _status = NOT_STARTED;
  Stats _stats = {};
  uint32_t _ramUsed = 0, _psramUsed = 0;
};
