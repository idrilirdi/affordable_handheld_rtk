#pragma once

// One-shot Quectel LC29H(DA) setup, run before the BLE link comes up.
//
// Compiles to an empty call when GNSS_INIT_LC29H is 0.

#include <Arduino.h>

void gnssInit(HardwareSerial &ser);

// True once the module has acknowledged a command - proof that what we send
// reaches it. NMEA arriving proves only the other direction: with the wire to
// the module's RX loose, positions keep coming while every setup command and
// every correction is silently lost, which looks like a correction problem.
bool gnssAnswering();

// From loop(): while the module has not answered, probe it now and then and
// run the whole setup again the moment it does, so reseating a loose wire
// fixes everything without a restart. Does nothing once it has answered.
void gnssInitTask(HardwareSerial &ser);

// Raw observations and ephemerides as RTCM3 on the same UART, for recording
// a point that can be post-processed. Off at boot; see points.h.
void gnssSetRawOutput(HardwareSerial &ser, bool on);
