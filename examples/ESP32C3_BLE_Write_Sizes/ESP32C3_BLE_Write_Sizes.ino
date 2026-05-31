/*
  ESP32-C3 BLE write-size probe

  Advertises as: chromium write sizes

  Companion to Chromium issue 40686244 ("20 byte MTU for web-bluetooth on
  Windows Chrome"). Use with the dino-style sampler at:
    https://static.januschka.com/i-40265040/writes.html

  Behavior:
    - One characteristic FFE1 with READ | WRITE | WRITE_NR | NOTIFY.
    - For every write the device receives, it logs to Serial AND sends back
      an ASCII notification:
        "RX seq=<n> len=<bytes> first=0xXX last=0xXX\n"
      so the browser can independently confirm how many bytes actually
      reached the peripheral, no matter which write API was used.
    - The device requests ATT MTU 517; the per-connection negotiated value
      is reported via NimBLEServer::getPeerMTU().

  Purpose:
    The browser-side page sweeps a range of payload sizes through both
    writeValueWithResponse() and writeValueWithoutResponse() and records:
      - whether the JS Promise resolved (browser-side success)
      - the actual length received by the peripheral (firmware-side truth)
    This makes it obvious whether Chromium is honoring the negotiated MTU
    on writes or silently capping at 20 bytes (the historical Windows bug).
*/

#include <Arduino.h>
#include <NimBLEDevice.h>

static const char* DEVICE_NAME  = "chromium write sizes";
static const char* SERVICE_UUID = "0000ffe0-0000-1000-8000-00805f9b34fb";
static const char* CHAR_UUID    = "0000ffe1-0000-1000-8000-00805f9b34fb";

static NimBLEServer* server = nullptr;
static NimBLECharacteristic* probeChar = nullptr;
static bool connected = false;
static bool notifyEnabled = false;
static uint32_t writeSeq = 0;

static const uint8_t LED_PINS[] = {2, 8, 10};
static bool ledState = false;

static void advertise() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(DEVICE_NAME);
  adv->addServiceUUID(SERVICE_UUID);
  adv->enableScanResponse(true);
  adv->start();
  Serial.println("Advertising as chromium write sizes");
}

static uint16_t currentPeerMTU() {
  if (!server || server->getConnectedCount() == 0) return 23;
  uint16_t handle = server->getPeerInfo(0).getConnHandle();
  uint16_t mtu = server->getPeerMTU(handle);
  return mtu < 23 ? 23 : mtu;
}

static void notifyBytes(const uint8_t* data, size_t len) {
  if (!probeChar || !notifyEnabled || !connected) return;
  probeChar->setValue(data, len);
  probeChar->notify();
}

static void notifyText(const char* text) {
  notifyBytes((const uint8_t*)text, strlen(text));
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    connected = true;
    writeSeq = 0;
    Serial.printf("Connected: %s requestedMTU=%u\n",
                  connInfo.getAddress().toString().c_str(),
                  NimBLEDevice::getMTU());
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

class ProbeCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    std::string value = c->getValue();
    const size_t len = value.length();
    const uint8_t firstByte = len > 0 ? (uint8_t)value[0] : 0;
    const uint8_t lastByte  = len > 0 ? (uint8_t)value[len - 1] : 0;
    writeSeq++;

    Serial.printf("WRITE seq=%lu len=%u first=0x%02x last=0x%02x peerMTU=%u\n",
                  (unsigned long)writeSeq, (unsigned)len, firstByte, lastByte,
                  currentPeerMTU());

    char ack[96];
    snprintf(ack, sizeof(ack),
             "RX seq=%lu len=%u first=0x%02x last=0x%02x mtu=%u\n",
             (unsigned long)writeSeq, (unsigned)len, firstByte, lastByte,
             currentPeerMTU());
    notifyText(ack);
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
  Serial.println("ESP32-C3 BLE write-size probe BOOT");
  Serial.printf("BLE name: %s\n", DEVICE_NAME);
  Serial.println("Use https://static.januschka.com/i-40265040/writes.html");
  Serial.println("========================================");

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setMTU(517);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* service = server->createService(SERVICE_UUID);
  probeChar = service->createCharacteristic(
    CHAR_UUID,
    NIMBLE_PROPERTY::READ |
    NIMBLE_PROPERTY::WRITE |
    NIMBLE_PROPERTY::WRITE_NR |
    NIMBLE_PROPERTY::NOTIFY
  );
  probeChar->setCallbacks(new ProbeCallbacks());
  probeChar->setValue("ready: enable notifications and start writing");

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
    Serial.printf("Status: connected=%s notify=%s peerMTU=%u writes=%lu\n",
                  connected ? "yes" : "no",
                  notifyEnabled ? "yes" : "no",
                  currentPeerMTU(),
                  (unsigned long)writeSeq);
  }

  delay(10);
}
