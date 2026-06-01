/*
  Adafruit Feather nRF52840 Sense Web Bluetooth tester

  Advertises as: dino tester

  Same Web Bluetooth API contract as the ESP32-C3 tester:
    Service UUID:        0000ffe0-0000-1000-8000-00805f9b34fb
    Characteristic UUID: 0000ffe1-0000-1000-8000-00805f9b34fb

  Commands:
    mtu             -> notify "MTU peerMTU=<N>"
    lets go/start   -> notify image metadata
    get <seq>       -> notify one binary image chunk
    status/help     -> notify help/status
    anything else   -> write-size ACK with the actual received length
*/

#include <bluefruit.h>
#include "dino_jpg.h"

#define IMG_DATA DINO_JPG
#define IMG_LEN  DINO_JPG_LEN
#define IMG_MIME "image/jpeg"
#define IMG_TAG  "JPG"

// 0000xxxx-0000-1000-8000-00805f9b34fb in little-endian byte order for Bluefruit.
#define BT_UUID_16_IN_BASE(val) \
  (const uint8_t[]) { \
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, \
    0x00, 0x10, 0x00, 0x00, (uint8_t)((val) & 0xff), (uint8_t)((val) >> 8), 0x00, 0x00 \
  }

BLEService testerService(BT_UUID_16_IN_BASE(0xffe0));
BLECharacteristic testerChar(BT_UUID_16_IN_BASE(0xffe1));

static const char* DEVICE_NAME = "dino tester";
static constexpr uint16_t WEB_BLUETOOTH_NOTIFY_CAP = 244;

static bool notifyEnabled = false;
static uint16_t currentConn = BLE_CONN_HANDLE_INVALID;
static uint32_t writeSeq = 0;

static uint16_t currentPeerMTU() {
  if (currentConn == BLE_CONN_HANDLE_INVALID) return 23;
  BLEConnection* conn = Bluefruit.Connection(currentConn);
  if (!conn) return 23;

  // Adafruit Bluefruit/SoftDevice can report 527 here on this board after
  // BANDWIDTH_MAX setup, even though this core's BLE_GATT_ATT_MTU_MAX is 247
  // and Chromium/Windows reports the negotiated ATT MTU as 247. For this
  // tester, report the effective ATT MTU used by Web Bluetooth write commands
  // and notifications so mtu.html compares apples-to-apples with the browser.
  uint16_t mtu = conn->getMtu();
  if (mtu < 23) mtu = 23;
  if (mtu > 247) mtu = 247;
  return mtu;
}

static uint16_t chunkDataSize() {
  const uint16_t mtu = currentPeerMTU();
  const uint16_t notifyPayloadMax = min<uint16_t>(WEB_BLUETOOTH_NOTIFY_CAP, (mtu > 3) ? (uint16_t)(mtu - 3) : (uint16_t)20);
  const uint16_t headerLen = 8;
  return (notifyPayloadMax > headerLen) ? (uint16_t)(notifyPayloadMax - headerLen) : (uint16_t)12;
}

static uint16_t totalChunkCount() {
  const uint16_t dataPerChunk = chunkDataSize();
  return (IMG_LEN + dataPerChunk - 1) / dataPerChunk;
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

static void notifyBytes(const uint8_t* data, uint16_t len) {
  if (currentConn == BLE_CONN_HANDLE_INVALID || !notifyEnabled) return;
  testerChar.notify(currentConn, data, len);
}

static void notifyText(const char* text) {
  notifyBytes((const uint8_t*)text, (uint16_t)strlen(text));
}

static void setReadStatus() {
  if (notifyEnabled) return; // avoid overwriting pending async notification value
  char status[128];
  snprintf(status, sizeof(status), "dino tester nRF52840 ready writes=%lu mtu=%u image=" IMG_TAG ":%lu",
           (unsigned long)writeSeq, currentPeerMTU(), (unsigned long)IMG_LEN);
  testerChar.write(status, strlen(status));
}

static void sendMetadata() {
  const uint16_t mtu = currentPeerMTU();
  const uint16_t dataPerChunk = chunkDataSize();
  const uint16_t chunks = totalChunkCount();
  char meta[180];
  snprintf(meta, sizeof(meta),
           "IMG type=" IMG_TAG " mime=" IMG_MIME " size=%lu mtu=%u chunkData=%u chunks=%u\n",
           (unsigned long)IMG_LEN, mtu, dataPerChunk, chunks);
  Serial.print(meta);
  notifyText(meta);
}

static void sendChunk(uint16_t seq) {
  const uint16_t dataPerChunk = chunkDataSize();
  const uint16_t chunks = totalChunkCount();
  if (seq >= chunks) {
    notifyText("DONE\n");
    Serial.println("DONE");
    return;
  }

  uint8_t packet[244];
  const uint32_t offset = (uint32_t)seq * dataPerChunk;
  const uint16_t len = min<uint32_t>(dataPerChunk, IMG_LEN - offset);
  put16le(packet + 0, seq);
  put16le(packet + 2, chunks);
  put32le(packet + 4, offset);
  for (uint16_t i = 0; i < len; i++) {
    packet[8 + i] = pgm_read_byte(IMG_DATA + offset + i);
  }
  notifyBytes(packet, 8 + len);
  Serial.printf("IMG chunk seq=%u/%u offset=%lu len=%u\n", seq, chunks, (unsigned long)offset, len);
}

static void startAdv() {
  Bluefruit.Advertising.clearData();
  Bluefruit.ScanResponse.clearData();
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(testerService);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);
  Serial.println("Advertising as dino tester");
}

void connect_callback(uint16_t conn_handle) {
  currentConn = conn_handle;
  writeSeq = 0;
  BLEConnection* conn = Bluefruit.Connection(conn_handle);
  Serial.printf("Connected requested max MTU=%u peerMTU=%u\n", Bluefruit.getMaxMtu(BLE_GAP_ROLE_PERIPH), currentPeerMTU());
  if (conn) {
    conn->requestPHY();
    conn->requestDataLengthUpdate();
    conn->requestMtuExchange(247);
  }
  setReadStatus();
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  (void)conn_handle;
  notifyEnabled = false;
  currentConn = BLE_CONN_HANDLE_INVALID;
  Serial.printf("Disconnected reason=0x%02x\n", reason);
}

void cccd_callback(uint16_t conn_hdl, BLECharacteristic* chr, uint16_t cccd_value) {
  (void)conn_hdl;
  (void)chr;
  (void)cccd_value;
  notifyEnabled = testerChar.notifyEnabled(conn_hdl);
  Serial.printf("Notifications %s\n", notifyEnabled ? "enabled" : "disabled");
}

void write_callback(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  (void)conn_hdl;
  (void)chr;
  writeSeq++;

  const uint8_t firstByte = len > 0 ? data[0] : 0;
  const uint8_t lastByte = len > 0 ? data[len - 1] : 0;
  Serial.printf("WRITE seq=%lu len=%u first=0x%02x last=0x%02x peerMTU=%u\n",
                (unsigned long)writeSeq, len, firstByte, lastByte, currentPeerMTU());

  if (len >= 5 && memcmp(data, "get ", 4) == 0) {
    uint16_t seq = (uint16_t)strtoul((const char*)data + 4, nullptr, 10);
    sendChunk(seq);
    return;
  }

  char msg[32] = {0};
  uint16_t n = min<uint16_t>(len, sizeof(msg) - 1);
  memcpy(msg, data, n);
  for (uint16_t i = 0; i < n; i++) msg[i] = (char)tolower((unsigned char)msg[i]);
  while (n > 0 && (msg[n - 1] == '\r' || msg[n - 1] == '\n' || msg[n - 1] == ' ')) msg[--n] = 0;

  if (!strcmp(msg, "mtu")) {
    char out[48];
    snprintf(out, sizeof(out), "MTU peerMTU=%u\n", currentPeerMTU());
    notifyText(out);
    return;
  }

  if (!strcmp(msg, "lets go") || !strcmp(msg, "let's go") || !strcmp(msg, "go") || !strcmp(msg, "start") || !strcmp(msg, "image")) {
    sendMetadata();
    return;
  }

  if (!strcmp(msg, "status") || !strcmp(msg, "help") || !strcmp(msg, "?")) {
    char help[220];
    snprintf(help, sizeof(help),
             "dino tester nRF52840: write bytes for RX ack; 'mtu'; 'lets go'; 'get <seq>'; mtu=%u image=" IMG_TAG ":%lu\n",
             currentPeerMTU(), (unsigned long)IMG_LEN);
    notifyText(help);
    return;
  }

  char ack[128];
  snprintf(ack, sizeof(ack), "RX seq=%lu len=%u first=0x%02x last=0x%02x mtu=%u\n",
           (unsigned long)writeSeq, len, firstByte, lastByte, currentPeerMTU());
  notifyText(ack);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("Feather nRF52840 Sense BLE tester BOOT");
  Serial.printf("BLE name: %s\n", DEVICE_NAME);
  Serial.printf(IMG_TAG " bytes embedded: %lu\n", (unsigned long)IMG_LEN);

  Bluefruit.autoConnLed(true);
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.begin();
  Bluefruit.setName(DEVICE_NAME);
  Bluefruit.setTxPower(4);
  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);
  Bluefruit.Periph.setConnInterval(6, 12);

  testerService.begin();
  testerChar.setProperties(CHR_PROPS_READ | CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP | CHR_PROPS_NOTIFY);
  testerChar.setPermission(SECMODE_OPEN, SECMODE_OPEN);
  testerChar.setMaxLen(512);
  testerChar.setWriteCallback(write_callback);
  testerChar.setCccdWriteCallback(cccd_callback);
  testerChar.begin();
  testerChar.write("dino tester nRF52840 ready", strlen("dino tester nRF52840 ready"));

  startAdv();
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last > 5000) {
    last = millis();
    setReadStatus();
    Serial.printf("Status: connected=%s notify=%s peerMTU=%u writes=%lu image=%lu bytes\n",
                  Bluefruit.connected() ? "yes" : "no",
                  notifyEnabled ? "yes" : "no",
                  currentPeerMTU(),
                  (unsigned long)writeSeq,
                  (unsigned long)IMG_LEN);
  }
  delay(10);
}
