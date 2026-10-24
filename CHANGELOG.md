# Changelog

All notable changes to LottieMCU are documented here.
This project follows [Semantic Versioning](https://semver.org/).

## [1.1.0] - 2026-10-24

### Added
- Image layers: embedded (`data:`) and external PNG/JPEG images, pre-scaled to the display
  size by the converter (`--size WxH`, `--image-scale`), drawn with rotation, scaling,
  transparency, masks and mattes. New `Config::maxImages` and `Config::bilinear`.
- PSRAM support on ESP32: `Config::usePSRAM` (default on) places the large working tables in
  PSRAM when present; `psramUsed()` reports how much.
- `Stats::overflowGradients` and `Stats::overflowImages`.
- `LOTTIEMCU_VERSION` macros; `lottie2mcu.py --version`.

### Changed
- When more gradients are visible than `maxGradients`, the extra ones now fall back to the
  gradient's average colour (was: its middle stop).

### Compatibility
- Binary format unchanged for vector animations: files converted with 1.0.0 load as before.
  Re-convert animations that contain image layers (1.0.0 dropped them).

## [1.0.0] - 2026-10-24

### Added
- First release: vector Lottie playback (shapes, strokes, gradients, trim paths, repeaters,
  masks, track mattes, precomps, parenting, skew, eased/hold keyframes) with anti-aliased strip
  rendering to RGB565, the `lottie2mcu.py` converter and examples for TFT_eSPI, Adafruit_GFX,
  LittleFS loading and benchmarking.
