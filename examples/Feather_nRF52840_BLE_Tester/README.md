# Feather nRF52840 BLE Tester

Adafruit Feather nRF52840 Sense version of the Chromium/Web Bluetooth tester firmware.

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

## Board

Arduino IDE board:

```text
Adafruit Feather nRF52840 Sense
```

FQBN:

```text
adafruit:nrf52:feather52840sense
```

## Notes

The Adafruit nRF52 core has an effective ATT MTU max of 247 in this setup. The firmware reports the effective peer MTU capped at 247 so it matches Chromium `getNegotiatedMTU()` and the `MTU - 3 = 244` write-command payload limit.

Commands:

```text
mtu
lets go
get <seq>
status
help
```

Arbitrary writes return an RX ACK notification with the actual received length.
