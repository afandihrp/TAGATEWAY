/*
 * MJPEG Stream Viewer — ESP32 + ILI9488 (LovyanGFX)
 * Displays a live MJPEG stream from an ESP32-CAM over WiFi.
 *
 * Wiring (VSPI):
 *   SCLK → 18 | MOSI → 23 | MISO → 19
 *   DC   →  2 | CS   → 15 | RST  →  4
 */

#include <WiFi.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ============================================================
//  Display Configuration — ILI9488 480×320
// ============================================================
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel;
  lgfx::Bus_SPI       _bus;
public:
  LGFX() {
    {
      auto cfg      = _bus.config();
      cfg.spi_host  = VSPI_HOST;
      cfg.spi_mode  = 0;
      cfg.freq_write = 75000000; // 40 MHz — safe ceiling for ILI9488
      cfg.freq_read  = 16000000;
      cfg.pin_sclk  = 18;
      cfg.pin_mosi  = 23;
      cfg.pin_miso  = 19;
      cfg.pin_dc    = 2;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg             = _panel.config();
      cfg.pin_cs           = 15;
      cfg.pin_rst          = 4;
      cfg.pin_busy         = -1;
      cfg.panel_width      = 320;
      cfg.panel_height     = 480;
      cfg.offset_rotation  = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = true;
      cfg.invert           = false;
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = true;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};

// ============================================================
//  Configuration
// ============================================================
static const char* WIFI_SSID   = "BatuKhan";
static const char* WIFI_PASS   = "momoygemoy";
static const char* CAM_HOST    = "192.168.11.249";
static const int   CAM_PORT    = 81;
static const char* CAM_PATH    = "/stream";

// JPEG buffer — increase if your CAM resolution is VGA or higher
static const size_t JPEG_BUF_SIZE = 80000;

// ============================================================
//  Globals
// ============================================================
LGFX        lcd;
WiFiClient  client;
uint8_t*    jpegBuf = nullptr;

// FPS tracking
uint32_t    frameCount = 0;
uint32_t    fpsTimer   = 0;

// ============================================================
//  Helpers
// ============================================================

// Read one \n-terminated line, strip \r
String readLine(uint32_t timeoutMs = 2000) {
  String s;
  s.reserve(64);
  uint32_t deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    if (client.available()) {
      char c = (char)client.read();
      if (c == '\n') break;
      if (c != '\r') s += c;
      deadline = millis() + timeoutMs; // reset on each byte
    }
  }
  return s;
}

// Read exactly `len` bytes — returns false on timeout
bool readExact(uint8_t* dst, size_t len, uint32_t timeoutMs = 5000) {
  size_t got = 0;
  uint32_t deadline = millis() + timeoutMs;
  while (got < len && millis() < deadline) {
    int avail = client.available();
    if (avail > 0) {
      size_t take = min((size_t)avail, len - got);
      client.readBytes(dst + got, take); // bulk read — faster than byte loop
      got += take;
      deadline = millis() + timeoutMs;
    }
  }
  return got == len;
}

// Consume HTTP or MJPEG part headers until a blank line
void skipHeaders() {
  while (client.connected()) {
    if (readLine().length() == 0) break;
  }
}

// ============================================================
//  Connect & send HTTP GET
// ============================================================
bool connectToStream() {
  client.stop();

  lcd.fillScreen(TFT_BLACK);
  lcd.setCursor(8, 8);
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(2);
  lcd.print("Connecting to cam...");

  if (!client.connect(CAM_HOST, CAM_PORT)) {
    Serial.println("[ERR] TCP connect failed");
    return false;
  }

  // Disable Nagle — reduces latency on small packets
  client.setNoDelay(true);

  client.printf(
    "GET %s HTTP/1.1\r\n"
    "Host: %s:%d\r\n"
    "Connection: keep-alive\r\n"
    "\r\n",
    CAM_PATH, CAM_HOST, CAM_PORT);

  // Wait for first byte of response
  uint32_t t = millis();
  while (!client.available()) {
    if (millis() - t > 5000) { Serial.println("[ERR] No response"); return false; }
    delay(1);
  }

  skipHeaders(); // skip HTTP/1.1 200 OK … headers

  Serial.println("[OK] Stream connected");
  lcd.fillScreen(TFT_BLACK);
  fpsTimer = millis();
  frameCount = 0;
  return true;
}

// ============================================================
//  Read one MJPEG frame from chunked stream
//  Returns number of bytes written into jpegBuf, 0 on error.
// ============================================================
size_t readFrame() {
  size_t total = 0;

  while (client.connected()) {
    // Each iteration: read one HTTP chunk
    String sizeLine = readLine();
    sizeLine.trim();
    if (sizeLine.length() == 0) continue;

    size_t chunkSize = strtoul(sizeLine.c_str(), nullptr, 16);
    if (chunkSize == 0) { readLine(); break; } // final chunk

    if (total + chunkSize > JPEG_BUF_SIZE) {
      Serial.printf("[ERR] Frame overflow %u + %u\n", total, chunkSize);
      return 0;
    }

    if (!readExact(jpegBuf + total, chunkSize)) {
      Serial.println("[ERR] readExact timeout");
      return 0;
    }

    total += chunkSize;
    readLine(); // consume trailing CRLF after chunk data

    // JPEG EOI = FF D9 → frame complete
    if (total >= 2 &&
        jpegBuf[total - 2] == 0xFF &&
        jpegBuf[total - 1] == 0xD9) {
      break;
    }
  }

  return total;
}

// ============================================================
//  Setup
// ============================================================
void setup() {
  Serial.begin(115200);

  // --- Display init ---
  lcd.init();
  lcd.setRotation(1); // landscape
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(2);
  lcd.setCursor(8, 8);
  lcd.print("Starting...");

  // --- Allocate JPEG buffer once (no repeated malloc/free) ---
  jpegBuf = (uint8_t*)heap_caps_malloc(JPEG_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!jpegBuf) {
    // Fall back to internal RAM if no PSRAM
    jpegBuf = (uint8_t*)malloc(JPEG_BUF_SIZE);
  }
  if (!jpegBuf) {
    Serial.println("[FATAL] Cannot allocate JPEG buffer");
    while (true) delay(1000);
  }

  // --- WiFi ---
  WiFi.setSleep(false); // disable WiFi modem sleep → lower latency
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lcd.setCursor(8, 40);
  lcd.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(250); lcd.print("."); }
  Serial.printf("[WiFi] %s\n", WiFi.localIP().toString().c_str());

  connectToStream();
}

// ============================================================
//  Loop
// ============================================================
void loop() {
  if (!client.connected()) {
    Serial.println("[WARN] Disconnected — reconnecting...");
    delay(1000);
    connectToStream();
    return;
  }

  // ── Find MJPEG boundary ────────────────────────────────────────────────
  String line = readLine();
  line.trim();

  if (line.length() == 0) return;

  // The boundary may arrive wrapped inside its own chunk header
  if (!line.startsWith("--")) {
    // Try interpreting as a hex chunk size; if small, peek at the next line
    unsigned long maybeSize = strtoul(line.c_str(), nullptr, 16);
    if (maybeSize > 0 && maybeSize < 256) {
      line = readLine();
      line.trim();
    }
    if (!line.startsWith("--")) return; // unrecognised — skip
  }

  // ── Skip MJPEG part headers (Content-Type: image/jpeg …) ──────────────
  skipHeaders();

  // ── Read frame ────────────────────────────────────────────────────────
  size_t frameLen = readFrame();
  if (frameLen < 4) return;

  // Find FF D8 (some streams prepend part-header text before the SOI)
  uint8_t* jpegStart = jpegBuf;
  size_t   jpegLen   = frameLen;
  for (size_t i = 0; i < frameLen - 1; i++) {
    if (jpegBuf[i] == 0xFF && jpegBuf[i + 1] == 0xD8) {
      jpegStart = jpegBuf + i;
      jpegLen   = frameLen - i;
      break;
    }
  }

  // ── Draw ──────────────────────────────────────────────────────────────
  lcd.drawJpg(jpegStart, jpegLen, 0, 0, lcd.width(), lcd.height(), 0, 0, JPEG_DIV_NONE);

  // ── FPS counter (Serial only) ─────────────────────────────────────────
  frameCount++;
  uint32_t elapsed = millis() - fpsTimer;
  if (elapsed >= 3000) {
    Serial.printf("[FPS] %.1f  frame=%u bytes\n",
                  frameCount * 1000.0f / elapsed, frameLen);
    frameCount = 0;
    fpsTimer   = millis();
  }
}