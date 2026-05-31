# ESP32-C3 BLE Image Streamer

BLE peripheral that streams a PNG image over notifications after receiving a `lets go` write.

Current embedded image source:

```text
https://img.itch.zone/aW1nLzIyMTk3MzUucG5n/original/iVdKq2.png
```

Device name:

```text
chromium image streamer
```

Service UUID:

```text
0000ffe0-0000-1000-8000-00805f9b34fb
```

Characteristic UUID:

```text
0000ffe1-0000-1000-8000-00805f9b34fb
```

## Protocol

1. Connect with Web Bluetooth.
2. Get service `FFE0` and characteristic `FFE1`.
3. Enable notifications on the characteristic.
4. Write ASCII:

```text
lets go
```

The device responds with notifications:

1. ASCII metadata:

```text
PNG size=<bytes> mtu=<mtu> chunkData=<payloadBytes> chunks=<n>
```

2. Binary chunks. Each notification has an 8-byte little-endian header followed by PNG bytes:

```c
uint16_t sequence;
uint16_t totalChunks;
uint32_t byteOffset;
uint8_t  pngData[];
```

3. ASCII end marker:

```text
DONE
```

The sketch requests ATT MTU `517`; browser/OS/peripheral negotiate the actual value.
