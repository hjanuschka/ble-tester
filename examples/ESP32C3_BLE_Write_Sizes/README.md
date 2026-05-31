# ESP32-C3 BLE Write-Size Probe

Companion peripheral for Chromium issue [40686244](https://issues.chromium.org/issues/40686244) ("20 byte MTU for web-bluetooth on Windows Chrome").

Device name:

```text
chromium write sizes
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

1. Browser connects, enables notifications on FFE1.
2. Browser writes a payload (any size).
3. Device responds with one ASCII notification:

```text
RX seq=<n> len=<bytes> first=0xXX last=0xXX mtu=<peerMTU>
```

This lets the browser independently verify how many bytes actually reached the peripheral, regardless of which write API was used.

## What it tests

The companion page at `https://static.januschka.com/i-40265040/writes.html` sweeps a range of payload sizes through both:

- `characteristic.writeValueWithResponse(...)`
- `characteristic.writeValueWithoutResponse(...)`

and records, per (api, size):

- Whether the JS Promise resolved (browser-side success).
- The length the peripheral reported in its `RX ...` notification (ground truth).

Historical behavior on Windows (issue 40686244):

- `writeValueWithoutResponse()` rejected payloads > 20 bytes.
- `writeValueWithResponse()` accepted payloads > 20 bytes.

With the negotiated-MTU fix in flight (CL 7879985, behind `--enable-features=NewBLEGattSessionHandling,WebBluetooth`), both APIs should accept payloads up to `negotiatedMTU - 3` bytes, and the device should confirm the full payload was received.
