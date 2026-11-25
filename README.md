# RTK Rover

Firmware for a handheld RTK GNSS receiver: a Quectel LC29H and a Seeed XIAO
ESP32-C6, streaming NMEA to a phone over Bluetooth LE, with RTCM3 corrections
coming back the same way.

    pio run -e xiao_esp32c6 -t upload
    bazel test //...
