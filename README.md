# Sensor Fusion Pipeline

Real-time aggregation of four synchronized temperature sensor streams
(96 kHz, 71 kHz, 69.9 kHz, 23 kHz) into a temporally sorted stream, with two
sliding-window fusion functions computed over the N most recent readings.

Written in C11 with POSIX threads. No third-party dependencies.

## Build

```sh
make
```
