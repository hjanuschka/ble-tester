# ESP32-C3 BLE Image Streamer

BLE peripheral that streams a progressive JPEG image over notifications.

Embedded image source (re-encoded as progressive JPEG, quality 75):

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

## Protocol (pull-mode flow control)

1. Connect with Web Bluetooth.
2. Get service `FFE0` and characteristic `FFE1`.
3. Enable notifications on the characteristic.
4. Write ASCII:

```text
lets go
```

The device replies with one ASCII metadata notification:

```text
IMG type=JPG mime=image/jpeg size=<bytes> mtu=<mtu> chunkData=<payloadBytes> chunks=<n>
```

5. Pull each chunk by writing:

```text
get 0
get 1
...
get <n-1>
```

Each `get <seq>` produces exactly one binary notification with an 8-byte little-endian header followed by JPEG bytes:

```c
uint16_t sequence;
uint16_t totalChunks;
uint32_t byteOffset;
uint8_t  jpegData[];
```

`get <n>` (out of range) returns the ASCII string `DONE\n`.

## Why pull mode

Pushing all chunks back-to-back (even with `delay()`) frequently overruns the host BLE stack's notification queue, especially CoreBluetooth on macOS. In testing, only the final notification would survive a 114-chunk push. With pull mode there is only ever one outstanding request, so nothing is dropped.

## Why progressive JPEG

PNG cannot be progressively rendered unless interlaced (Adam7), and even then browsers usually wait for the full file. Progressive JPEG is decoded in successive scans, so the browser shows a blurry preview after the first ~10-20% of bytes that sharpens with each additional pass.

## ATT MTU

The sketch requests ATT MTU `517`. The actual per-connection negotiated MTU is read with `NimBLEServer::getPeerMTU()` and reported in the metadata frame, which drives the chunk size.
