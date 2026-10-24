/*
  LottieMCU: load an animation from LittleFS (or SD) at runtime.

  Convert on the PC, using your display size:
    python tools/lottie2mcu.py my_anim.json --size 240x320 --bin my_anim.lmc
  then upload my_anim.lmc to LittleFS (data/ folder + "Upload Filesystem Image").

  Part of LottieMCU 1.1.0 - https://github.com/jobitjoseph/LottieMCU
  Copyright (c) 2026 Jobit Joseph. MIT License.
*/
#include <LittleFS.h>
#include <TFT_eSPI.h>
#include <LottieMCU.h>

const bool USE_PSRAM     = true;  // ESP32: animation file and LottieMCU's tables in PSRAM if present
const bool SMOOTH_IMAGES = true;  // bilinear image layers (false: faster)

TFT_eSPI tft;
LottieMCU lottie;
uint8_t *animData = nullptr;

void flush(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *pixels, void *) {
  tft.pushImage(x, y, w, h, (uint16_t *)pixels);
}

bool loadAnimation(const char *path) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  size_t n = f.size();
  free(animData);
  animData = (uint8_t *)((USE_PSRAM && psramFound()) ? ps_malloc(n) : malloc(n));
  if (!animData) return false;
  f.read(animData, n);
  f.close();

  LottieMCU::Config cfg;
  cfg.usePSRAM = USE_PSRAM;
  cfg.bilinear = SMOOTH_IMAGES;
  return lottie.begin(animData, tft.width(), tft.height(), cfg);
}

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setSwapBytes(true);
  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  if (!loadAnimation("/my_anim.lmc")) {
    Serial.println("could not load /my_anim.lmc");
    while (true) delay(1000);
  }
  lottie.setFlush(flush);
}

void loop() {
  lottie.update(millis());
}
