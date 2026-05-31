/*
  ESP32-C3 BLE image streamer

  Advertises as: chromium image streamer

  Protocol:
    1. Web Bluetooth client connects to service FFE0 / characteristic FFE1.
    2. Client enables notifications on FFE1.
    3. Client writes ASCII: lets go
    4. Device notifies:
       - ASCII metadata: DINO PNG size=<bytes> mtu=<mtu> chunkData=<payloadBytes>\n
       - Binary chunks with 8 byte little-endian header:
           uint16_t sequence
           uint16_t totalChunks
           uint32_t byteOffset
           uint8_t  pngData[]
       - ASCII: DONE\n
  The sketch requests ATT MTU 517. Browser/OS/peripheral will negotiate the real value.
*/

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "dino_png.h"

static const char* DEVICE_NAME = "chromium image streamer";
static const char* SERVICE_UUID = "0000ffe0-0000-1000-8000-00805f9b34fb";
static const char* CHAR_UUID    = "0000ffe1-0000-1000-8000-00805f9b34fb";

static NimBLEServer* server = nullptr;
static NimBLECharacteristic* streamChar = nullptr;
static bool connected = false;
static bool notifyEnabled = false;
static bool streamRequested = false;
static bool streaming = false;

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

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    connected = true;
    Serial.printf("Connected: %s MTU=%u\n", connInfo.getAddress().toString().c_str(), NimBLEDevice::getMTU());
  }

  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    connected = false;
    notifyEnabled = false;
    streamRequested = false;
    streaming = false;
    Serial.printf("Disconnected reason=%d\n", reason);
    advertise();
  }
};

class StreamCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    std::string value = c->getValue();
    String msg;
    for (char ch : value) msg += ch;
    msg.trim();
    msg.toLowerCase();

    Serial.printf("WRITE len=%u text='%s' MTU=%u\n", (unsigned)value.length(), msg.c_str(), NimBLEDevice::getMTU());

    if (msg == "lets go" || msg == "let's go" || msg == "go") {
      streamRequested = true;
      Serial.println("Stream requested");
    } else {
      char reply[96];
      snprintf(reply, sizeof(reply), "send 'lets go' to receive %lu byte PNG\n", (unsigned long)DINO_PNG_LEN);
      c->setValue((uint8_t*)reply, strlen(reply));
      if (notifyEnabled) c->notify();
    }
  }

  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& connInfo, uint16_t subValue) override {
    notifyEnabled = (subValue & 0x0001) != 0;
    Serial.printf("Notifications %s\n", notifyEnabled ? "enabled" : "disabled");
  }
};

static void streamImagePng() {
  if (!connected || !notifyEnabled || streaming) return;
  streaming = true;
  streamRequested = false;

  // Use the actual negotiated MTU for the current connection, not the locally
  // requested setMTU() value. NimBLEDevice::getMTU() returns the local request
  // and can be wrong if the peer negotiated a smaller MTU.
  uint16_t mtu = 23;
  if (server && server->getConnectedCount() > 0) {
    uint16_t connHandle = server->getPeerInfo(0).getConnHandle();
    mtu = server->getPeerMTU(connHandle);
  }
  if (mtu < 23) mtu = 23;
  const uint16_t notifyPayloadMax = (mtu > 3) ? (mtu - 3) : 20;
  const uint16_t headerLen = 8;
  const uint16_t dataPerChunk = (notifyPayloadMax > headerLen) ? (notifyPayloadMax - headerLen) : 12;
  const uint16_t totalChunks = (DINO_PNG_LEN + dataPerChunk - 1) / dataPerChunk;

  char meta[128];
  snprintf(meta, sizeof(meta), "PNG size=%lu mtu=%u chunkData=%u chunks=%u\n",
           (unsigned long)DINO_PNG_LEN, mtu, dataPerChunk, totalChunks);
  Serial.print(meta);
  notifyText(meta);
  delay(50);

  uint8_t packet[520];
  uint32_t offset = 0;
  for (uint16_t seq = 0; seq < totalChunks && connected && notifyEnabled; seq++) {
    const uint16_t len = min<uint32_t>(dataPerChunk, DINO_PNG_LEN - offset);
    put16le(packet + 0, seq);
    put16le(packet + 2, totalChunks);
    put32le(packet + 4, offset);
    for (uint16_t i = 0; i < len; i++) {
      packet[headerLen + i] = pgm_read_byte(DINO_PNG + offset + i);
    }

    notifyBytes(packet, headerLen + len);
    Serial.printf("chunk %u/%u offset=%lu len=%u\n", seq + 1, totalChunks, (unsigned long)offset, len);
    offset += len;
    delay(30); // be gentle with browser/OS notification queues
  }

  notifyText("DONE\n");
  Serial.println("DONE");
  streaming = false;
}

void setup() {
  for (uint8_t pin : LED_PINS) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }

  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("========================================");
  Serial.println("ESP32-C3 BLE image streamer BOOT");
  Serial.printf("BLE name: %s\n", DEVICE_NAME);
  Serial.printf("PNG bytes embedded: %lu\n", (unsigned long)DINO_PNG_LEN);
  Serial.println("Write 'lets go' after enabling notifications.");
  Serial.println("========================================");

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setMTU(517); // request max ATT MTU; negotiated value may be lower
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
  streamChar->setValue("ready: enable notifications, write lets go");

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

  if (streamRequested) streamImagePng();

  if (millis() - lastLog > 2000) {
    lastLog = millis();
    Serial.printf("Status: connected=%s notify=%s mtu=%u image=%lu bytes\n",
                  connected ? "yes" : "no",
                  notifyEnabled ? "yes" : "no",
                  NimBLEDevice::getMTU(),
                  (unsigned long)DINO_PNG_LEN);
  }

  delay(10);
}
