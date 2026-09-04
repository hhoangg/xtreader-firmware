#pragma once

// Minimal host stub for the Arduino Print sink. The parsers under test only
// ever use the two write() overloads; nothing else of Print's surface is
// touched, so nothing else is modelled here.

#include <cstddef>
#include <cstdint>

class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t c) = 0;
  virtual size_t write(const uint8_t* buffer, size_t size) = 0;
};
