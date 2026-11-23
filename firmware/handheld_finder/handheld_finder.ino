/*
 * =========================================================================================
 * BLE CAR KEY FINDER & RANGE ALARM - HANDHELD / POCKET UNIT (ESP32 #2)
 * =========================================================================================
 * Hardware Connections:
 *   - External Push Button -> GPIO 4 (Active LOW with internal pull-up)
 *   - On-Board BOOT Button -> GPIO 0 (Universal Silence / Calibrate)
 *   - Handheld Buzzer      -> GPIO 25 (Sounds on separation & alert)
 *   - 16x2 I2C LCD SDA     -> GPIO 21
 *   - 16x2 I2C LCD SCL     -> GPIO 22
 *   - 16x2 I2C LCD VCC     -> VIN (5V)
 *   - 16x2 I2C LCD GND     -> GND
 *   - On-Board LED         -> GPIO 2
 * =========================================================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Preferences.h>
#include <esp_bt.h>
#include <esp_gap_ble_api.h>

// ----------------- Pin Definitions -----------------
const int BTN_EXT_PIN     = 4;   // External push-button on GPIO 4 to trigger ringing
const int BTN_BOOT_PIN    = 0;   // On-board BOOT button to silence alarms / calibrate
const int BUZZER_PIN      = 25;  // Handheld buzzer (sounds on separation & alerts)
const int LED_PIN         = 2;   // On-board status LED (GPIO 2)

// ----------------- BLE Configuration -----------------
#define SERVICE_UUID        "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CHAR_UUID           "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define TARGET_DEVICE_NAME  "CarKey"

// Commands sent to the CarKey Tag
const uint8_t CMD_STOP = 0;  // Silence / Normal
const uint8_t CMD_RING = 1;  // Find My Key melody & bright flash
const uint8_t CMD_FAR  = 2;  // Out-of-range alarm

// ----------------- Signal & Range Settings -----------------
// Changed to -82 dBm so it doesn't trigger false alarms when close (1m - 1.5m)
const int DEFAULT_LIMIT_RSSI     = -82;
const int HYSTERESIS_DB          = 5;    // Must recover by 5 dB to clear FAR alarm
const int FAR_CONFIRM_COUNT      = 5;    // Requires 5 consecutive readings below limit (~1.5s)
const int NEAR_CONFIRM_COUNT     = 2;    // Requires 2 consecutive readings above limit
const unsigned long RSSI_POLL_MS = 300;  // RSSI sample interval (ms)
const unsigned long WARMUP_MS    = 3000; // Suppress false alarms for first 3s after connect

// ----------------- Button Timing (ms) -----------------
const unsigned long CALIBRATE_HOLD_MS = 3500; // Hold BOOT for >= 3.5s to recalibrate limit

// ----------------- Global Objects -----------------
Preferences prefs;
LiquidCrystal_I2C lcd(0x27, 16, 2);

static BLEUUID serviceUUID(SERVICE_UUID);
static BLEUUID charUUID(CHAR_UUID);

BLEClient* pClient = nullptr;
BLEScan*   pBLEScan = nullptr;
BLERemoteCharacteristic* pRemoteChar = nullptr;
BLEAdvertisedDevice* targetDevice = nullptr;

// ----------------- State Machine Variables -----------------
volatile bool connected       = false;
volatile bool doConnect       = false;
volatile bool scanInProgress  = false;
bool wasConnected             = false;

bool isKeyRinging  = false;
bool farAlert      = false;
bool linkLostAlert = false;
int  farCounter    = 0;
int  nearCounter   = 0;

unsigned long connectTimestamp = 0;

// Filtered RSSI data (Median of 7 + Exponential Moving Average)
const int FILTER_SIZE = 7;
int rssiSamples[FILTER_SIZE];
int sampleIdx   = 0;
int sampleCount = 0;
float smoothedRssi = -65.0;
int currentLimit = DEFAULT_LIMIT_RSSI;

unsigned long lastRssiTime  = 0;
unsigned long lastLcdUpdate = 0;
unsigned long lastLedBlink  = 0;
unsigned long buzzerTimer   = 0;
bool ledState = false;

// Button debounce tracking
bool extBtnPrev = HIGH;
unsigned long extBtnChangeTime = 0;

bool bootBtnWasPressed = false;
unsigned long bootBtnPressTime = 0;
bool calibTriggered = false;

// Display cache to eliminate LCD flicker
char cachedL0[17] = "";
char cachedL1[17] = "";

// ----------------- Helper: I2C Scanner -----------------
uint8_t findLcdAddress() {
  Wire.begin(21, 22);
  byte commonAddrs[] = {0x27, 0x3F, 0x20, 0x38};
  for (byte addr : commonAddrs) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C] LCD detected at 0x%02X\n", addr);
      return addr;
    }
  }
  return 0x27;
}

// ----------------- LCD Safe Print -----------------
void updateLcdRow(int row, const char* str) {
  char formatted[17];
  snprintf(formatted, sizeof(formatted), "%-16s", str);
  char* target = (row == 0) ? cachedL0 : cachedL1;
  if (strncmp(target, formatted, 16) != 0) {
    strncpy(target, formatted, 16);
    target[16] = '\0';
    lcd.setCursor(0, row);
    lcd.print(formatted);
  }
}

// ----------------- Local Handheld Buzzer -----------------
void driveHandheldBuzzer() {
  unsigned long now = millis();

  // 1. Separation / FAR Alert: Two distinct beeps
  if (farAlert && !isKeyRinging) {
    unsigned long cycle = (now - buzzerTimer) % 1040;
    if (cycle < 120 || (cycle >= 220 && cycle < 340)) {
      digitalWrite(BUZZER_PIN, HIGH);
    } else {
      digitalWrite(BUZZER_PIN, LOW);
    }
  }
  // 2. Link Lost Alert: Three fast beeps
  else if (linkLostAlert) {
    unsigned long cycle = (now - buzzerTimer) % 1440;
    if (cycle < 90 || (cycle >= 180 && cycle < 270) || (cycle >= 360 && cycle < 450)) {
      digitalWrite(BUZZER_PIN, HIGH);
    } else {
      digitalWrite(BUZZER_PIN, LOW);
    }
  }
  // 3. Find-My-Key active: Pulse buzzer briefly every 2s as audio confirmation
  else if (isKeyRinging) {
    unsigned long cycle = (now - buzzerTimer) % 2000;
    if (cycle < 80) {
      digitalWrite(BUZZER_PIN, HIGH);
    } else {
      digitalWrite(BUZZER_PIN, LOW);
    }
  }
  else {
    digitalWrite(BUZZER_PIN, LOW);
  }
}

void silenceAllAlerts() {
  isKeyRinging = false;
  farAlert = false;
  linkLostAlert = false;
  farCounter = 0;
  nearCounter = 0;
  digitalWrite(BUZZER_PIN, LOW);
}

// ----------------- BLE Client Callbacks -----------------
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* client) {
    Serial.println("[BLE] Handheld: Connected to CarKey");
  }
  void onDisconnect(BLEClient* client) {
    connected = false;
    isKeyRinging = false;
    farAlert = false;
    linkLostAlert = true; // Ring handheld buzzer on link loss
    buzzerTimer = millis();
    Serial.println("[BLE] Handheld: Link lost -> Handheld buzzer ringing!");
  }
};

// ----------------- BLE Scan Callbacks -----------------
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    bool match = false;
    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(serviceUUID)) {
      match = true;
    } else if (advertisedDevice.haveName() && advertisedDevice.getName() == TARGET_DEVICE_NAME) {
      match = true;
    }

    if (match) {
      Serial.printf("[SCAN] Found CarKey: %s (RSSI %d)\n",
                    advertisedDevice.getAddress().toString().c_str(),
                    advertisedDevice.getRSSI());
      
      BLEDevice::getScan()->stop();
      if (targetDevice != nullptr) {
        delete targetDevice;
        targetDevice = nullptr;
      }
      targetDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
      scanInProgress = false;
    }
  }
};

// ----------------- Send Command to Key Tag -----------------
bool sendBleCommand(uint8_t cmd) {
  if (!connected || pRemoteChar == nullptr) return false;
  try {
    pRemoteChar->writeValue(&cmd, 1, false);
    Serial.printf("[CMD] Sent to Key: %d (%s)\n", cmd,
                  (cmd == CMD_RING) ? "RING" : (cmd == CMD_FAR) ? "FAR" : "STOP");
    return true;
  } catch (...) {
    return false;
  }
}

// ----------------- BLE Connection Handler -----------------
bool connectToCarKey() {
  if (targetDevice == nullptr) return false;

  updateLcdRow(0, "Connecting...");
  updateLcdRow(1, targetDevice->getAddress().toString().c_str());

  if (pClient == nullptr) {
    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(new MyClientCallback());
  }

  if (!pClient->connect(targetDevice)) {
    Serial.println("[BLE] Connection failed!");
    pClient->disconnect();
    if (targetDevice != nullptr) {
      delete targetDevice;
      targetDevice = nullptr;
    }
    return false;
  }

  BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    pClient->disconnect();
    if (targetDevice != nullptr) {
      delete targetDevice;
      targetDevice = nullptr;
    }
    return false;
  }

  pRemoteChar = pRemoteService->getCharacteristic(charUUID);
  if (pRemoteChar == nullptr) {
    pClient->disconnect();
    if (targetDevice != nullptr) {
      delete targetDevice;
      targetDevice = nullptr;
    }
    return false;
  }

  if (targetDevice != nullptr) {
    delete targetDevice;
    targetDevice = nullptr;
  }

  connected = true;
  connectTimestamp = millis();
  silenceAllAlerts();
  sampleCount = 0;
  smoothedRssi = currentLimit + 10; // start safely inside zone

  Serial.println("[BLE] Successfully connected!");
  // Short confirmation beep
  digitalWrite(BUZZER_PIN, HIGH); delay(80); digitalWrite(BUZZER_PIN, LOW);
  return true;
}

// ----------------- RSSI Filter & Distance Engine -----------------
int computeMedian7(int* arr, int n) {
  int temp[FILTER_SIZE];
  for (int i = 0; i < n; i++) temp[i] = arr[i];
  for (int i = 1; i < n; i++) {
    int key = temp[i];
    int j = i - 1;
    while (j >= 0 && temp[j] > key) {
      temp[j + 1] = temp[j];
      j--;
    }
    temp[j + 1] = key;
  }
  return temp[n / 2];
}

void processRssi() {
  if (!connected || pClient == nullptr) return;

  int rawRssi = 0;
  try {
    rawRssi = pClient->getRssi();
  } catch (...) {
    return;
  }

  // Filter out invalid noise
  if (rawRssi < -110 || rawRssi >= 0) return;

  rssiSamples[sampleIdx] = rawRssi;
  sampleIdx = (sampleIdx + 1) % FILTER_SIZE;
  if (sampleCount < FILTER_SIZE) sampleCount++;

  int medianRssi = computeMedian7(rssiSamples, sampleCount);

  // Exponential Moving Average filter (alpha = 0.25 for smooth response)
  if (sampleCount == 1) {
    smoothedRssi = (float)medianRssi;
  } else {
    smoothedRssi = (0.25f * medianRssi) + (0.75f * smoothedRssi);
  }

  int filteredRssi = (int)round(smoothedRssi);

  // If Find-My-Key ring is active or within 3s warmup, do not trigger false FAR alarms
  if (isKeyRinging || (millis() - connectTimestamp < WARMUP_MS)) {
    return;
  }

  // Hysteresis & Debounced Distance Separation Logic
  if (!farAlert) {
    if (filteredRssi < currentLimit) {
      farCounter++;
    } else {
      farCounter = 0;
    }

    // Must be below limit for 5 consecutive cycles (~1.5s) to prevent close-range spikes
    if (farCounter >= FAR_CONFIRM_COUNT) {
      farAlert = true;
      farCounter = 0;
      nearCounter = 0;
      buzzerTimer = millis();
      sendBleCommand(CMD_FAR);
      Serial.printf("[SEPARATION] Key separated! RSSI: %d (Limit: %d) -> Buzzers ringing!\n", filteredRssi, currentLimit);
    }
  } else {
    // Requires recovering by HYSTERESIS_DB (5 dB) to stop alert
    if (filteredRssi >= (currentLimit + HYSTERESIS_DB)) {
      nearCounter++;
    } else {
      nearCounter = 0;
    }

    if (nearCounter >= NEAR_CONFIRM_COUNT) {
      farAlert = false;
      farCounter = 0;
      nearCounter = 0;
      digitalWrite(BUZZER_PIN, LOW);
      sendBleCommand(CMD_STOP);
      Serial.printf("[SEPARATION] Key back in range! RSSI: %d\n", filteredRssi);
    }
  }
}

// ----------------- Buttons Handling -----------------
void handleButtons() {
  unsigned long now = millis();

  // 1. EXTERNAL PUSH BUTTON ON GPIO 4: Trigger Key Module Ringing
  bool extCurrent = digitalRead(BTN_EXT_PIN);
  if (extCurrent != extBtnPrev && (now - extBtnChangeTime > 50)) {
    extBtnChangeTime = now;
    extBtnPrev = extCurrent;

    // Pressed down
    if (extCurrent == LOW) {
      if (connected) {
        if (!isKeyRinging) {
          // Ring Key Module
          isKeyRinging = true;
          farAlert = false;
          buzzerTimer = now;
          sendBleCommand(CMD_RING);
          Serial.println("[BTN GPIO4] Pressed -> RINGING Key Module!");
        } else {
          // Already ringing -> Turn off
          isKeyRinging = false;
          sendBleCommand(CMD_STOP);
          digitalWrite(BUZZER_PIN, LOW);
          Serial.println("[BTN GPIO4] Pressed -> Ringing STOPPED!");
        }
      } else {
        Serial.println("[BTN GPIO4] Not connected to Key Module");
      }
    }
  }

  // 2. ON-BOARD BOOT BUTTON ON GPIO 0: Universal Silence & Calibrate
  bool bootDown = (digitalRead(BTN_BOOT_PIN) == LOW);
  if (bootDown) {
    if (!bootBtnWasPressed) {
      bootBtnWasPressed = true;
      bootBtnPressTime = now;
      calibTriggered = false;

      // If any alarm is currently ringing, pressing BOOT turns it off immediately!
      if (isKeyRinging || farAlert || linkLostAlert) {
        silenceAllAlerts();
        sendBleCommand(CMD_STOP);
        Serial.println("[BOOT BTN] Pressed -> Universal Silence Activated!");
      }
    } else {
      // Long hold >= 3.5s: Recalibrate Range Limit at current distance
      if ((now - bootBtnPressTime >= CALIBRATE_HOLD_MS) && !calibTriggered) {
        calibTriggered = true;
        if (connected) {
          currentLimit = (int)round(smoothedRssi);
          prefs.putInt("limit", currentLimit);
          digitalWrite(BUZZER_PIN, HIGH); delay(120); digitalWrite(BUZZER_PIN, LOW);
          updateLcdRow(0, "Limit Saved!");
          char buf[17];
          snprintf(buf, sizeof(buf), "Calib: %d dBm", currentLimit);
          updateLcdRow(1, buf);
          Serial.printf("[CALIB] New range limit saved: %d dBm\n", currentLimit);
          delay(1200);
        }
      }
    }
  } else {
    bootBtnWasPressed = false;
  }
}

// ----------------- Screen Rendering -----------------
void renderScreen() {
  char l0[17], l1[17];

  if (!connected) {
    if (linkLostAlert) {
      updateLcdRow(0, "Key:LINK LOST!");
      updateLcdRow(1, "BOOT: Silence");
    } else {
      updateLcdRow(0, "Searching key...");
      updateLcdRow(1, "P4:Ring BOOT:Off");
    }
    return;
  }

  if (isKeyRinging) {
    updateLcdRow(0, "RINGING KEY...");
    snprintf(l1, sizeof(l1), "R:%d P4/BOOT:Off", (int)round(smoothedRssi));
    updateLcdRow(1, l1);
    return;
  }

  if (farAlert) {
    snprintf(l0, sizeof(l0), "Key:FAR! %d", (int)round(smoothedRssi));
    snprintf(l1, sizeof(l1), "Lim:%d ALERT!", currentLimit);
  } else {
    snprintf(l0, sizeof(l0), "Key:OK  %d dBm", (int)round(smoothedRssi));
    snprintf(l1, sizeof(l1), "Lim:%d P4:Ring", currentLimit);
  }

  updateLcdRow(0, l0);
  updateLcdRow(1, l1);
}

// ----------------- LED State Driver -----------------
void updateLedIndicator() {
  unsigned long now = millis();

  if (!connected) {
    if (now - lastLedBlink >= 500) {
      lastLedBlink = now;
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    }
  } else if (isKeyRinging || farAlert) {
    if (now - lastLedBlink >= 100) {
      lastLedBlink = now;
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    }
  } else {
    digitalWrite(LED_PIN, HIGH);
  }
}

// ----------------- Arduino Setup -----------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n==============================================");
  Serial.println("  BLE Key Finder - Handheld Unit v3.5 (Updated)");
  Serial.println("==============================================");

  pinMode(BTN_EXT_PIN, INPUT_PULLUP);
  pinMode(BTN_BOOT_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  uint8_t lcdAddr = findLcdAddress();
  lcd = LiquidCrystal_I2C(lcdAddr, 16, 2);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  updateLcdRow(0, "Key Finder v3.5");
  updateLcdRow(1, "Starting BLE...");

  prefs.begin("carkey", false);
  currentLimit = prefs.getInt("limit", DEFAULT_LIMIT_RSSI);
  Serial.printf("[CONFIG] Range limit loaded: %d dBm\n", currentLimit);

  BLEDevice::init("Pocket");
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_N0);

  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);

  delay(600);
}

// ----------------- Arduino Main Loop -----------------
void loop() {
  unsigned long now = millis();

  if (connected && !wasConnected) {
    wasConnected = true;
    Serial.println("[STATE] Link CONNECTED");
  }
  if (!connected && wasConnected) {
    wasConnected = false;
    Serial.println("[STATE] Link DISCONNECTED -> re-scanning");
    if (pClient != nullptr) {
      pClient->disconnect();
    }
    pRemoteChar = nullptr;
    scanInProgress = false;
  }

  // Handle Button Inputs (GPIO 4 push-button & GPIO 0 BOOT silence)
  handleButtons();

  // Drive Handheld Buzzer for separation / alerts
  driveHandheldBuzzer();

  // Manage Connection & RSSI
  if (connected) {
    if (now - lastRssiTime >= RSSI_POLL_MS) {
      lastRssiTime = now;
      processRssi();
    }
  } else {
    if (doConnect) {
      doConnect = false;
      connectToCarKey();
    } else if (!scanInProgress) {
      scanInProgress = true;
      pBLEScan->start(2, false);
      pBLEScan->clearResults();
      scanInProgress = false;
    }
  }

  // Update Display
  if (now - lastLcdUpdate >= 250) {
    lastLcdUpdate = now;
    renderScreen();
  }

  updateLedIndicator();
  delay(10);
}
