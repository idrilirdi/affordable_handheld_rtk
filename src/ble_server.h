#pragma once

// BLE peripheral: the receiver's NMEA stream over Nordic UART, plus the SIG
// Battery Service. The stream only costs airtime while a client is subscribed.
//
// Corrections written by the phone (RTCM3 frames, ASCII commands) are queued
// and drained by the main loop into the receiver UART.

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "nmea.h"

void bleBegin();

// True when a client is subscribed to the NMEA stream - nothing needs sending
// when this is false.
bool bleRawActive();

// Mirror raw receiver bytes to the Nordic UART stream. No-op when unsubscribed.
void bleSendRaw(const uint8_t *data, size_t len);

// Largest payload bleSendRaw() should be handed in one call.
size_t bleMaxChunk();

// Bytes the phone sent us, for the receiver. Returns 0 when nothing is pending.
size_t bleReadDownlink(uint8_t *buf, size_t max);

// Publish the battery level (0-100, or BATTERY_UNKNOWN) on the SIG Battery
// Service. Cheap to call repeatedly; only notifies on a real change.
void blePublishBattery(uint8_t percent);

// Advertised name - only valid after bleBegin().
const char *bleDeviceName();

// Notifications the stack refused, usually for want of a buffer. Any non-zero
// value means NMEA the app never received.
uint32_t bleNotifyFailures();

// Downlink diagnostics: total bytes the phone has written, and bytes dropped
// because the queue to the receiver was full. Distinguishes "the app never
// sent corrections" from "they were lost on the way".
void bleDownlinkStats(uint32_t *total, uint32_t *dropped);
