# ESP32-C3 All BLE Tester

All-in-one Chromium/Web Bluetooth test firmware.

Advertises as:

```text
dino tester
```

Service UUID:

```text
0000ffe0-0000-1000-8000-00805f9b34fb
```

Characteristic UUID:

```text
0000ffe1-0000-1000-8000-00805f9b34fb
```

## Modes

### MTU / generic write tester

Write arbitrary payloads to the characteristic with either `writeValueWithResponse()` or `writeValueWithoutResponse()`.

If notifications are enabled, the device replies:

```text
RX seq=<n> len=<bytes> first=0xXX last=0xXX mtu=<peerMTU>
```

### getNegotiatedMTU probe

Write the ASCII command:

```text
mtu
```

The device replies with one notification:

```text
MTU peerMTU=<peerMTU>
```

Used by the `mtu.html` conformance test on the sampler page to cross-check the browser-side `getNegotiatedMTU()` return value against the peripheral-side `NimBLEServer::getPeerMTU()`.

### Image streamer

1. Enable notifications.
2. Write:

```text
lets go
```

3. Device replies with metadata:

```text
IMG type=JPG mime=image/jpeg size=<bytes> mtu=<peerMTU> chunkData=<bytes> chunks=<n>
```

4. Pull chunks one at a time:

```text
get 0
get 1
get 2
...
```

Each binary notification has:

```c
uint16_t sequence;
uint16_t totalChunks;
uint32_t byteOffset;
uint8_t  jpgData[];
```

The sketch requests ATT MTU 517 but uses `NimBLEServer::getPeerMTU()` for the actual negotiated per-connection MTU.
