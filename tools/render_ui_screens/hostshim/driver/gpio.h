#pragma once
// Minimal host stand-in for ESP-IDF's driver/gpio.h -- see Arduino.h shim in
// this same directory for why this exists and why declarations (not
// definitions) are enough.

using gpio_num_t = int;
inline void gpio_hold_dis(gpio_num_t) {}
