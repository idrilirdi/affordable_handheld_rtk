#include "uplink_split.h"

static uint32_t crc24q(const uint8_t *p, size_t n) {
  uint32_t crc = 0;
  for (size_t i = 0; i < n; ++i) {
    crc ^= (uint32_t)p[i] << 16;
    for (int b = 0; b < 8; ++b) {
      crc <<= 1;
      if (crc & 0x1000000) crc ^= 0x1864CFB;
    }
  }
  return crc & 0xFFFFFF;
}

size_t UplinkSplit::feed(const uint8_t *in, size_t len, uint8_t *text,
                         void (*frame)(const uint8_t *data, size_t len)) {
  size_t tn = 0;
  for (size_t i = 0; i < len; ++i) {
    const uint8_t c = in[i];
    if (!have_) {
      if (c == 0xD3) {
        buf_[have_++] = c;
        need_ = 0;
      } else {
        text[tn++] = c;
      }
      continue;
    }

    buf_[have_++] = c;
    if (have_ == 3) {
      // Six reserved bits, which are zero in every frame. Anything else was a
      // stray 0xD3, and the receiver never sends one in text - drop it.
      if (buf_[1] & 0xFC) {
        bad_++;
        have_ = 0;
        continue;
      }
      need_ = 3 + (((size_t)(buf_[1] & 0x03) << 8) | buf_[2]) + 3;
    }
    if (need_ && have_ == need_) {
      const size_t body = need_ - 3;
      const uint32_t want = ((uint32_t)buf_[body] << 16) | ((uint32_t)buf_[body + 1] << 8) |
                            buf_[body + 2];
      if (crc24q(buf_, body) == want) {
        ok_++;
        if (frame) frame(buf_, need_);
      } else {
        bad_++;
      }
      have_ = 0;
    }
  }
  return tn;
}
