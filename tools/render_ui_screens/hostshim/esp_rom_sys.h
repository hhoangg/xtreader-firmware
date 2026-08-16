#pragma once
// Minimal host stand-in for ESP-IDF's esp_rom_sys.h -- see Arduino.h shim in
// this same directory for why this exists and why a declaration (not a
// definition) is enough.

inline void esp_rom_printf(const char*, ...) {}
