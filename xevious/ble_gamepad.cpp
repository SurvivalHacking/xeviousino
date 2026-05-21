/*
 * BLE HID Host Gamepad using NimBLE library
 * Adapted from GALAGONE 3.5" (oraQuadra_Nano implementation)
 * Requires: NimBLE-Arduino library by h2zero (install from Library Manager)
 *
 * Tested: ShanWan Q36, Terios T3, Xbox BLE, generic BLE HID gamepads
 *
 * Output: NES-format bitmask (bit=1 = pressed)
 *   bit 0=UP, 1=DOWN, 2=LEFT, 3=RIGHT, 4=SELECT, 5=START, 6=A, 7=B, 8=X, 9=Y
 */

#include "ble_gamepad.h"

#include <NimBLEDevice.h> // NimBLE-Arduino da h2zero
#include "esp_log.h"
#include "esp_mac.h"
#include <EEPROM.h>

static const char *TAG = "BLE_GP";

// ====== Config ======
#define BLE_GP_SCAN_TIME      0       // 0 = scan indefinitely until found
#define BLE_GP_STICK_DEADZONE 50
#define BLE_GP_HID_SVC        "1812"
#define BLE_GP_REPORT_CHR     "2a4d"
#define BLE_GP_PROTOCOL_MODE  "2a4e"
#define BLE_GP_MAX_RETRIES    3

// NES bitmask bits (bit=1 = pressed)
#define NES_UP     (1 << 0)
#define NES_DOWN   (1 << 1)
#define NES_LEFT   (1 << 2)
#define NES_RIGHT  (1 << 3)
#define NES_SELECT (1 << 4)
#define NES_START  (1 << 5)
#define NES_A      (1 << 6)
#define NES_B      (1 << 7)
#define NES_X      (1 << 8)
#define NES_Y      (1 << 9)

// EEPROM layout for BLE address (after volume bytes 0-1)
// [30]=magic, [31]=addr_type, [32..49]=address string
#define EEPROM_BLE_MAGIC_ADDR 30
#define EEPROM_BLE_MAGIC_VAL  0xBE
#define EEPROM_BLE_TYPE_ADDR  31
#define EEPROM_BLE_STR_START  32
#define EEPROM_BLE_STR_LEN    18  // "XX:XX:XX:XX:XX:XX" + null
#define EEPROM_BLE_SIZE       50

// ====== State ======
static volatile uint32_t _buttons = 0;
static volatile bool _connected = false;
static volatile bool _initialized = false;
static volatile bool _connecting = false;
static NimBLEClient *_client = nullptr;
static NimBLEAddress *_targetAddr = nullptr;
static uint8_t _retryCount = 0;

// ====== Save/Load gamepad address to EEPROM ======
static void saveGamepadAddr(const NimBLEAddress &addr) {
  EEPROM.begin(EEPROM_BLE_SIZE);
  uint8_t type = addr.getType();
  std::string s = addr.toString();
  EEPROM.write(EEPROM_BLE_MAGIC_ADDR, EEPROM_BLE_MAGIC_VAL);
  EEPROM.write(EEPROM_BLE_TYPE_ADDR, type);
  for (int i = 0; i < EEPROM_BLE_STR_LEN && i < (int)s.length() + 1; i++)
    EEPROM.write(EEPROM_BLE_STR_START + i, i < (int)s.length() ? s[i] : 0);
  EEPROM.commit();
  Serial.printf("[BLE] Gamepad address saved: %s (type=%d)\n", s.c_str(), type);
}

static bool loadGamepadAddr(NimBLEAddress &addr) {
  EEPROM.begin(EEPROM_BLE_SIZE);
  if (EEPROM.read(EEPROM_BLE_MAGIC_ADDR) != EEPROM_BLE_MAGIC_VAL)
    return false;
  uint8_t type = EEPROM.read(EEPROM_BLE_TYPE_ADDR);
  char buf[EEPROM_BLE_STR_LEN];
  for (int i = 0; i < EEPROM_BLE_STR_LEN; i++)
    buf[i] = EEPROM.read(EEPROM_BLE_STR_START + i);
  buf[EEPROM_BLE_STR_LEN - 1] = 0;
  addr = NimBLEAddress(std::string(buf), type);
  Serial.printf("[BLE] Loaded saved gamepad: %s (type=%d)\n", buf, type);
  return true;
}

// ====== Format Detection ======
enum GpFormat { FMT_UNKNOWN, FMT_STANDARD_8, FMT_XBOX_BLE, FMT_LONG_GENERIC, FMT_Q36_10 };
static GpFormat _format = FMT_UNKNOWN;

// ====== Known gamepad names ======
static bool isKnownGamepad(const std::string &name) {
  if (name.empty()) return false;
  const char *known[] = {
    "Q36", "ShanWan", "shanwan", "Terios", "GameSir", "Xbox", "XBOX",
    "Wireless Controller", "Pro Controller", "8BitDo", "80EL",
    "GamePad", "Gamepad", "gamepad", "Controller", "Joystick",
    NULL
  };
  for (int i = 0; known[i]; i++) {
    if (name.find(known[i]) != std::string::npos) return true;
  }
  return false;
}

// ====== Hat Switch Parser ======
static uint32_t parseHat(uint8_t hat) {
  uint32_t b = 0;
  if (hat == 0 || hat == 1 || hat == 7) b |= NES_UP;
  if (hat == 1 || hat == 2 || hat == 3) b |= NES_RIGHT;
  if (hat == 3 || hat == 4 || hat == 5) b |= NES_DOWN;
  if (hat == 5 || hat == 6 || hat == 7) b |= NES_LEFT;
  return b;
}

// ====== Stick Axis Parser ======
static uint32_t parseStick8(uint8_t x, uint8_t y) {
  uint32_t b = 0;
  if (x < (128 - BLE_GP_STICK_DEADZONE)) b |= NES_LEFT;
  if (x > (128 + BLE_GP_STICK_DEADZONE)) b |= NES_RIGHT;
  if (y < (128 - BLE_GP_STICK_DEADZONE)) b |= NES_UP;
  if (y > (128 + BLE_GP_STICK_DEADZONE)) b |= NES_DOWN;
  return b;
}

// ====== HID Report Parser ======
static uint32_t parseReport(const uint8_t *data, size_t len) {
  if (len < 2) return 0;
  uint32_t buttons = 0;

  // Auto-detect format on first report
  if (_format == FMT_UNKNOWN) {
    if (len >= 14 && data[0] == 0x01) {
      _format = FMT_XBOX_BLE;
      ESP_LOGI(TAG, "Format: Xbox BLE (%d bytes)", len);
    } else if (len == 10) {
      _format = FMT_Q36_10;
      ESP_LOGI(TAG, "Format: Q36/ShanWan (%d bytes)", len);
    } else if (len == 8) {
      _format = FMT_STANDARD_8;
      ESP_LOGI(TAG, "Format: Standard HID (%d bytes)", len);
    } else if (len >= 10) {
      _format = FMT_LONG_GENERIC;
      ESP_LOGI(TAG, "Format: Generic (%d bytes)", len);
    } else {
      _format = FMT_STANDARD_8;
      ESP_LOGI(TAG, "Format: Fallback standard (%d bytes)", len);
    }
  }

  switch (_format) {
    case FMT_STANDARD_8:
      if (len >= 5) {
        buttons |= parseStick8(data[0], data[1]);
        buttons |= parseHat(data[4] & 0x0F);
        if (data[4] & 0x10) buttons |= NES_A;      // A
        if (data[4] & 0x20) buttons |= NES_B;      // B
        if (len >= 6) {
          if (data[5] & 0x04) buttons |= NES_SELECT;
          if (data[5] & 0x08) buttons |= NES_START;
        }
      }
      break;

    case FMT_XBOX_BLE:
      if (len >= 14) {
        uint8_t btnsLo = data[1];
        buttons |= parseHat(data[3]);
        int16_t lx = (int16_t)(data[4] | (data[5] << 8));
        int16_t ly = (int16_t)(data[6] | (data[7] << 8));
        uint8_t nx = (uint8_t)((lx >> 8) + 128);
        uint8_t ny = (uint8_t)((ly >> 8) + 128);
        buttons |= parseStick8(nx, ny);
        if (btnsLo & 0x01) buttons |= NES_A;       // A
        if (btnsLo & 0x02) buttons |= NES_B;       // B
        if (btnsLo & 0x04) buttons |= NES_A;       // X → A
        if (btnsLo & 0x08) buttons |= NES_B;       // Y → B
        if (btnsLo & 0x40) buttons |= NES_SELECT;
        if (btnsLo & 0x80) buttons |= NES_START;
      }
      break;

    case FMT_Q36_10:
      if (len >= 7) {
        buttons |= parseStick8(data[0], data[1]);
        uint8_t hat = data[4];
        if (hat <= 7) buttons |= parseHat(hat);
        uint8_t b1 = data[5];
        uint8_t b2 = data[6];
        if (b1 & 0x01) buttons |= NES_A;           // A
        if (b1 & 0x02) buttons |= NES_B;           // B
        if (b1 & 0x04) buttons |= NES_A;           // X → A
        if (b1 & 0x08) buttons |= NES_B;           // Y → B
        if (b1 & 0x10) buttons |= NES_A;           // L1 → A
        if (b1 & 0x20) buttons |= NES_B;           // R1 → B
        if (b2 & 0x04) buttons |= NES_SELECT;
        if (b2 & 0x08) buttons |= NES_START;
      }
      break;

    case FMT_LONG_GENERIC:
      if (len >= 7) {
        buttons |= parseStick8(data[0], data[1]);
        buttons |= parseHat(data[6]);
        if (data[4] & 0x01) buttons |= NES_A;
        if (data[4] & 0x02) buttons |= NES_B;
        if (data[5] & 0x10) buttons |= NES_SELECT;
        if (data[5] & 0x20) buttons |= NES_START;
      }
      break;

    default: break;
  }
  return buttons;
}

// ====== Notify Callback ======
static void notifyCallback(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify) {
  uint32_t parsed = parseReport(data, len);

  // Debug: log when buttons change
  static uint32_t lastParsed = 0;
  if (parsed != lastParsed) {
    ESP_LOGI(TAG, "Buttons -> 0x%03X %s%s%s%s%s%s%s%s", parsed,
      (parsed & NES_UP)     ? "UP " : "", (parsed & NES_DOWN)   ? "DN " : "",
      (parsed & NES_LEFT)   ? "LT " : "", (parsed & NES_RIGHT)  ? "RT " : "",
      (parsed & NES_A)      ? "A " : "",  (parsed & NES_B)      ? "B " : "",
      (parsed & NES_SELECT) ? "SEL " : "", (parsed & NES_START)  ? "STA " : "");
    lastParsed = parsed;
  }

  _buttons = parsed;
}

// ====== Scan Callback ======
class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    bool hasHID = dev->isAdvertisingService(NimBLEUUID(BLE_GP_HID_SVC));
    std::string devName = dev->getName();
    bool knownName = isKnownGamepad(devName);

    if (hasHID || knownName) {
      ESP_LOGI(TAG, "FOUND: \"%s\" HID=%d addr=%s type=%d",
               devName.c_str(), hasHID,
               dev->getAddress().toString().c_str(),
               dev->getAddress().getType());
      NimBLEDevice::getScan()->stop();
      if (_targetAddr) delete _targetAddr;
      _targetAddr = new NimBLEAddress(dev->getAddress());
      _retryCount = 0;
    }
  }
};

// ====== Client Callbacks ======
class ClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient *client) override {
    _connected = true;
    ESP_LOGI(TAG, "Connected!");
  }
  void onDisconnect(NimBLEClient *client, int reason) override {
    _connected = false;
    _buttons = 0;
    _format = FMT_UNKNOWN;
    ESP_LOGW(TAG, "Disconnected (reason=%d)", reason);
    if (!_initialized) return;
    // Always restart scan on disconnect — most reliable way to reconnect
    // The gamepad might change address or need fresh pairing
    if (_targetAddr) {
      delete _targetAddr;
      _targetAddr = nullptr;
    }
    _retryCount = 0;
    if (!_connecting) {
      Serial.println("[BLE] Restarting scan after disconnect...");
      NimBLEDevice::getScan()->start(BLE_GP_SCAN_TIME, false);
    }
  }
};

// ====== Connect and subscribe ======
static void tryConnect() {
  if (!_targetAddr) return;

  ESP_LOGI(TAG, "Connecting to %s...", _targetAddr->toString().c_str());

  if (_client == nullptr) {
    _client = NimBLEDevice::createClient();
    _client->setClientCallbacks(new ClientCB(), false);
  }

  if (!_client->connect(*_targetAddr, false)) {
    _retryCount++;
    Serial.printf("[BLE] Connect failed (attempt %d/%d)\n", _retryCount, BLE_GP_MAX_RETRIES);
    if (_retryCount >= BLE_GP_MAX_RETRIES) {
      Serial.println("[BLE] Max retries, back to scan...");
      NimBLEDevice::deleteBond(*_targetAddr);
      delete _targetAddr;
      _targetAddr = nullptr;
      _retryCount = 0;
      NimBLEDevice::getScan()->start(BLE_GP_SCAN_TIME, false);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    return;
  }

  // Set connection parameters for low latency
  _client->updateConnParams(12, 24, 0, 400);
  _retryCount = 0;

  // "Just Works" secure connection (encryption without PIN)
  Serial.println("[BLE] Securing connection (Just Works)...");
  if (_client->secureConnection()) {
    Serial.println("[BLE] Secure connection OK!");
  } else {
    Serial.println("[BLE] Secure connection failed, continuing...");
  }
  vTaskDelay(pdMS_TO_TICKS(300));

  // Find HID service
  NimBLERemoteService *hidSvc = _client->getService(NimBLEUUID(BLE_GP_HID_SVC));
  if (!hidSvc) {
    ESP_LOGE(TAG, "HID service (0x1812) NOT found! Services:");
    auto allSvc = _client->getServices(true);
    for (auto &s : allSvc) {
      ESP_LOGI(TAG, "  -> %s", s->getUUID().toString().c_str());
    }
    _client->disconnect();
    return;
  }
  ESP_LOGI(TAG, "HID service found!");

  // Set Protocol Mode to Report Protocol (0x01) if available
  NimBLERemoteCharacteristic *protMode = hidSvc->getCharacteristic(NimBLEUUID(BLE_GP_PROTOCOL_MODE));
  if (protMode && protMode->canWrite()) {
    uint8_t reportProto = 0x01;
    protMode->writeValue(&reportProto, 1, true);
    ESP_LOGI(TAG, "Protocol Mode set to Report");
  }
  vTaskDelay(pdMS_TO_TICKS(100));

  // Subscribe to ALL HID Report characteristics
  int subscribed = 0;
  auto chars = hidSvc->getCharacteristics(true);
  for (auto &chr : chars) {
    if (chr->getUUID() == NimBLEUUID(BLE_GP_REPORT_CHR) && chr->canNotify()) {
      vTaskDelay(pdMS_TO_TICKS(100));
      if (chr->subscribe(true, notifyCallback)) {
        subscribed++;
        ESP_LOGI(TAG, "Subscribed report handle=%d", chr->getHandle());
      }
    }
  }

  if (subscribed > 0) {
    Serial.printf("[BLE] === GAMEPAD READY! %d report(s) active ===\n", subscribed);
    // Save gamepad address so we can reconnect directly after reboot
    if (_targetAddr) {
      saveGamepadAddr(*_targetAddr);
    }
  } else {
    Serial.println("[BLE] No reports subscribed!");
    _client->disconnect();
  }
}

// ====== Connect task ======
static void connectTask(void *param) {
  tryConnect();
  _connecting = false;
  vTaskDelete(NULL);
}

// ====== Scan+connect task on core 1 (matching oraQuadra) ======
static void ble_scan_task(void *arg) {
  Serial.println("[BLE] Init NimBLE...");

  NimBLEDevice::init("NES_EMU");

  // ESP32-S3: some modules have no factory public BLE address.
  // Try public first; if it fails, set a fixed random-static address
  // derived from the WiFi MAC so it stays the same across reboots.
  if (!NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC)) {
    Serial.println("[BLE] No public address, setting fixed random-static from WiFi MAC");
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    // Random-static address: two MSBs of last byte must be 11
    mac[5] |= 0xC0;
    NimBLEDevice::setOwnAddr(mac);
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
  }

  Serial.printf("[BLE] Address: %s\n", NimBLEDevice::getAddress().toString().c_str());

  // Clean start: delete ALL old bonds to avoid stale encryption keys
  // that prevent reconnection after reboot
  int numBonds = NimBLEDevice::getNumBonds();
  if (numBonds > 0) {
    Serial.printf("[BLE] Clearing %d old bond(s) for clean pairing...\n", numBonds);
    for (int i = numBonds - 1; i >= 0; i--) {
      NimBLEDevice::deleteBond(NimBLEDevice::getBondedAddress(i));
    }
  }

  // "Just Works" pairing: bonding enabled + SC, NO MITM
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  // Configure scan — more aggressive window for faster detection
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(new ScanCB(), false);
  scan->setActiveScan(true);
  scan->setInterval(80);   // 80 × 0.625ms = 50ms interval
  scan->setWindow(40);     // 40 × 0.625ms = 25ms window (50% duty)

  // Always start scan — most reliable approach
  // Even with a saved address, we scan because the gamepad might have
  // changed address (RPA) or need fresh discovery after power cycle
  NimBLEAddress savedAddr;
  bool hasSaved = loadGamepadAddr(savedAddr);
  if (hasSaved) {
    Serial.printf("[BLE] Saved gamepad: %s (will match in scan)\n",
                  savedAddr.toString().c_str());
  }

  Serial.println("[BLE] Starting scan for HID gamepads...");
  scan->start(BLE_GP_SCAN_TIME, false);

  _initialized = true;

  // Main loop: check for target and connect
  while (true) {
    if (_targetAddr && !_connected && !_connecting) {
      _connecting = true;
      // Run connect on core 1 (same as oraQuadra)
      xTaskCreatePinnedToCore(connectTask, "bleGpConn", 6144, NULL, 1, NULL, 1);
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void ble_gamepad_init(void) {
  ESP_LOGI(TAG, "==============================");
  ESP_LOGI(TAG, "BLE HID Host (NimBLE)");
  ESP_LOGI(TAG, "==============================");

  // Core 1, priority 1 — matching oraQuadra's working configuration
  xTaskCreatePinnedToCore(ble_scan_task, "ble_scan", 8192, NULL, 1, NULL, 1);

  ESP_LOGI(TAG, "BLE ready - put controller in pairing mode!");
}

uint32_t ble_gamepad_buttons(void) {
  return _buttons;
}

bool ble_gamepad_connected(void) {
  return _connected;
}
