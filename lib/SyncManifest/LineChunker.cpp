#include "LineChunker.h"

LineChunker::LineChunker(LineCallback onLine, void* ctx, size_t capacity)
    : onLine_(onLine), ctx_(ctx), buf_(makeUniqueNoThrow<char[]>(capacity)), capacity_(capacity) {
  if (!buf_) {
    // Every allocation checked (this build is -fno-exceptions, so `new`
    // returns nullptr rather than throwing): feed() below refuses to do
    // anything once this is true, so a construction-time OOM fails the
    // whole sync cleanly instead of writing into an unallocated buffer.
    error_ = true;
    capacity_ = 0;
  }
}

bool LineChunker::feed(const uint8_t* data, size_t len) {
  if (error_) return false;

  for (size_t i = 0; i < len; ++i) {
    const char c = static_cast<char>(data[i]);
    if (c != '\n') {
      if (lineLen_ < capacity_) {
        buf_[lineLen_++] = c;
      } else {
        // Keep consuming silently until the newline that ends this
        // oversized line, then fail once -- rather than growing the buffer
        // (there is no bound on how large a hostile/broken response could
        // make a single "line").
        overflow_ = true;
      }
      continue;
    }

    if (overflow_) {
      error_ = true;
      return false;
    }

    // Strip a trailing '\r' (defensive against a CRLF body; the API
    // documents plain '\n').
    size_t n = lineLen_;
    if (n > 0 && buf_[n - 1] == '\r') --n;

    const bool ok = onLine_(ctx_, buf_.get(), n);
    lineLen_ = 0;
    if (!ok) {
      error_ = true;
      return false;
    }
  }
  return true;
}
