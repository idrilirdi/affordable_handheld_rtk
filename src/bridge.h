#pragma once

#include <stdbool.h>

#include <Arduino.h>

// A transparent path from the USB console to the GNSS module's UART, so the
// receiver's own firmware can be updated through the ESP32.
//
// The production board exposes only RX, TX, GND and power; there is no USB on
// it and no reset pin. Sealed in an enclosure, the ESP32 is the only thing that
// can reach it. See BRIDGE_* in config.h for why the baud is pinned.
//
// Entering is deliberate and leaving is not possible: while bridging, every
// byte in both directions is firmware image, so no escape sequence could be
// distinguished from data. Reset the board to get out.

void bridgeBegin(HardwareSerial &gnss);

// Watches the USB console for BRIDGE_COMMAND while the rover is running
// normally. Returns true once the bridge has taken over, after which the
// caller must stop touching the GNSS UART and call bridgeTask() instead.
bool bridgePoll();

bool bridgeActive();

// Pumps both directions. Never returns to normal operation.
void bridgeTask();
