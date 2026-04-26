#include <WiFi.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel_instance;
  lgfx::Bus_SPI       _bus_instance;
public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host   = VSPI_HOST;
      cfg.spi_mode   = 0;
      cfg.freq_write = 75000000;
      cfg.freq_read  = 16000000;
      cfg.pin_sclk   = 18;
      cfg.pin_mosi   = 23;
      cfg.pin_miso   = 19;
      cfg.pin_dc     = 2;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
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
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

LGFX lcd;

const char* ssid       = "BatuKhan";
const char* password   = "momoygemoy";
const char* streamHost = "192.168.11.249";
const int   streamPort = 81;
const char* streamPath = "/stream";

WiFiClient client;

// ── Wait for data with timeout ─────────────────────────────────────────────
bool waitForData(uint32_t timeoutMs = 3000) {
  uint32_t t = millis();
  while (!client.available()) {
    if (millis() - t > timeoutMs) return false;
    delay(1);
  }
  return true;
}

// ── Read one line stripped of \r\n ─────────────────────────────────────────
String readLine() {
  String line = "";
  uint32_t t = millis();
  while (millis() - t < 3000) {
    if (client.available()) {
      char c = client.read();
      if (c == '\n') break;
      if (c != '\r') line += c;
      t = millis();
    }
  }
  return line;
}

// ── Read exactly n bytes ───────────────────────────────────────────────────
bool readExact(uint8_t* buf, size_t len, uint32_t timeoutMs = 8000) {
  size_t got = 0;
  uint32_t t = millis();
  while (got < len) {
    if (millis() - t > timeoutMs) {
      Serial.printf("[ERR] readExact timeout at %d/%d\n", got, len);
      return false;
    }
    if (client.available()) {
      buf[got++] = (uint8_t)client.read();
      t = millis();
    }
  }
  return true;
}

// ── Skip HTTP headers until blank line ────────────────────────────────────
void skipHttpHeaders() {
  while (client.connected()) {
    String line = readLine();
    Serial.println(line);
    if (line.length() == 0) break;
  }
}

// ── Read chunked JPEG — stops at 0xFF 0xD9 end-of-image marker ────────────
// Pattern observed: chunks of 75 + ~3670 + 36 bytes repeat per frame
// The 36-byte chunk (0x24) is the next boundary embedded as a chunk
// So we stop as soon as we see the JPEG EOI marker FF D9
uint8_t* readJpegUntilEOI(size_t& outLen) {
  const size_t MAX_JPEG = 60000;
  uint8_t* buf = (uint8_t*)malloc(MAX_JPEG);
  if (!buf) { Serial.println("[ERR] malloc fail"); return nullptr; }
  outLen = 0;

  while (client.connected()) {
    // Read hex chunk size line
    String sizeLine = readLine();
    sizeLine.trim();
    if (sizeLine.length() == 0) continue;

    size_t chunkSize = strtol(sizeLine.c_str(), nullptr, 16);

    if (chunkSize == 0) {
      readLine(); // consume trailing CRLF of final chunk
      break;
    }

    // Guard against overflow
    if (outLen + chunkSize > MAX_JPEG) {
      Serial.printf("[ERR] overflow: have %d + chunk %d > %d\n", outLen, chunkSize, MAX_JPEG);
      free(buf);
      return nullptr;
    }

    if (!readExact(buf + outLen, chunkSize)) {
      free(buf);
      return nullptr;
    }

    outLen += chunkSize;
    readLine(); // consume CRLF after chunk data

    // ── Check last 2 bytes for JPEG EOI marker 0xFF 0xD9 ──────────────────
    if (outLen >= 2 &&
        buf[outLen - 2] == 0xFF &&
        buf[outLen - 1] == 0xD9) {
      Serial.printf("[EOI] JPEG complete: %d bytes\n", outLen);
      break;
    }
  }

  return buf;
}

void connectToStream() {
  client.stop();
  delay(100);
  lcd.fillScreen(TFT_BLACK);
  lcd.setCursor(10, 10);
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(2);
  lcd.println("Connecting...");

  Serial.println("\nConnecting to stream...");
  if (!client.connect(streamHost, streamPort)) {
    Serial.println("[ERR] Connection failed, retrying in 3s");
    delay(3000);
    return;
  }

  client.printf("GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: keep-alive\r\n\r\n",
                streamPath, streamHost, streamPort);

  if (!waitForData(5000)) {
    Serial.println("[ERR] No response");
    return;
  }

  Serial.println("--- HTTP Headers ---");
  skipHttpHeaders();
  Serial.println("--- End Headers ---");
  Serial.println("[OK] Ready");
  lcd.fillScreen(TFT_BLACK);
}

void loop() {
  if (!client.connected()) {
    connectToStream();
    return;
  }

  if (!waitForData(5000)) {
    Serial.println("[WARN] Timeout, reconnecting...");
    connectToStream();
    return;
  }

  String line = readLine();
  line.trim();
  if (line.length() == 0) return;

  // Handle chunk-wrapped boundary
  if (!line.startsWith("--")) {
    size_t maybeChunkSize = strtol(line.c_str(), nullptr, 16);
    if (maybeChunkSize > 0 && maybeChunkSize < 200) {
      // Likely a chunk header wrapping the boundary line
      line = readLine();
      line.trim();
    }
    if (!line.startsWith("--")) return; // not a boundary, skip
  }

  Serial.println("[BOUNDARY] found");

  // Read and discard part headers (Content-Type: image/jpeg etc.)
  while (client.connected()) {
    String hdr = readLine();
    hdr.trim();
    if (hdr.length() == 0) break;
    Serial.println("[PART HDR] " + hdr);
  }

  // Read JPEG chunks until EOI marker
  size_t jpegLen = 0;
  uint8_t* jpegBuf = readJpegUntilEOI(jpegLen);

// Replace your draw block in loop() with this:

if (jpegBuf && jpegLen > 100) {

  // ── Find actual JPEG start (FF D8) ──────────────────────────────────────
  int jpegStart = -1;
  for (int i = 0; i < (int)jpegLen - 1; i++) {
    if (jpegBuf[i] == 0xFF && jpegBuf[i+1] == 0xD8) {
      jpegStart = i;
      break;
    }
  }

  if (jpegStart < 0) {
    Serial.println("[ERR] No FF D8 found in buffer");
    free(jpegBuf);
    return;
  }

  if (jpegStart > 0) {
    Serial.printf("[INFO] FF D8 at offset %d (skipping %d bytes of header)\n",
                  jpegStart, jpegStart);
  }

  size_t realLen = jpegLen - jpegStart;
  Serial.printf("[DRAW] %d bytes (start offset %d)\n", realLen, jpegStart);

  // Draw centered on 480x320 display
  lcd.drawJpg(jpegBuf + jpegStart, realLen, 0, 0, 480, 320, 0, 0, JPEG_DIV_NONE);

  } else {
    Serial.printf("[WARN] Bad frame: len=%d\n", jpegLen);
  }

  if (jpegBuf) free(jpegBuf);
}

void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.setRotation(1);
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(2);
  lcd.setCursor(10, 10);
  lcd.println("Starting...");

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi: " + WiFi.localIP().toString());

  lcd.fillScreen(TFT_BLACK);
  lcd.setCursor(10, 10);
  lcd.println("WiFi OK");
  lcd.println(WiFi.localIP());
  delay(1000);

  connectToStream();
}