#pragma once

#include <Memory.h>

#include <cstddef>
#include <cstdint>
#include <memory>

// Assembles arbitrary-sized byte chunks (as handed out by
// HttpDownloader::fetchUrl's DataCallback, in ~1 KB pieces, or by a small
// fixed-size read off the SD card) into '\n'-terminated lines, one callback
// per complete line.
//
// Why this is safe across an arbitrary chunk boundary, including one that
// falls mid-line or mid-UTF-8-sequence: this only ever looks for the single
// byte 0x0A ('\n'). UTF-8 guarantees 0x0A cannot appear as any byte of a
// multi-byte sequence -- continuation bytes are 10xxxxxx (0x80-0xBF) and
// lead bytes are 11xxxxxx (0xC0-0xFF), never 0x0A -- so splitting on raw
// bytes wherever a chunk happens to end can never mistake the middle of a
// Vietnamese diacritic's encoding for a line break, and never needs to
// know where a multi-byte sequence started. The buffered remainder simply
// carries whatever partial bytes (of a line, and possibly of a UTF-8
// sequence within it) arrived so far into the next feed() call; the line
// callback only ever sees whole, complete lines.
//
// Fixed-capacity buffer, allocated once (checked -- see Memory.h) at
// construction: an unbounded line (a hostile or broken server that never
// sends '\n') is capped rather than growing the buffer without limit; feed()
// fails once a single line would exceed that cap. This is a different bound
// than the caller's own page-count cap on pagination -- this one guards a
// single line, that one guards the whole sync.
class LineChunker {
 public:
  // Returns false to abort (line callback rejected the line, e.g. malformed).
  // Plain function pointer + context, not std::function: avoids the
  // per-signature binary cost and heap-allocating closure (see CLAUDE.md's
  // "Template and std::function Bloat").
  using LineCallback = bool (*)(void* ctx, const char* line, size_t len);

  // capacity bounds a single line's length, trailing '\n'/'\r' excluded.
  // `ctx` is passed back to onLine unchanged; the caller owns it and must
  // keep it alive for as long as this LineChunker is fed.
  LineChunker(LineCallback onLine, void* ctx, size_t capacity);

  // Feeds the next chunk. Returns false once and for all if allocation
  // failed at construction, a line exceeded capacity, or onLine returned
  // false -- callers should stop feeding and check hasError()/overflowed().
  bool feed(const uint8_t* data, size_t len);

  bool hasError() const { return error_; }
  bool overflowed() const { return overflow_; }
  // True if a partial (no trailing '\n' yet) line is currently buffered.
  bool hasPendingLine() const { return lineLen_ > 0; }

 private:
  LineCallback onLine_;
  void* ctx_;
  std::unique_ptr<char[]> buf_;
  size_t capacity_;
  size_t lineLen_ = 0;
  bool overflow_ = false;
  bool error_ = false;
};
