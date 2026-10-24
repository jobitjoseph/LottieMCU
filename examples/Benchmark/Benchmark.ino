/*
  LottieMCU benchmark, no display needed: renders every frame once and prints the
  render time and buffer usage, to check speed and size LottieMCU::Config.

  Part of LottieMCU 1.1.0 - https://github.com/jobitjoseph/LottieMCU
  Copyright (c) 2026 Jobit Joseph. MIT License.
*/
#include <LottieMCU.h>
#include "demo_spinner.h"

const bool USE_PSRAM     = true;  // ESP32: compare true/false to see the PSRAM speed cost
const bool SMOOTH_IMAGES = true;  // bilinear image layers (false: faster)
const uint16_t OUT_W = 240, OUT_H = 240;

LottieMCU lottie;
volatile uint32_t checksum = 0;

void flush(int16_t, int16_t, uint16_t w, uint16_t h, const uint16_t *pixels, void *) {
  checksum += pixels[(uint32_t)w * h / 2];
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  LottieMCU::Config cfg;
  cfg.usePSRAM = USE_PSRAM;
  cfg.bilinear = SMOOTH_IMAGES;
  if (!lottie.begin(demo_spinner, OUT_W, OUT_H, cfg)) {
    Serial.print("begin failed: ");
    Serial.println(lottie.status());
    return;
  }
  lottie.setFlush(flush);

  uint32_t worst = 0, total = 0, frames = 0;
  uint16_t maxPts = 0, maxCont = 0, maxItems = 0;
  bool overflow = false;
  for (float f = lottie.firstFrame(); f < lottie.lastFrame(); f += 1) {
    uint32_t t0 = micros();
    lottie.renderFrame(f);
    uint32_t dt = micros() - t0;
    total += dt;
    frames++;
    if (dt > worst) worst = dt;
    const LottieMCU::Stats &s = lottie.stats();
    if (s.points > maxPts) maxPts = s.points;
    if (s.contours > maxCont) maxCont = s.contours;
    if (s.items > maxItems) maxItems = s.items;
    overflow |= s.overflowPoints || s.overflowCurve || s.overflowVerts || s.overflowCross ||
                s.overflowItems || s.overflowGradients || s.overflowImages;
  }
  Serial.print("RAM bytes: "); Serial.print(lottie.ramUsed());
  Serial.print(" (PSRAM "); Serial.print(lottie.psramUsed()); Serial.println(")");
  Serial.print("frames: "); Serial.println(frames);
  Serial.print("avg ms/frame: "); Serial.println(total / 1000.0f / frames);
  Serial.print("worst ms/frame: "); Serial.println(worst / 1000.0f);
  Serial.print("peak points/contours/items: ");
  Serial.print(maxPts); Serial.print(" / "); Serial.print(maxCont); Serial.print(" / ");
  Serial.println(maxItems);
  Serial.println(overflow ? "OVERFLOW: raise the Config limits" : "no overflow");
}

void loop() {}
