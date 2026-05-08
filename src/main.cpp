#include <Arduino.h>
#include <U8g2lib.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_bt_device.h>

// ==================== PIN DEFINITIONS ====================
#define OLED_SDA    1
#define OLED_SCL    2
#define OLED_RST    -1   // Not used
#define SW1_PIN     20   // Mode switch
#define SW2_PIN     21   // Start/Stop switch
#define LED_PIN     0    // Status LED (active LOW on ESP32-C3)
#define BAT_PIN     4    // Battery voltage monitor (optional)

// ==================== OLED SETUP ====================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, OLED_RST, OLED_SCL, OLED_SDA);

// ==================== MENU SYSTEM ====================
typedef enum {
  MODE_BLE_ADV_FLOOD,
  MODE_BLE_SCAN_FLOOD,
  MODE_BLE_CONNECT_FLOOD,
  MODE_BLE_SCAN_AND_ADV,
  MODE_COUNT
} JammerMode_t;

JammerMode_t currentMode = MODE_BLE_ADV_FLOOD;
bool jammerRunning = false;
bool modeChanged = true;

// ==================== STATISTICS ====================
volatile uint32_t packetsSent = 0;
volatile uint32_t packetsReceived = 0;
unsigned long startTime = 0;
unsigned long lastStatsUpdate = 0;

// ==================== BLE ADVERTISING DATA ====================
// Massive advertising payload (max 31 bytes)
uint8_t advData[] = {
  0x02, 0x01, 0x06,                     // Flags: LE General Discoverable
  0x03, 0x02, 0x00, 0x18,              // Complete 16-bit UUIDs: 0x1800
  0x11, 0x09,                          // Complete Local Name (17 chars)
  'B','L','E',' ','J','A','M','M','E','R',' ','V','1','.','0','!','!',
  0x05, 0x08,                          // TX Power Level
  0x00, 0x00, 0x00, 0x00              // Padding
};
uint8_t advDataLen = 27;

// Random MAC address generator
uint8_t randomMAC[6];
esp_ble_adv_params_t advParams;

// ==================== SCAN CALLBACK ====================
class JammerAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    packetsReceived++;
  }
};

BLEScan* pBLEScan = nullptr;
BLEAdvertisedDeviceCallbacks* scanCallbacks = nullptr;

// ==================== LED ANIMATION ====================
void animateLED(int pattern) {
  switch(pattern) {
    case 0: // Slow blink (idle)
      for(int i=0; i<3; i++) {
        digitalWrite(LED_PIN, LOW); delay(100);
        digitalWrite(LED_PIN, HIGH); delay(400);
      }
      break;
    case 1: // Fast blink (active)
      for(int i=0; i<5; i++) {
        digitalWrite(LED_PIN, LOW); delay(50);
        digitalWrite(LED_PIN, HIGH); delay(50);
      }
      break;
    case 2: // Double blink
      for(int i=0; i<2; i++) {
        digitalWrite(LED_PIN, LOW); delay(80);
        digitalWrite(LED_PIN, HIGH); delay(80);
      }
      delay(500);
      break;
  }
}

// ==================== OLED UI FUNCTIONS ====================
void drawSplashScreen() {
  u8g2.clearBuffer();
  
  // Draw animated radar circle
  static int radarAngle = 0;
  radarAngle = (radarAngle + 5) % 360;
  
  u8g2.drawCircle(64, 32, 28);
  u8g2.drawCircle(64, 32, 20);
  u8g2.drawCircle(64, 32, 12);
  
  // Radar sweep line
  float rad = radarAngle * 3.14159 / 180;
  int ex = 64 + (int)(28 * cos(rad));
  int ey = 32 + (int)(28 * sin(rad));
  u8g2.drawLine(64, 32, ex, ey);
  
  // Title
  u8g2.setFont(u8g2_font_10x20_tf);
  u8g2.drawStr(20, 58, "BLE JAMMER");
  
  u8g2.sendBuffer();
}

void drawMainScreen() {
  u8g2.clearBuffer();
  
  // Top bar - Title
  u8g2.setDrawColor(1);
  u8g2.drawBox(0, 0, 128, 14);
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_6x10_tf);
  
  const char* modeNames[] = {"ADV FLOOD", "SCAN FLOOD", "CONN FLOOD", "SCN+ADV"};
  char title[24];
  snprintf(title, sizeof(title), " %s %s", 
           jammerRunning ? ">>" : "||",
           modeNames[currentMode]);
  u8g2.drawStr(4, 11, title);
  
  u8g2.setDrawColor(1);
  
  // Channel info
  u8g2.setFont(u8g2_font_6x12_tf);
  char line1[24];
  snprintf(line1, sizeof(line1), "CH:37-38-39 | 2.402GHz");
  u8g2.drawStr(0, 28, line1);
  
  // Progress bar for activity
  if (jammerRunning) {
    unsigned long elapsed = millis() - startTime;
    int barWidth = (elapsed / 100) % 128;
    if (barWidth > 128) barWidth = 128;
    u8g2.drawFrame(0, 30, 128, 6);
    u8g2.drawBox(0, 30, barWidth, 6);
  }
  
  // Stats
  u8g2.setFont(u8g2_font_5x8_tf);
  char line2[24], line3[24], line4[24];
  snprintf(line2, sizeof(line2), "TX:%05u  RX:%05u", packetsSent, packetsReceived);
  u8g2.drawStr(0, 44, line2);
  
  unsigned long elapsed = jammerRunning ? (millis() - startTime) / 1000 : 0;
  snprintf(line3, sizeof(line3), "TIME:%03lus  PKT/s:%03u", 
           (unsigned long)elapsed, 
           packetsSent / max((unsigned long)1, elapsed));
  u8g2.drawStr(0, 54, line3);
  
  // Bottom status bar
  u8g2.drawHLine(0, 56, 128);
  snprintf(line4, sizeof(line4), "SW1:MODE  SW2:%s", 
           jammerRunning ? "STOP" : "START");
  u8g2.drawStr(0, 63, line4);
  
  u8g2.sendBuffer();
}

// ==================== BUTTON HANDLING ====================
void checkButtons() {
  static bool lastSW1 = HIGH, lastSW2 = HIGH;
  bool sw1 = digitalRead(SW1_PIN);
  bool sw2 = digitalRead(SW2_PIN);
  
  // SW1 - Mode change (debounced)
  if (sw1 == LOW && lastSW1 == HIGH) {
    delay(50); // Debounce
    if (digitalRead(SW1_PIN) == LOW) {
      if (!jammerRunning) {
        currentMode = (JammerMode_t)((currentMode + 1) % MODE_COUNT);
        modeChanged = true;
        animateLED(2);
      }
    }
  }
  
  // SW2 - Start/Stop (debounced)
  if (sw2 == LOW && lastSW2 == HIGH) {
    delay(50);
    if (digitalRead(SW2_PIN) == LOW) {
      jammerRunning = !jammerRunning;
      if (jammerRunning) {
        startTime = millis();
        packetsSent = 0;
        packetsReceived = 0;
        animateLED(0);
      } else {
        animateLED(2);
        digitalWrite(LED_PIN, HIGH); // OFF
      }
    }
  }
  
  lastSW1 = sw1;
  lastSW2 = sw2;
}

// ==================== JAMMER MODES ====================
void startBLEAdvertisingFlood() {
  for (int ch = 37; ch <= 39; ch++) {
    esp_ble_gap_config_local_privacy(false);
    esp_ble_gap_set_prefer_conn_params(ESP_BLE_CONN_INT_MIN, ESP_BLE_CONN_INT_MAX, 
                                       0, 0x600, 0x200, 0x0, 0x600);
  }
  
  // Generate random MAC
  for (int i = 0; i < 6; i++) {
    randomMAC[i] = random(0x00, 0xFF);
    randomMAC[5] &= 0xFC; // Ensure unicast
  }
  
  BLEAddress addr(randomMAC);
  BLEDevice::init(""); // Empty name
  BLEDevice::setOwnAddrType(ESP_BLE_ADDR_TYPE_RANDOM);
  
  esp_bt_dev_set_device_name("BLE_Jammer");
  
  advParams.adv_type          = ESP_BLE_ADV_TYPE_NONCONN_IND;
  advParams.own_addr_type     = ESP_BLE_ADDR_TYPE_RANDOM;
  advParams.peer_addr_type    = ESP_BLE_ADDR_TYPE_PUBLIC;
  advParams.adv_filter_policy = ESP_BLE_ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
  advParams.adv_int_min       = 0x20;
  advParams.adv_int_max       = 0x40;
  advParams.channel_map       = ESP_BLE_ADV_CHNL_37 | ESP_BLE_ADV_CHNL_38 | ESP_BLE_ADV_CHNL_39;
  
  esp_ble_gap_config_adv_data_raw((uint8_t*)advData, advDataLen);
  esp_ble_gap_start_advertising(&advParams);
}

void startBLEScanFlood() {
  if (!pBLEScan) {
    pBLEScan = BLEDevice::getScan();
    scanCallbacks = new JammerAdvertisedDeviceCallbacks();
    pBLEScan->setAdvertisedDeviceCallbacks(scanCallbacks);
  }
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(0x10);
  pBLEScan->setWindow(0x10);
  pBLEScan->start(0, nullptr, false); // Continuous
}

void stopAllOperations() {
  esp_ble_gap_stop_advertising();
  if (pBLEScan) pBLEScan->stop();
  esp_bt_controller_disable();
  delay(100);
  esp_bt_controller_enable(ESP_BT_MODE_BTDM);
}

// ==================== MAIN SETUP & LOOP ====================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\nBLE JAMMER v1.0 - ESP32-C3");
  
  // Pin setup
  pinMode(SW1_PIN, INPUT_PULLUP);
  pinMode(SW2_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // OFF (active LOW)
  
  // OLED setup
  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.begin();
  u8g2.setContrast(255);
  u8g2.setBusClock(400000); // 400kHz I2C
  
  // Splash animation
  for (int i = 0; i < 20; i++) {
    drawSplashScreen();
    delay(50);
  }
  
  // Init BLE
  BLEDevice::init("BLE_Jammer");
  
  // Show mode
  drawMainScreen();
  animateLED(0);
}

void loop() {
  checkButtons();
  
  if (modeChanged) {
    drawMainScreen();
    modeChanged = false;
  }
  
  if (jammerRunning) {
    switch (currentMode) {
      case MODE_BLE_ADV_FLOOD:
        startBLEAdvertisingFlood();
        packetsSent++;
        delay(50);
        break;
        
      case MODE_BLE_SCAN_FLOOD:
        startBLEScanFlood();
        delay(10);
        break;
        
      case MODE_BLE_CONNECT_FLOOD:
        // Rapid adv restart
        esp_ble_gap_stop_advertising();
        delay(5);
        startBLEAdvertisingFlood();
        packetsSent++;
        delay(30);
        break;
        
      case MODE_BLE_SCAN_AND_ADV:
        startBLEAdvertisingFlood();
        startBLEScanFlood();
        packetsSent++;
        delay(50);
        break;
    }
    
    // Update LED and display
    digitalWrite(LED_PIN, (millis() / 500) % 2 ? LOW : HIGH);
    
    if (millis() - lastStatsUpdate > 200) {
      drawMainScreen();
      lastStatsUpdate = millis();
    }
  } else {
    stopAllOperations();
    digitalWrite(LED_PIN, HIGH);
    if (millis() - lastStatsUpdate > 500) {
      drawMainScreen();
      lastStatsUpdate = millis();
    }
  }
}
