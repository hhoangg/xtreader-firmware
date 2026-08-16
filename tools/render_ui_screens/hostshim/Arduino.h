#pragma once
#include <cstdint>
#include <initializer_list>
using std::int8_t;
using std::uint16_t;
using std::uint32_t;
using std::uint8_t;
// Minimal host stand-in for Arduino.h, scoped to this tool's include path
// only (never touches the real firmware build). Its one job is to let
// freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h parse on the
// host: BoardConfig.h unconditionally includes <Arduino.h> and references a
// handful of Arduino symbols in `inline` functions (serialTransport(),
// holdPowerRails(), releaseSdRail()) that this tool never calls -- so those
// symbols only need to be *declared*, never defined/linked. The actual data
// this tool reads (BoardConfig::XTEINK_X4's constexpr geometry) has no
// Arduino dependency at all.

class HardwareSerial {};
extern HardwareSerial Serial;
extern HardwareSerial Serial0;

constexpr int OUTPUT = 1;
constexpr int INPUT = 0;
constexpr int HIGH = 1;
constexpr int LOW = 0;

inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return 0; }
