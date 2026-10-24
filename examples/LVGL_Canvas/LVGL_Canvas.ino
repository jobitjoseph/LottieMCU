/*
  LottieMCU + LVGL 9: plays a Lottie animation inside an LVGL canvas, next to
  normal LVGL widgets. The display is driven by TFT_eSPI.

  Requirements
    - LVGL 9.x (Library Manager: "lvgl") with an lv_conf.h: copy
      lv_conf_template.h next to the lvgl folder as lv_conf.h, change "#if 0" to
      "#if 1", and keep LV_COLOR_DEPTH 16 and LV_USE_CANVAS 1.
    - TFT_eSPI set up for your display (User_Setup.h).

  LVGL needs the canvas image in RAM: ANIM_SIZE x ANIM_SIZE x 2 bytes
  (51 KB at 160 px). On ESP32 with PSRAM it is allocated there.

  Part of LottieMCU - https://github.com/jobitjoseph/LottieMCU
  Copyright (c) 2026 Jobit Joseph. MIT License.
*/
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <LottieMCU.h>
#include "demo_spinner.h"

const uint16_t ANIM_SIZE = 160;     // canvas width and height in pixels
const bool USE_PSRAM     = true;    // ESP32: canvas buffer and LottieMCU tables in PSRAM if present
const uint32_t SCREEN_BG = 0x14141E;

TFT_eSPI tft;
LottieMCU lottie;
lv_obj_t *canvas;
lv_obj_t *status;
uint16_t *animBuf;
float frame = 0;
uint32_t lastMs = 0;

static uint32_t tickCb() { return millis(); }

static void displayFlush(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
  uint32_t w = lv_area_get_width(area), h = lv_area_get_height(area);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)px, w * h, true);
  tft.endWrite();
  lv_display_flush_ready(disp);
}

static void lottieFlush(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *px, void *) {
  memcpy(&animBuf[(uint32_t)y * ANIM_SIZE + x], px, (size_t)w * h * 2);
}

static void animTimer(lv_timer_t *) {
  uint32_t now = millis();
  frame += (now - lastMs) * 0.001f * lottie.frameRate();
  lastMs = now;
  frame = fmodf(frame, lottie.totalFrames());
  lottie.renderFrame(lottie.firstFrame() + frame);
  lv_obj_invalidate(canvas);
  lv_label_set_text_fmt(status, "frame %d / %d", (int)frame, (int)lottie.totalFrames());
}

static void *allocBuffer(size_t bytes) {
#if defined(ARDUINO_ARCH_ESP32)
  if (USE_PSRAM && psramFound()) return ps_malloc(bytes);
#endif
  return malloc(bytes);
}

void setup() {
  Serial.begin(115200);
  tft.init();

  lv_init();
  lv_tick_set_cb(tickCb);
  uint32_t drawBufBytes = tft.width() * tft.height() / 10 * 2;
  lv_display_t *disp = lv_display_create(tft.width(), tft.height());
  lv_display_set_flush_cb(disp, displayFlush);
  lv_display_set_buffers(disp, malloc(drawBufBytes), nullptr, drawBufBytes,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  animBuf = (uint16_t *)allocBuffer((size_t)ANIM_SIZE * ANIM_SIZE * 2);
  LottieMCU::Config cfg;
  cfg.usePSRAM = USE_PSRAM;
  if (!animBuf || !lottie.begin(demo_spinner, ANIM_SIZE, ANIM_SIZE, cfg)) {
    Serial.println("Not enough memory for the animation");
    while (true) delay(1000);
  }
  lottie.setFlush(lottieFlush);
  lottie.setBackground(lv_color_to_u16(lv_color_hex(SCREEN_BG)));

  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(SCREEN_BG), 0);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "LottieMCU + LVGL");
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

  canvas = lv_canvas_create(scr);
  lv_canvas_set_buffer(canvas, animBuf, ANIM_SIZE, ANIM_SIZE, LV_COLOR_FORMAT_RGB565);
  lv_obj_center(canvas);

  status = lv_label_create(scr);
  lv_obj_set_style_text_color(status, lv_color_hex(0x9AA0B4), 0);
  lv_obj_align(status, LV_ALIGN_BOTTOM_MID, 0, -16);

  lastMs = millis();
  lv_timer_create(animTimer, 33, nullptr);
}

void loop() {
  lv_timer_handler();
  delay(5);
}
