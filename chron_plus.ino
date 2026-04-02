#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <LittleFS.h>

/*
 * PIN CONNECTIONS:
 * ---------------------------------------
 * Component  | Function | NodeMCU | GPIO 
 * ---------------------------------------
 * MFRC522    | SS (SDA) | D8      | 15
 * MFRC522    | RST      | D3      | 0
 * ST7735 TFT | CS       | D2      | 4
 * ST7735 TFT | DC       | D1      | 5
 * ST7735 TFT | RST      | D0      | 16
 * BUZZER      | +        | D4      | 2
 * SHARED SPI | SCK      | D5      | 14
 * SHARED SPI | MOSI     | D7      | 13
 * ---------------------------------------
 */

// RFID Pins
#define RFID_SS_PIN  D8
#define RFID_RST_PIN D3

// TFT Pins
#define TFT_CS   D2
#define TFT_DC   D1
#define TFT_RST  D0

// Buzzer Pin
#define BUZZER_PIN D4

// WiFi Credentials
const char* ssid     = "WHOAMI";
const char* password = "Rafael024!!qwerty";

// Laravel Server Settings (UPDATE PORT IF NEEDED)
const char* serverUrl = "http://10.113.66.235:8000/api/zentrix/test"; 

// NTP Client Settings
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 28800); // UTC+8

MFRC522 mfrc522(RFID_SS_PIN, RFID_RST_PIN);
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

// Function Prototypes (Helps avoid 'not declared in scope' errors)
void playTone(int frequency, int duration);
void updateDisplay(String id);
void saveLogLocally(String tagID);
void syncLogsToServer();
void drawClock();
void drawDeviceInfo();

// Global State
unsigned long lastTimeUpdate = 0;
String currentTagID = "";
unsigned long tagDisplayTime = 0;
bool isConnected = false;
unsigned long lastReconnectAttempt = 0;
const unsigned long RECONNECT_INTERVAL = 30000; 
unsigned long lastSyncAttempt = 0;
const unsigned long SYNC_INTERVAL = 5000; // Sync every 5 seconds

int currentScreen = 0; // 0=Clock, 1=Info
unsigned long lastScreenSwitch = 0;
const unsigned long SCREEN_INTERVAL = 5000; 
String serialNumber = "";

void setup() {
  Serial.begin(115200);
  
  pinMode(RFID_SS_PIN, OUTPUT);
  pinMode(TFT_CS, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(RFID_SS_PIN, HIGH);
  digitalWrite(TFT_CS, HIGH);

  // Initialize LittleFS
  if(!LittleFS.begin()){
    Serial.println("LittleFS Mount Failed");
  } else {
    Serial.println("LittleFS Mounted");
  }

  // Startup Sound
  playTone(2000, 100);
  delay(50);
  playTone(2500, 150);

  SPI.begin();
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);

  // WiFi Connection
  tft.setCursor(10, 50);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.println(F("Connecting WiFi..."));
  
  WiFi.begin(ssid, password);
  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    isConnected = true;
    timeClient.begin();
  } else {
    isConnected = false;
  }

  // Generate Serial Number
  char sn[16];
  sprintf(sn, "ZXP%08X", ESP.getChipId());
  serialNumber = String(sn);

  mfrc522.PCD_Init();
  tft.fillScreen(ST77XX_BLACK);
}

void loop() {
  unsigned long now = millis();

  // Periodical Sync
  if (isConnected && (now - lastSyncAttempt >= SYNC_INTERVAL)) {
    lastSyncAttempt = now;
    syncLogsToServer();
  }

  // Time & Connectivity
  if (isConnected) {
    timeClient.update();
  } else if (now - lastReconnectAttempt >= RECONNECT_INTERVAL) {
    lastReconnectAttempt = now;
    if (WiFi.status() == WL_CONNECTED) {
      isConnected = true;
      timeClient.begin();
    }
  }

  // Dual Screen Logic
  if (now - lastScreenSwitch >= SCREEN_INTERVAL) {
    lastScreenSwitch = now;
    currentScreen = (currentScreen + 1) % 2;
    tft.fillRect(0, 0, 128, 70, ST77XX_BLACK);
  }

  if (currentScreen == 0) {
    if (now - lastTimeUpdate >= 1000) {
      lastTimeUpdate = now;
      drawClock();
    }
  } else {
    drawDeviceInfo();
  }

  // Clear Scan UI after 2 seconds
  if (currentTagID != "" && (now - tagDisplayTime >= 2000)) {
    currentTagID = "";
    tft.fillRect(0, 75, 128, 85, ST77XX_BLACK);
  }

  // RFID Scan Check
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    String tagID = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      if (mfrc522.uid.uidByte[i] < 0x10) tagID += "0";
      tagID += String(mfrc522.uid.uidByte[i], HEX);
    }
    tagID.toUpperCase();

    tagDisplayTime = now;
    updateDisplay(tagID);
    playTone(3000, 150);
    saveLogLocally(tagID);

    mfrc522.PICC_HaltA();
  }
}

void saveLogLocally(String tagID) {
  File logFile = LittleFS.open("/logs.txt", "a");
  if (!logFile) return;
  
  String ts = isConnected ? timeClient.getFormattedTime() : "OFFLINE";
  logFile.println(tagID + "|" + ts);
  logFile.close();
  
  tft.setCursor(15, 130);
  tft.setTextColor(ST77XX_ORANGE);
  tft.setTextSize(1);
  tft.println("DATABASE UPDATED (LOCAL)");
}

void syncLogsToServer() {
  if (!LittleFS.exists("/logs.txt") || WiFi.status() != WL_CONNECTED) return;

  File logFile = LittleFS.open("/logs.txt", "r");
  if (!logFile) return;

  WiFiClient client;
  HTTPClient http;
  http.begin(client, serverUrl);
  http.addHeader("Content-Type", "application/json");

  String jsonPayload = "[";
  bool first = true;
  while (logFile.available()) {
    String line = logFile.readStringUntil('\n');
    line.trim();
    if (line.indexOf('|') == -1) continue;
    
    if (!first) jsonPayload += ",";
    String tid = line.substring(0, line.indexOf('|'));
    String ts = line.substring(line.indexOf('|') + 1);
    jsonPayload += "{\"tag_id\":\"" + tid + "\",\"serial_number\":\"" + serialNumber + "\",\"scanned_at\":\"" + ts + "\"}";
    first = false;
  }
  jsonPayload += "]";
  logFile.close();

  if (first) return;

  int code = http.POST(jsonPayload);
  if (code == 201 || code == 200) {
    LittleFS.remove("/logs.txt");
    Serial.println("Batch Sync OK");
  }
  http.end();
}

void drawClock() {
  String dateStr = "CLOCK: OFFLINE";
  String timeStr = timeClient.getFormattedTime();

  if (isConnected) {
    time_t epochTime = timeClient.getEpochTime();
    struct tm *ti = localtime(&epochTime);
    const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    dateStr = String(months[ti->tm_mon]) + " " + String(ti->tm_mday) + ", " + String(ti->tm_year + 1900);
  }

  // Use (Color, BgColor) to prevent ghosting/stockpiling
  tft.setCursor(10, 15);
  tft.setTextColor(isConnected ? ST77XX_CYAN : ST77XX_RED, ST77XX_BLACK);
  tft.setTextSize(2); 
  tft.print(timeStr);

  tft.setCursor(10, 40);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.println(dateStr);

  tft.setCursor(10, 55);
  tft.setTextColor(isConnected ? ST77XX_YELLOW : ST77XX_RED, ST77XX_BLACK);
  tft.println(isConnected ? "SYSTEM ACTIVE  " : "SYSTEM: OFFLINE");
}


void drawDeviceInfo() {
  tft.setCursor(43, 0);
  tft.setTextColor(ST77XX_MAGENTA);
  tft.setTextSize(1);
  tft.println("- NETWORK -");

  tft.setCursor(5, 15);
  tft.setTextColor(ST77XX_ORANGE);
  tft.println("MODE: WAN");
  
  tft.setCursor(5, 28);
  tft.setTextColor(ST77XX_WHITE);
  tft.print("SN  : "); tft.println(serialNumber);
  
  tft.setCursor(5, 41);
  tft.setTextColor(ST77XX_CYAN);
  tft.print("DEV : "); tft.println("ZentrixPlus");
  
  tft.setCursor(5, 54);
  tft.setTextColor(ST77XX_WHITE);
  tft.print("WIFI: ");
  tft.println(isConnected ? WiFi.localIP().toString() : "N/A");
}

void updateDisplay(String id) {
  currentTagID = id;
  tft.fillRect(0, 75, 128, 85, ST77XX_BLACK);
  tft.setCursor(15, 80);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setTextSize(1);
  tft.println("SCAN DETECTED");
  tft.setCursor(15, 95);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.println(id);
}

void playTone(int frequency, int duration) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(duration);
  digitalWrite(BUZZER_PIN, LOW);
}
