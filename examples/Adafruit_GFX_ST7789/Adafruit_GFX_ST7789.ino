/*
  LottieMCU + Adafruit_GFX (ST7789 240x240 shown; any Adafruit_SPITFT display works).
  Runs on 32 KB-RAM boards too (SAMD21, UNO R4), which get smaller default limits.

  Part of LottieMCU 1.1.0 - https://github.com/jobitjoseph/LottieMCU
  Copyright (c) 2026 Jobit Joseph. MIT License.
*/
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <LottieMCU.h>
#include "demo_spinner.h"

#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST   8

const bool USE_PSRAM     = true;  // ESP32: keep LottieMCU's tables in PSRAM if present
const bool SMOOTH_IMAGES = true;  // bilinear image layers (false: faster)

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
LottieMCU lottie;

void flush(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *pixels, void *) {
  tft.drawRGBBitmap(x, y, (uint16_t *)pixels, w, h);
}

void setup() {
  Serial.begin(115200);
  tft.init(240, 240);
  tft.setSPISpeed(40000000);
  tft.fillScreen(ST77XX_BLACK);

  LottieMCU::Config cfg;
  cfg.usePSRAM = USE_PSRAM;
  cfg.bilinear = SMOOTH_IMAGES;
  if (!lottie.begin(demo_spinner, tft.width(), tft.height(), cfg)) {
    Serial.print("LottieMCU begin failed: ");
    Serial.println(lottie.status());
    while (true) delay(1000);
  }
  lottie.setFlush(flush);
}

void loop() {
  lottie.update(millis());
}
