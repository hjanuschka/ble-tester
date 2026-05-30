/*
  ESP32-C3 Web Bluetooth MTU/write-size test peripheral

  Use with: https://static.januschka.com/i-40265040/

  Arduino IDE board suggestion:
    Board: "ESP32C3 Dev Module" (or your exact ESP32-C3 Mini board)
    USB CDC On Boot: Enabled (nice for Serial logs)

  Web page values:
    Service UUID:        0000ffe0-0000-1000-8000-00805f9b34fb
    Characteristic UUID: 0000ffe1-0000-1000-8000-00805f9b34fb

  The characteristic accepts Write With Response and Write Without Response.
  Every write is logged on Serial and, if notifications are enabled, an ACK is sent back.
*/

#include <Arduino.h>
#include <NimBLEDevice.h>

static const char* DEVICE_NAME = "chromium ble tester";
static const char* SERVICE_UUID = "0000ffe0-0000-1000-8000-00805f9b34fb";
static const char* CHAR_UUID    = "0000ffe1-0000-1000-8000-00805f9b34fb";

static NimBLEServer* server = nullptr;
static NimBLECharacteristic* ioChar = nullptr;
static bool deviceConnected = false;
static bool notificationsEnabled = false;
static uint32_t writeCount = 0;
static uint16_t maxWriteLen = 0;

// Many ESP32-C3 Mini boards use one of these pins for a blue/user LED.
// This sketch toggles all three so you should see *some* blink if an LED exists.
static const uint8_t LED_PINS[] = {2, 8, 10};
static bool ledState = false;

static void updateStatusValue() {
  if (!ioChar) return;
  char status[96];
  snprintf(status, sizeof(status), "ready writes=%lu max=%u mtu=%u uptime=%lus",
           (unsigned long)writeCount, maxWriteLen, NimBLEDevice::getMTU(), (unsigned long)(millis() / 1000));
  ioChar->setValue((uint8_t*)status, strlen(status));
}

static void advertise() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(DEVICE_NAME);
  adv->addServiceUUID(SERVICE_UUID);
  adv->enableScanResponse(true);
  adv->start();
  Serial.println("Advertising as ESP32C3-MTU-Test with service FFE0");
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    deviceConnected = true;
    Serial.printf("Connected: %s, conn_handle=%u, MTU=%u\n",
                  connInfo.getAddress().toString().c_str(),
                  connInfo.getConnHandle(),
                  NimBLEDevice::getMTU());
  }

  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected = false;
    notificationsEnabled = false;
    Serial.printf("Disconnected, reason=%d\n", reason);
    advertise();
  }
};

class CharacteristicCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    std::string value = c->getValue();
    const uint16_t len = value.length();
    writeCount++;
    if (len > maxWriteLen) maxWriteLen = len;

    Serial.printf("WRITE #%lu len=%u max=%u MTU=%u firstBytes=",
                  (unsigned long)writeCount, len, maxWriteLen, NimBLEDevice::getMTU());
    for (uint16_t i = 0; i < len && i < 12; i++) {
      Serial.printf("%02X ", (uint8_t)value[i]);
    }
    if (len > 12) Serial.print("...");
    Serial.println();

    char ack[64];
    snprintf(ack, sizeof(ack), "ACK #%lu len=%u max=%u", (unsigned long)writeCount, len, maxWriteLen);
    c->setValue((uint8_t*)ack, strlen(ack));
    if (notificationsEnabled) {
      c->notify();
    }
  }

  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& connInfo, uint16_t subValue) override {
    notificationsEnabled = (subValue & 0x0001) != 0;
    Serial.printf("Notifications %s\n", notificationsEnabled ? "enabled" : "disabled");
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
  Serial.println("================================================");
  Serial.println("ESP32-C3 Web Bluetooth MTU/write-size test BOOT");
  Serial.println("BLE name: chromium ble tester");
  Serial.println("LED heartbeat pins being toggled: GPIO2, GPIO8, GPIO10");
  Serial.println("If Serial is blank: set Tools > USB CDC On Boot > Enabled, then upload again.");
  Serial.println("================================================");

  // Request a larger ATT MTU. The central/browser may negotiate lower.
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* service = server->createService(SERVICE_UUID);

  ioChar = service->createCharacteristic(
    CHAR_UUID,
    NIMBLE_PROPERTY::READ |
    NIMBLE_PROPERTY::WRITE |
    NIMBLE_PROPERTY::WRITE_NR |
    NIMBLE_PROPERTY::NOTIFY
  );
  ioChar->setCallbacks(new CharacteristicCallbacks());
  ioChar->setValue("ready booting");

  service->start();
  advertise();

  updateStatusValue();
  Serial.println("Ready. In Chrome page click Connect and select chromium ble tester.");
  Serial.println("Probe writeValueWithoutResponse at 20, 21, 32, 64, 128, etc.");
}

void loop() {
  static uint32_t lastBlink = 0;
  static uint32_t lastLog = 0;

  if (millis() - lastBlink > 250) {
    lastBlink = millis();
    ledState = !ledState;
    for (uint8_t pin : LED_PINS) {
      digitalWrite(pin, ledState ? HIGH : LOW);
    }
  }

  // Log often so you can open Serial Monitor after reset and still see activity.
  if (millis() - lastLog > 1000) {
    lastLog = millis();
    updateStatusValue();
    Serial.printf("Status: connected=%s writes=%lu maxWriteLen=%u localMTU=%u uptime=%lus\n",
                  deviceConnected ? "yes" : "no",
                  (unsigned long)writeCount,
                  maxWriteLen,
                  NimBLEDevice::getMTU(),
                  (unsigned long)(millis() / 1000));
  }
  delay(20);
}
