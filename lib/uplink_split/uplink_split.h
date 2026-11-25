#pragma once

// Separates the receiver's output into its NMEA text and its RTCM3 frames.
//
// The receiver only sends RTCM while a point is being recorded (see points.h),
// and then on the same UART as the NMEA, interleaved. Neither consumer can
// take the mix: a raw-NMEA app over BLE would be fed binary, and the point log
// needs whole frames. NMEA is plain ASCII, so a 0xD3 byte can only ever be the
// start of a frame.
//
// Pure, so it can be tested on a host.

#include <stddef.h>
#include <stdint.h>

class UplinkSplit {
 public:
  // Splits `in`. The text goes to `text` (room for `len` bytes is enough), and
  // its length is returned; each complete frame with a valid CRC is handed to
  // `frame`. A frame may span calls. Frames that fail their CRC are dropped.
  size_t feed(const uint8_t *in, size_t len, uint8_t *text,
              void (*frame)(const uint8_t *data, size_t len));

  uint32_t framesOk() const { return ok_; }
  uint32_t framesBad() const { return bad_; }

 private:
  // A frame is at most 3 + 1023 + 3 bytes.
  uint8_t buf_[1029];
  size_t have_ = 0;  // bytes of the current frame so far; 0 = in text
  size_t need_ = 0;  // its full length, once the header is in
  uint32_t ok_ = 0, bad_ = 0;
};
