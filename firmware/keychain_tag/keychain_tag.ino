/*
 * =========================================================================================
 * BLE CAR KEY FINDER & RANGE ALARM - KEY TAG UNIT (ESP32 #1)
 * =========================================================================================
 * Hardware Connections:
 *   - Buzzer (+)       -> GPIO 25 (Sounds on separation & find-my-key)
 *   - Buzzer (-)       -> GND
 *   - External LED     -> GPIO 4 (via 220 ohm resistor) and/or GPIO 26
 *   - LED Cathode      -> GND
 *   - On-Board LED     -> GPIO 2 (Built-in)
 *   - BOOT Button      -> GPIO 0 (Universal Silence Button)
 * =========================================================================================
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_bt.h>
#include <esp_gap_ble_api.h>

// ----------------- Pin Definitions -----------------
const int BUZZER_PIN     = 25;  // Buzzer (passive or active)
const int LED_PIN_MAIN   = 4;   // External LED via 220 ohm resistor
const int LED_PIN_ALT    = 26;  // Secondary/legacy external LED pin
const int LED_PIN_BOARD  = 2;   // Built-in blue LED
const int BTN_SILENCE    = 0;   // On-board BOOT button to silence sound

// Set to false if you are using an active buzzer
const bool PASSIVE_BUZZER = true;

// ----------------- BLE Configuration -----------------
#define SERVICE_UUID        "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CHAR_UUID           "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define DEVICE_NAME         "CarKey"

// Commands received from Handheld
const uint8_t CMD_STOP = 0;  // Silence / Normal
const uint8_t CMD_RING = 1;  // Find My Key (triggered by Handheld button)
const uint8_t CMD_FAR  = 2;  // Distance separation warning

// ----------------- Operation Modes / States -----------------
enum DeviceState {
  STATE_SEARCHING,   // Not connected, advertising
  STATE_CONNECTED,   // Connected, in safe range, silent
  STATE_RINGING,     // Find-my-key active (melody + rapid flash)
  STATE_FAR_WARN,    // Beyond distance limit (separation beeps)
  STATE_LINK_LOST    // Link dropped unexpectedly (triple beeps)
};

DeviceState currentState = STATE_SEARCHING;

// Timeouts
const unsigned long RING_TIMEOUT_MS     = 30000;  // Auto-stop find-my-key after 30s
const unsigned long LINK_LOST_ALERT_MS  = 60000;  // Auto-stop link-lost beeps after 60s

unsigned long stateStartTime = 0;
volatile bool isClientConnected = false;
volatile bool shouldRestartAdvertising = false;
volatile uint8_t pendingCommand = 255;

BLEServer* pServer = nullptr;
BLECharacteristic* pCharacteristic = nullptr;

// ----------------- Output Helper -----------------
void setLeds(bool on) {
  digitalWrite(LED_PIN_MAIN,  on ? HIGH : LOW);
  digitalWrite(LED_PIN_ALT,   on ? HIGH : LOW);
  digitalWrite(LED_PIN_BOARD, on ? HIGH : LOW);
}

void playTone(int freq, int durationMs) {
  if (PASSIVE_BUZZER) {
    if (freq > 0) {
      tone(BUZZER_PIN, freq, durationMs);
    } else {
      noTone(BUZZER_PIN);
    }
  } else {
    digitalWrite(BUZZER_PIN, (freq > 0) ? HIGH : LOW);
  }
}

void stopBuzzer() {
  if (PASSIVE_BUZZER) {
    noTone(BUZZER_PIN);
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }
}

// ----------------- BLE Server Callbacks -----------------
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    isClientConnected = true;
    currentState = STATE_CONNECTED;
    stopBuzzer();
    setLeds(true);
    Serial.println("[BLE] Handheld CONNECTED");
  }

  void onDisconnect(BLEServer* pServer) {
    isClientConnected = false;
    shouldRestartAdvertising = true;
    currentState = STATE_LINK_LOST;
    stateStartTime = millis();
    Serial.println("[BLE] Handheld DISCONNECTED -> Link Lost Alert activated");
  }
};

// ----------------- BLE Characteristic Callbacks -----------------
class MyCharCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) {
    auto value = pChar->getValue();
    if (value.length() > 0) {
      uint8_t cmd = (uint8_t)value[0];
      Serial.printf("[BLE] Command received from handheld: %d\n", cmd);
      pendingCommand = cmd;
    }
  }
};

// ----------------- Sound & LED Pattern Sequencer -----------------
unsigned long patternTimer = 0;
int patternStep = 0;

void executePatterns() {
  unsigned long now = millis();

  switch (currentState) {
    case STATE_SEARCHING:
      stopBuzzer();
      if (now - patternTimer >= 500) {
        patternTimer = now;
        patternStep = !patternStep;
        setLeds(patternStep);
      }
      break;

    case STATE_CONNECTED:
      stopBuzzer();
      setLeds(true);
      break;

    // FIND-MY-KEY Melodic Chime: 523Hz, 659Hz, 784Hz, 1047Hz
    case STATE_RINGING: {
      if (now - stateStartTime >= RING_TIMEOUT_MS) {
        Serial.println("[RING] 30s timeout reached -> returning to normal");
        currentState = isClientConnected ? STATE_CONNECTED : STATE_SEARCHING;
        stopBuzzer();
        setLeds(isClientConnected);
        break;
      }

      unsigned long elapsed = (now - stateStartTime) % 2100;

      if (elapsed < 140) {
        playTone(523, 130); setLeds(true);
      } else if (elapsed < 180) {
        stopBuzzer(); setLeds(false);
      } else if (elapsed < 320) {
        playTone(659, 130); setLeds(true);
      } else if (elapsed < 360) {
        stopBuzzer(); setLeds(false);
      } else if (elapsed < 500) {
        playTone(784, 130); setLeds(true);
      } else if (elapsed < 540) {
        stopBuzzer(); setLeds(false);
      } else if (elapsed < 860) {
        playTone(1047, 310); setLeds(true);
      } else if (elapsed < 940) {
        stopBuzzer(); setLeds(false);
      } else if (elapsed < 1080) {
        playTone(784, 130); setLeds(true);
      } else if (elapsed < 1120) {
        stopBuzzer(); setLeds(false);
      } else if (elapsed < 1540) {
        playTone(1047, 400); setLeds(true);
      } else {
        stopBuzzer(); setLeds(false);
      }
      break;
    }

    // SEPARATION / FAR WARNING: Two 880 Hz beeps
    case STATE_FAR_WARN: {
      unsigned long elapsed = (now - stateStartTime) % 1040;

      if (elapsed < 120) {
        playTone(880, 110); setLeds(true);
      } else if (elapsed < 220) {
        stopBuzzer(); setLeds(false);
      } else if (elapsed < 340) {
        playTone(880, 110); setLeds(true);
      } else {
        stopBuzzer(); setLeds(false);
      }
      break;
    }

    // LINK LOST: Three fast 988 Hz beeps
    case STATE_LINK_LOST: {
      if (now - stateStartTime >= LINK_LOST_ALERT_MS) {
        currentState = STATE_SEARCHING;
        stopBuzzer();
        setLeds(false);
        break;
      }

      unsigned long elapsed = (now - stateStartTime) % 1440;

      if (elapsed < 90) {
        playTone(988, 80); setLeds(true);
      } else if (elapsed < 180) {
        stopBuzzer(); setLeds(false);
      } else if (elapsed < 270) {
        playTone(988, 80); setLeds(true);
      } else if (elapsed < 360) {
        stopBuzzer(); setLeds(false);
      } else if (elapsed < 450) {
        playTone(988, 80); setLeds(true);
      } else {
        stopBuzzer(); setLeds(false);
      }
      break;
    }
  }
}

// ----------------- Silence Button Check (BOOT / GPIO 0) -----------------
void checkSilenceButton() {
  if (digitalRead(BTN_SILENCE) == LOW) {
    if (currentState == STATE_RINGING || currentState == STATE_FAR_WARN || currentState == STATE_LINK_LOST) {
      stopBuzzer();
      currentState = isClientConnected ? STATE_CONNECTED : STATE_SEARCHING;
      setLeds(isClientConnected);
      Serial.println("[BOOT BTN] Pressed on Key Tag -> Sound Silenced!");
      delay(300); // debounce
    }
  }
}

// ----------------- Startup Chirp -----------------
void playStartupChirp() {
  setLeds(true);
  playTone(1047, 120);
  delay(120);
  playTone(1319, 120);
  delay(120);
  stopBuzzer();
  setLeds(false);
  delay(100);
}

// ----------------- Arduino Setup -----------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n==============================================");
  Serial.println("  BLE Car Key Finder - Key Tag v3.5 (Updated)");
  Serial.println("==============================================");

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN_MAIN, OUTPUT);
  pinMode(LED_PIN_ALT, OUTPUT);
  pinMode(LED_PIN_BOARD, OUTPUT);
  pinMode(BTN_SILENCE, INPUT_PULLUP);

  stopBuzzer();
  setLeds(false);

  playStartupChirp();

  BLEDevice::init(DEVICE_NAME);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_N0);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
      CHAR_UUID,
      BLECharacteristic::PROPERTY_READ |
      BLECharacteristic::PROPERTY_WRITE |
      BLECharacteristic::PROPERTY_WRITE_NR
  );
  pCharacteristic->setCallbacks(new MyCharCallbacks());

  uint8_t initVal = 0;
  pCharacteristic->setValue(&initVal, 1);
  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);

  BLEDevice::startAdvertising();
  Serial.println("[BLE] Advertising started as 'CarKey'");

  currentState = STATE_SEARCHING;
  patternTimer = millis();
}

// ----------------- Arduino Main Loop -----------------
void loop() {
  // Check for local BOOT silence button press
  checkSilenceButton();

  // Handle incoming commands from Handheld
  if (pendingCommand != 255) {
    uint8_t cmd = pendingCommand;
    pendingCommand = 255;

    if (cmd == CMD_RING) {
      currentState = STATE_RINGING;
      stateStartTime = millis();
      Serial.println("[CMD] Handheld button pressed -> Melodic chime ringing!");
    } else if (cmd == CMD_FAR) {
      if (currentState != STATE_RINGING) {
        currentState = STATE_FAR_WARN;
        stateStartTime = millis();
        Serial.println("[CMD] Distance separation warning!");
      }
    } else if (cmd == CMD_STOP) {
      stopBuzzer();
      currentState = isClientConnected ? STATE_CONNECTED : STATE_SEARCHING;
      setLeds(isClientConnected);
      Serial.println("[CMD] Silenced / Returned to normal");
    }
  }

  // Safe re-advertising
  if (shouldRestartAdvertising) {
    shouldRestartAdvertising = false;
    delay(100);
    pServer->startAdvertising();
    Serial.println("[BLE] Re-advertising active");
  }

  executePatterns();
  delay(5);
}
