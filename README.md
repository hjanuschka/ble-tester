# BLE Tester

ESP32-C3 BLE peripheral for testing Chromium/Web Bluetooth ATT MTU negotiation and write payload sizes.

The device advertises as:

```text
chromium ble tester
```

Use with the sampler page:

```text
https://static.januschka.com/i-40265040/
```

## UUIDs

Service UUID:

```text
0000ffe0-0000-1000-8000-00805f9b34fb
```

Characteristic UUID:

```text
0000ffe1-0000-1000-8000-00805f9b34fb
```

## Hardware

Tested with an ESP32-C3 Mini development board based on ESP32-C3FN4.

## Arduino IDE setup

Install:

- Espressif ESP32 Arduino core
- NimBLE-Arduino library

Recommended board/settings:

```text
Board: ESP32C3 Dev Module
USB CDC On Boot: Enabled
Flash Size: 4MB
CPU Frequency: 160MHz
Upload Speed: 921600
```

Serial monitor baud:

```text
115200
```

## Behavior

- Advertises a writable/notifiable BLE characteristic.
- Requests ATT MTU 247.
- Accepts `writeValueWithoutResponse()` and `writeValueWithResponse()`.
- Logs write payload length over Serial.
- Updates characteristic value with a status/ACK string.
- Toggles common ESP32-C3 LED pins GPIO2/GPIO8/GPIO10 as a heartbeat.

## Expected Web Bluetooth test result

With a browser/build that supports negotiated MTU and large writes, expected results are:

```text
negotiated MTU = 247
max ATT payload ~= 244 bytes
writeValueWithoutResponse passes above 20 bytes
```
