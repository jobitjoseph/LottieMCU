/*
  LottieMCU + TFT_eSPI: plays a Lottie animation full screen on any TFT_eSPI display
  (set up your display in TFT_eSPI's User_Setup.h as usual).

  Your own animation:
    python tools/lottie2mcu.py my_anim.json --size 240x320 -o my_anim.h -n my_anim

  Part of LottieMCU 1.1.0 - https://github.com/jobitjoseph/LottieMCU
  Copyright (c) 2026 Jobit Joseph. MIT License.
*/
#include <TFT_eSPI.h>
#include <LottieMCU.h>
#include "demo_spinner.h"

const bool USE_PSRAM     = true;  // ESP32: keep LottieMCU's tables in PSRAM if present
const bool SMOOTH_IMAGES = true;  // bilinear image layers (false: faster)

TFT_eSPI tft;
LottieMCU lottie;

void flush(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *pixels, void *) {
  tft.pushImage(x, y, w, h, (uint16_t *)pixels);
}

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setSwapBytes(true);
  tft.fillScreen(TFT_BLACK);

  LottieMCU::Config cfg;
  cfg.usePSRAM = USE_PSRAM;
  cfg.bilinear = SMOOTH_IMAGES;
  if (!lottie.begin(demo_spinner, tft.width(), tft.height(), cfg)) {
    Serial.printf("LottieMCU begin failed: %d\n", lottie.status());
    while (true) delay(1000);
  }
  lottie.setFlush(flush);
  Serial.printf("RAM %u bytes (%u in PSRAM)\n", (unsigned)lottie.ramUsed(), (unsigned)lottie.psramUsed());
}

void loop() {
  tft.startWrite();
  lottie.update(millis());
  tft.endWrite();
}
