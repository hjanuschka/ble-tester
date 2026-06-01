/*
  ESP32-C3 all-in-one Chromium BLE tester

  Advertises as: dino tester

  One service/characteristic pair compatible with the existing Chromium sampler pages:
    Service UUID:        0000ffe0-0000-1000-8000-00805f9b34fb
    Characteristic UUID: 0000ffe1-0000-1000-8000-00805f9b34fb

  Features in one firmware:
    1. Generic MTU/write test peripheral:
       - accepts WRITE and WRITE_NR of any payload
       - logs actual received length to Serial
       - if notifications are enabled, replies with: RX seq=<n> len=<bytes> ... mtu=<peerMTU>

    2. Write-size confirmation tester:
       - works with writes.html style tests that need peripheral-side truth

    3. Dino/image streaming tester:
       - enable notifications
       - write: lets go
       - receive metadata notification
       - write: get 0, get 1, ... to pull image chunks
       - each chunk has 8-byte little-endian header:
           uint16_t sequence
           uint16_t totalChunks
           uint32_t byteOffset
           uint8_t  jpgData[]

  The sketch requests ATT MTU 517, but always uses NimBLEServer::getPeerMTU()
  for the real per-connection negotiated MTU.
*/

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "dino_jpg.h"

#define IMG_DATA DINO_JPG
#define IMG_LEN  DINO_JPG_LEN
#define IMG_MIME "image/jpeg"
#define IMG_TAG  "JPG"

static const char* DEVICE_NAME  = "dino tester";
static const char* SERVICE_UUID = "0000ffe0-0000-1000-8000-00805f9b34fb";
static const char* CHAR_UUID    = "0000ffe1-0000-1000-8000-00805f9b34fb";

static NimBLEServer* server = nullptr;
static NimBLECharacteristic* testerChar = nullptr;
static bool connected = false;
static bool notifyEnabled = false;
static uint32_t writeSeq = 0;

// Onboard ESP32-C3 RGB LED. This is the board's built-in LED mapping,
// not an external header pin. The esp32c3 Arduino variant maps RGB_BUILTIN
// to the onboard WS2812/SK6812 data line.
static constexpr uint8_t STATUS_RGB_LED_PIN = RGB_BUILTIN;
static constexpr uint8_t STATUS_RGB_LED_BRIGHTNESS = 64;

static void setStatusLedColor(uint8_t red, uint8_t green, uint8_t blue) {
  // RGB_BUILTIN is the Arduino board definition for the onboard addressable LED.
  rgbLedWrite(STATUS_RGB_LED_PIN, red, green, blue);
}

static void setStatusLedBoot() {
  setStatusLedColor(STATUS_RGB_LED_BRIGHTNESS, STATUS_RGB_LED_BRIGHTNESS / 3, 0); // orange = booting
}

static void setStatusLedIdle() {
  setStatusLedColor(STATUS_RGB_LED_BRIGHTNESS, 0, 0); // red = advertising / idle
}

static void setStatusLedConnected() {
  setStatusLedColor(0, 0, STATUS_RGB_LED_BRIGHTNESS); // blue = connected / paired
}

static void advertise() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(DEVICE_NAME);
  adv->addServiceUUID(SERVICE_UUID);
  adv->enableScanResponse(true);
  adv->start();
  Serial.println("Advertising as dino tester");
}

static uint16_t currentPeerMTU() {
  if (!server || server->getConnectedCount() == 0) return 23;
  uint16_t handle = server->getPeerInfo(0).getConnHandle();
  uint16_t mtu = server->getPeerMTU(handle);
  return mtu < 23 ? 23 : mtu;
}

// NOTE: do NOT call this immediately after a notify*() helper. NimBLE
// schedules the notification asynchronously; if setValue() runs first the
// notification packet ends up carrying the new value (the status string)
// instead of the binary chunk / metadata we just queued. Call this only
// when the client did not enable notifications, or right before another
// setValue+notify pair.
static void updateReadValue() {
  if (!testerChar) return;
  if (notifyEnabled) return; // notifications use setValue+notify themselves
  char status[128];
  snprintf(status, sizeof(status),
           "dino tester ready writes=%lu mtu=%u image=" IMG_TAG ":%lu",
           (unsigned long)writeSeq, currentPeerMTU(), (unsigned long)IMG_LEN);
  testerChar->setValue((uint8_t*)status, strlen(status));
}

static void notifyBytes(const uint8_t* data, size_t len) {
  if (!testerChar || !notifyEnabled || !connected) return;
  testerChar->setValue(data, len);
  testerChar->notify();
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

// Practical Web Bluetooth notification cap. Even if a link negotiates >247 ATT MTU,
// Chromium stacks are most reliable at <=244 notification payload bytes.
static constexpr uint16_t WEB_BLUETOOTH_NOTIFY_CAP = 244;

static uint16_t chunkDataSize() {
  const uint16_t mtu = currentPeerMTU();
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
  char meta[180];
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
  Serial.printf("IMG chunk seq=%u/%u offset=%lu len=%u\n", seq, totalChunks, (unsigned long)offset, len);
}

static void sendWriteAck(size_t len, uint8_t firstByte, uint8_t lastByte) {
  char ack[120];
  snprintf(ack, sizeof(ack),
           "RX seq=%lu len=%u first=0x%02x last=0x%02x mtu=%u\n",
           (unsigned long)writeSeq, (unsigned)len, firstByte, lastByte, currentPeerMTU());
  notifyText(ack);
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    connected = true;
    writeSeq = 0;
    setStatusLedConnected();
    Serial.printf("Connected: %s requestedMTU=%u peerMTU=%u\n",
                  connInfo.getAddress().toString().c_str(),
                  NimBLEDevice::getMTU(), currentPeerMTU());
    updateReadValue();
  }

  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    connected = false;
    notifyEnabled = false;
    setStatusLedIdle();
    Serial.printf("Disconnected reason=%d\n", reason);
    advertise();
  }

  void onMTUChange(uint16_t mtu, NimBLEConnInfo& connInfo) override {
    Serial.printf("MTU negotiated: %u\n", mtu);
    updateReadValue();
  }
};

class TesterCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    std::string value = c->getValue();
    const size_t len = value.length();
    const uint8_t firstByte = len > 0 ? (uint8_t)value[0] : 0;
    const uint8_t lastByte  = len > 0 ? (uint8_t)value[len - 1] : 0;
    writeSeq++;

    Serial.printf("WRITE seq=%lu len=%u first=0x%02x last=0x%02x peerMTU=%u\n",
                  (unsigned long)writeSeq, (unsigned)len, firstByte, lastByte, currentPeerMTU());

    // Image pull: get <seq>
    if (value.size() >= 5 && value.compare(0, 4, "get ") == 0) {
      uint16_t seq = (uint16_t)strtoul(value.c_str() + 4, nullptr, 10);
      sendChunk(seq);
      return;
    }

    // ASCII commands.
    String msg;
    for (char ch : value) msg += ch;
    msg.trim();
    msg.toLowerCase();

    if (msg == "lets go" || msg == "let's go" || msg == "go" || msg == "start" || msg == "image") {
      Serial.println("Image stream requested -- sending metadata; client should pull chunks with get <seq>");
      sendMetadata();
      return;
    }

    if (msg == "status" || msg == "help" || msg == "?") {
      char help[220];
      snprintf(help, sizeof(help),
               "dino tester: write arbitrary bytes for RX ack; write 'lets go' for image metadata; write 'get <seq>' for chunks; mtu=%u image=" IMG_TAG ":%lu\n",
               currentPeerMTU(), (unsigned long)IMG_LEN);
      notifyText(help);
      return;
    }

    // Default behavior: write-size tester ACK.
    sendWriteAck(len, firstByte, lastByte);
  }

  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& connInfo, uint16_t subValue) override {
    notifyEnabled = (subValue & 0x0001) != 0;
    Serial.printf("Notifications %s\n", notifyEnabled ? "enabled" : "disabled");
  }
};

void setup() {
  setStatusLedBoot();

  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("========================================");
  Serial.println("ESP32-C3 all-in-one Chromium BLE tester BOOT");
  Serial.printf("BLE name: %s\n", DEVICE_NAME);
  Serial.printf(IMG_TAG " bytes embedded: %lu\n", (unsigned long)IMG_LEN);
  Serial.println("Modes: MTU/write-size ACK + pull-mode image chunks");
  Serial.println("========================================");

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setMTU(517);
  // Lower TX power to reduce current draw and chip temperature.
  // P9 is maximum; P3 is plenty for nearby desktop testing.
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* service = server->createService(SERVICE_UUID);
  testerChar = service->createCharacteristic(
    CHAR_UUID,
    NIMBLE_PROPERTY::READ |
    NIMBLE_PROPERTY::WRITE |
    NIMBLE_PROPERTY::WRITE_NR |
    NIMBLE_PROPERTY::NOTIFY
  );
  testerChar->setCallbacks(new TesterCallbacks());
  testerChar->setValue("dino tester ready");

  service->start();
  setStatusLedIdle();
  advertise();
}

void loop() {
  static uint32_t lastLog = 0;

  if (millis() - lastLog > 5000) {
    lastLog = millis();
    updateReadValue();
    Serial.printf("Status: connected=%s notify=%s peerMTU=%u writes=%lu image=%lu bytes\n",
                  connected ? "yes" : "no",
                  notifyEnabled ? "yes" : "no",
                  currentPeerMTU(),
                  (unsigned long)writeSeq,
                  (unsigned long)IMG_LEN);
  }

  delay(10);
}
