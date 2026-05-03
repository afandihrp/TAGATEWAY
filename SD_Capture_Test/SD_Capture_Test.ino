#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// WiFi Credentials (reused from project)
const char* ssid = "BatuKhan";
const char* password = "momoygemoy";
const char* captureUrl = "http://192.168.11.249/capture";

// Shared 4KB Buffer for both Network and SD parsing
uint8_t sharedBuffer[4096];

// LovyanGFX configuration for ILI9488 + XPT2046 (from ESP32_Controller)
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel_instance;
  lgfx::Bus_SPI       _bus_instance;
  lgfx::Touch_XPT2046 _touch_instance;

public:
  LGFX(void) {
    auto bcfg = _bus_instance.config();
    bcfg.spi_host   = VSPI_HOST;
    bcfg.spi_mode   = 0;
    bcfg.freq_write = 40000000; // 40MHz for stability when sharing bus with SD
    bcfg.freq_read  = 16000000;
    bcfg.pin_sclk   = 18;
    bcfg.pin_mosi   = 23;
    bcfg.pin_miso   = 19;
    bcfg.pin_dc     = 2;
    _bus_instance.config(bcfg);
    _panel_instance.setBus(&_bus_instance);

    auto pcfg = _panel_instance.config();
    pcfg.pin_cs   = 15;
    pcfg.pin_rst  = 4;
    pcfg.pin_busy = -1;
    pcfg.panel_width  = 320;
    pcfg.panel_height = 480;
    pcfg.invert       = false;
    pcfg.rgb_order    = false;
    pcfg.bus_shared   = true;
    _panel_instance.config(pcfg);

    auto tcfg = _touch_instance.config();
    tcfg.x_min = 300;
    tcfg.x_max = 3800;
    tcfg.y_min = 3800;
    tcfg.y_max = 300;
    tcfg.pin_cs     = 21;
    tcfg.pin_int    = 27;
    tcfg.bus_shared = true;
    tcfg.spi_host   = VSPI_HOST;
    tcfg.freq       = 2500000;
    _touch_instance.config(tcfg);
    _panel_instance.setTouch(&_touch_instance);

    setPanel(&_panel_instance);
  }
};

LGFX tft;

// Minimal JPEG size parser to assist with centering
bool getJpgSize(File &file, uint16_t *w, uint16_t *h) {
  Serial.println("[LOG] Parsing JPEG header for dimensions...");
  file.seek(0);
  // Reuse sharedBuffer to read header
  if (file.read(sharedBuffer, 2) != 2 || sharedBuffer[0] != 0xFF || sharedBuffer[1] != 0xD8) {
    Serial.println("[ERR] Invalid JPEG header!");
    return false;
  }
  
  while (file.available()) {
    if (file.read(sharedBuffer, 2) != 2 || sharedBuffer[0] != 0xFF) break;
    uint8_t marker = sharedBuffer[1];
    if (file.read(sharedBuffer, 2) != 2) break;
    uint16_t len = (sharedBuffer[0] << 8) | sharedBuffer[1];
    
    if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) { // Start of Frame
      if (file.read(sharedBuffer, 5) != 5) break;
      // precision is at index 0
      *h = (sharedBuffer[1] << 8) | sharedBuffer[2];
      *w = (sharedBuffer[3] << 8) | sharedBuffer[4];
      Serial.printf("[LOG] Dimensions found: %dx%d\n", *w, *h);
      return true;
    }
    file.seek(file.position() + len - 2);
  }
  Serial.println("[ERR] SOF marker not found in JPEG!");
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n[EVENT] --- SD Capture & View Test Started ---");

  // [0] Ensure all CS pins are HIGH to avoid SPI bus contention during startup
  // Pins: 5 (SD), 15 (TFT), 21 (Touch)
  pinMode(5, OUTPUT);  digitalWrite(5, HIGH); 
  pinMode(15, OUTPUT); digitalWrite(15, HIGH);
  pinMode(21, OUTPUT); digitalWrite(21, HIGH);
  Serial.println("[LOG] CS pins (5, 15, 21) initialized HIGH.");

  // [1] Initialize SPI explicitly with shared pins
  // SCK=18, MISO=19, MOSI=23, CS=5 (SD_CS)
  Serial.println("[LOG] Initializing SPI bus (VSPI)...");
  SPI.begin(18, 19, 23, 5);

  // [2] Initialize SD Card FIRST (GPIO 5)
  Serial.println("[LOG] Initializing SD Card...");
  bool sdStarted = SD.begin(5, SPI);
  if (!sdStarted) {
    Serial.println("[ERR] SD Card Init Failed!");
  } else {
    uint64_t totalSize = SD.totalBytes() / (1024 * 1024);
    uint64_t usedSize = SD.usedBytes() / (1024 * 1024);
    Serial.printf("[LOG] SD Card Detected: %llu MB Total, %llu MB Used\n", totalSize, usedSize);
  }

  // [3] Initialize Display AFTER SD
  Serial.println("[LOG] Initializing Display...");
  tft.init();
  tft.setRotation(1); // Landscape
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("Starting Test...");

  if (!sdStarted) {
    tft.setTextColor(TFT_RED);
    tft.println("SD Init Failed!");
  } else {
    tft.setTextColor(TFT_GREEN);
    tft.printf("SD OK: %llu MB\n", SD.totalBytes() / (1024 * 1024));
  }

  // [4] Connect to WiFi
  Serial.printf("[LOG] Connecting to WiFi: %s\n", ssid);
  tft.setTextColor(TFT_WHITE);
  tft.println("WiFi Connecting...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[LOG] WiFi Connected!");
  Serial.printf("[LOG] IP Address: %s\n", WiFi.localIP().toString().c_str());
  tft.printf("IP: %s\n", WiFi.localIP().toString().c_str());
  tft.println("\nTAP SCREEN TO CAPTURE");
  Serial.println("[EVENT] System Ready. Waiting for touch...");
}

void captureImageToSD() {
  Serial.println("[EVENT] Capture sequence triggered.");
  Serial.printf("[LOG] Target URL: %s\n", captureUrl);
  
  tft.fillScreen(TFT_DARKGREEN);
  tft.setCursor(10, 10);
  tft.println("Capturing...");

  HTTPClient http;
  http.begin(captureUrl);
  http.setTimeout(10000);
  
  Serial.println("[LOG] Sending GET request...");
  int code = http.GET();
  Serial.printf("[LOG] HTTP Response Code: %d\n", code);

  if (code == HTTP_CODE_OK) {
    int totalLen = http.getSize();
    Serial.printf("[LOG] Content Length: %d bytes\n", totalLen);
    WiFiClient* stream = http.getStreamPtr();
    
    Serial.println("[LOG] Opening /capture.jpg for writing...");
    File file = SD.open("/capture.jpg", FILE_WRITE);
    if (!file) {
      Serial.println("[ERR] Failed to open /capture.jpg for writing!");
      return;
    }

    Serial.println("[LOG] Starting download to SD...");
    size_t bytesDownloaded = 0;
    uint32_t lastLogTime = millis();

    while (http.connected() && (totalLen > 0 || totalLen == -1)) {
      size_t avail = stream->available();
      if (avail) {
        // Use the shared 4KB buffer for downloading
        int readNow = stream->readBytes(sharedBuffer, min((size_t)avail, sizeof(sharedBuffer)));
        file.write(sharedBuffer, readNow);
        bytesDownloaded += readNow;
        if (totalLen > 0) totalLen -= readNow;

        // Periodic log every 50KB
        if (millis() - lastLogTime > 1000) {
          Serial.printf("[LOG] Downloaded: %d bytes...\n", bytesDownloaded);
          lastLogTime = millis();
        }
      }
      if (totalLen == 0) break;
      yield();
    }
    file.close();
    Serial.printf("[EVENT] Save complete. Total: %d bytes saved to SD.\n", bytesDownloaded);
  } else {
    Serial.printf("[ERR] HTTP GET failed, error: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

void viewImageFromSD() {
  Serial.println("[EVENT] Viewing sequence started.");
  Serial.println("[LOG] Opening /capture.jpg for reading...");
  File file = SD.open("/capture.jpg", FILE_READ);
  if (!file) {
    Serial.println("[ERR] Open /capture.jpg Failed!");
    return;
  }

  uint16_t imgW = 0, imgH = 0;
  if (!getJpgSize(file, &imgW, &imgH)) {
    Serial.println("[ERR] Could not parse image dimensions.");
    file.close();
    return;
  }
  file.seek(0);

  Serial.println("[LOG] Preparing display area...");
  tft.fillScreen(TFT_BLACK);

  // Calculate scale and offsets for centering
  float scale = 1.0;
  if (imgW > 0 && imgH > 0) {
    float sw = 480.0 / imgW;
    float sh = 320.0 / imgH;
    scale = (sw < sh) ? sw : sh;
  }

  int32_t xOff = (480 - (imgW * scale)) / 2;
  int32_t yOff = (320 - (imgH * scale)) / 2;
  
  Serial.printf("[LOG] Scale: %.2f, OffsetX: %d, OffsetY: %d\n", scale, xOff, yOff);
  Serial.println("[LOG] Starting LovyanGFX drawJpg from SD...");

  // LovyanGFX drawJpg handles file streaming internally in a memory-efficient way.
  uint32_t startTime = millis();
  bool success = tft.drawJpg(&file, xOff, yOff, 480, 320, 0, 0, scale, scale);
  uint32_t renderTime = millis() - startTime;

  file.close();
  
  if (success) {
    Serial.printf("[EVENT] Display complete. Render time: %d ms\n", renderTime);
  } else {
    Serial.println("[ERR] drawJpg failed!");
  }
}

void loop() {
  uint16_t x, y;
  static uint32_t lastDebugLog = 0;

  if (tft.getTouch(&x, &y)) {
    // [DEBUG] Raw touch log every 200ms
    if (millis() - lastDebugLog > 200) {
      Serial.printf("[DEBUG] Raw Touch Detected: x=%d, y=%d\n", x, y);
      lastDebugLog = millis();
    }

    static uint32_t lastTrigger = 0;
    if (millis() - lastTrigger > 3000) {
      Serial.printf("[EVENT] Valid Touch detected at (%d, %d). Triggering pipeline...\n", x, y);
      lastTrigger = millis();
      captureImageToSD();
      viewImageFromSD();
      Serial.println("[EVENT] Pipeline finished. Waiting for next touch...");
    }
  }
  delay(10);
}
