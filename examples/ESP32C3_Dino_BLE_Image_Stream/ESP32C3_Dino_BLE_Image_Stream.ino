/*
  ESP32-C3 BLE image streamer (pull-mode flow control)

  Advertises as: chromium image streamer

  Image format:
    Progressive JPEG (re-encoded from the original PNG). Progressive JPEG is
    decoded by browsers in successive passes as bytes arrive, so the page
    can show a blurry preview that sharpens over the stream -- unlike PNG,
    which only renders once the full file (including IEND) has been received.

  Protocol:
    1. Web Bluetooth client connects to service FFE0 / characteristic FFE1.
    2. Client enables notifications on FFE1.
    3. Client writes ASCII: "lets go"
       -> device replies with metadata notification:
          "IMG type=JPG mime=image/jpeg size=<bytes> mtu=<mtu> chunkData=<payloadBytes> chunks=<n>\n"
    4. Client requests chunks one at a time:
          "get <seq>"   (e.g. "get 0", "get 1", ... "get N-1")
       -> device replies with one binary notification per request, 8 byte LE header:
          uint16_t sequence
          uint16_t totalChunks
          uint32_t byteOffset
          uint8_t  pngData[]
       Sending "get N" (out of range) returns the ASCII string "DONE\n".

  Why pull mode?
    Pushing all chunks back-to-back (even with delay()) frequently overruns
    the host BLE stack's notification queue (especially CoreBluetooth on
    macOS). The client only ever has 1 outstanding request at a time, so
    nothing gets dropped.

  The sketch requests ATT MTU 517 and reports the actual per-connection MTU
  via NimBLEServer::getPeerMTU().
*/

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "dino_jpg.h"

// Embedded image: progressive JPEG -- the browser can decode and paint
// successive scans as bytes arrive, giving a real progressive reveal.
#define IMG_DATA DINO_JPG
#define IMG_LEN  DINO_JPG_LEN
#define IMG_MIME "image/jpeg"
#define IMG_TAG  "JPG"

static const char* DEVICE_NAME  = "chromium image streamer";
static const char* SERVICE_UUID = "0000ffe0-0000-1000-8000-00805f9b34fb";
static const char* CHAR_UUID    = "0000ffe1-0000-1000-8000-00805f9b34fb";

static NimBLEServer* server = nullptr;
static NimBLECharacteristic* streamChar = nullptr;
static bool connected = false;
static bool notifyEnabled = false;

static const uint8_t LED_PINS[] = {2, 8, 10};
static bool ledState = false;

static void advertise() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(DEVICE_NAME);
  adv->addServiceUUID(SERVICE_UUID);
  adv->enableScanResponse(true);
  adv->start();
  Serial.println("Advertising as chromium image streamer");
}

static void notifyBytes(const uint8_t* data, size_t len) {
  if (!streamChar || !notifyEnabled || !connected) return;
  streamChar->setValue(data, len);
  streamChar->notify();
}

static void notifyText(const char* text) {
  notifyBytes((const uint8_t*)text, strlen(text));
}

static void put16le(uint8_t* p, uint16_t v) {
  p[0] = v & 0xff;
  p[1] = (v >> 8) & 0xff;
}

static void put32le(uint8_t* p, uint32_t v) {
  p[0] = v & 0xff;
  p[1] = (v >> 8) & 0xff;
  p[2] = (v >> 16) & 0xff;
  p[3] = (v >> 24) & 0xff;
}

// Returns the per-connection negotiated ATT MTU, falling back to the default 23.
// NimBLEDevice::getMTU() returns the locally REQUESTED MTU and is misleading.
static uint16_t currentPeerMTU() {
  if (!server || server->getConnectedCount() == 0) return 23;
  uint16_t handle = server->getPeerInfo(0).getConnHandle();
  uint16_t mtu = server->getPeerMTU(handle);
  return mtu < 23 ? 23 : mtu;
}

// Web Bluetooth on Chromium effectively caps notification payloads around
// the iOS/Android-default 244 bytes (ATT MTU 247) regardless of what the
// link negotiates. Larger notifications get silently dropped, so we cap
// the per-notification ATT value at 244 bytes here.
static constexpr uint16_t WEB_BLUETOOTH_NOTIFY_CAP = 244;

static uint16_t chunkDataSize() {
  uint16_t mtu = currentPeerMTU();
  const uint16_t notifyPayloadMax =
      min<uint16_t>(WEB_BLUETOOTH_NOTIFY_CAP, (mtu > 3) ? (uint16_t)(mtu - 3) : (uint16_t)20);
  const uint16_t headerLen = 8;
  return (notifyPayloadMax > headerLen) ? (uint16_t)(notifyPayloadMax - headerLen) : (uint16_t)12;
}

static uint16_t totalChunkCount() {
  const uint16_t dataPerChunk = chunkDataSize();
  return (IMG_LEN + dataPerChunk - 1) / dataPerChunk;
}

static void sendMetadata() {
  const uint16_t mtu = currentPeerMTU();
  const uint16_t dataPerChunk = chunkDataSize();
  const uint16_t totalChunks = totalChunkCount();
  char meta[160];
  snprintf(meta, sizeof(meta),
           "IMG type=" IMG_TAG " mime=" IMG_MIME " size=%lu mtu=%u chunkData=%u chunks=%u\n",
           (unsigned long)IMG_LEN, mtu, dataPerChunk, totalChunks);
  Serial.print(meta);
  notifyText(meta);
}

static void sendChunk(uint16_t seq) {
  const uint16_t dataPerChunk = chunkDataSize();
  const uint16_t totalChunks = totalChunkCount();
  if (seq >= totalChunks) {
    notifyText("DONE\n");
    Serial.println("DONE");
    return;
  }

  uint8_t packet[520];
  const uint32_t offset = (uint32_t)seq * dataPerChunk;
  const uint16_t len = min<uint32_t>(dataPerChunk, IMG_LEN - offset);
  put16le(packet + 0, seq);
  put16le(packet + 2, totalChunks);
  put32le(packet + 4, offset);
  for (uint16_t i = 0; i < len; i++) {
    packet[8 + i] = pgm_read_byte(IMG_DATA + offset + i);
  }
  notifyBytes(packet, 8 + len);
  Serial.printf("chunk seq=%u/%u offset=%lu len=%u\n", seq, totalChunks, (unsigned long)offset, len);
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    connected = true;
    Serial.printf("Connected: %s requestedMTU=%u\n",
                  connInfo.getAddress().toString().c_str(), NimBLEDevice::getMTU());
  }

  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    connected = false;
    notifyEnabled = false;
    Serial.printf("Disconnected reason=%d\n", reason);
    advertise();
  }

  void onMTUChange(uint16_t mtu, NimBLEConnInfo& connInfo) override {
    Serial.printf("MTU negotiated: %u\n", mtu);
  }
};

class StreamCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    std::string value = c->getValue();
    Serial.printf("WRITE len=%u peerMTU=%u\n", (unsigned)value.length(), currentPeerMTU());

    // "get <n>" -- pull a single chunk by sequence number.
    if (value.size() >= 5 && value.compare(0, 4, "get ") == 0) {
      uint16_t seq = (uint16_t)strtoul(value.c_str() + 4, nullptr, 10);
      sendChunk(seq);
      return;
    }

    // Otherwise treat as ASCII command (trimmed, lowercased).
    String msg;
    for (char ch : value) msg += ch;
    msg.trim();
    msg.toLowerCase();

    if (msg == "lets go" || msg == "let's go" || msg == "go" || msg == "start") {
      Serial.println("Stream requested -- sending metadata; client will pull chunks");
      sendMetadata();
      return;
    }

    char reply[128];
    snprintf(reply, sizeof(reply),
             "send 'lets go' then 'get <seq>' per chunk (" IMG_TAG "=%lu bytes)\n",
             (unsigned long)IMG_LEN);
    c->setValue((uint8_t*)reply, strlen(reply));
    if (notifyEnabled) c->notify();
  }

  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& connInfo, uint16_t subValue) override {
    notifyEnabled = (subValue & 0x0001) != 0;
    Serial.printf("Notifications %s\n", notifyEnabled ? "enabled" : "disabled");
  }
};

void setup() {
  for (uint8_t pin : LED_PINS) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }

  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("========================================");
  Serial.println("ESP32-C3 BLE image streamer (pull-mode) BOOT");
  Serial.printf("BLE name: %s\n", DEVICE_NAME);
  Serial.printf(IMG_TAG " bytes embedded: %lu\n", (unsigned long)IMG_LEN);
  Serial.println("Flow: enable notifications -> write 'lets go' -> write 'get <seq>' per chunk");
  Serial.println("========================================");

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setMTU(517); // request max ATT MTU; per-link value may be lower
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* service = server->createService(SERVICE_UUID);
  streamChar = service->createCharacteristic(
    CHAR_UUID,
    NIMBLE_PROPERTY::READ |
    NIMBLE_PROPERTY::WRITE |
    NIMBLE_PROPERTY::WRITE_NR |
    NIMBLE_PROPERTY::NOTIFY
  );
  streamChar->setCallbacks(new StreamCallbacks());
  streamChar->setValue("ready: enable notifications, write 'lets go', then 'get <seq>'");

  service->start();
  advertise();
}

void loop() {
  static uint32_t lastBlink = 0;
  static uint32_t lastLog = 0;

  if (millis() - lastBlink > 250) {
    lastBlink = millis();
    ledState = !ledState;
    for (uint8_t pin : LED_PINS) digitalWrite(pin, ledState ? HIGH : LOW);
  }

  if (millis() - lastLog > 5000) {
    lastLog = millis();
    Serial.printf("Status: connected=%s notify=%s peerMTU=%u image=%lu bytes\n",
                  connected ? "yes" : "no",
                  notifyEnabled ? "yes" : "no",
                  currentPeerMTU(),
                  (unsigned long)IMG_LEN);
  }

  delay(10);
}
