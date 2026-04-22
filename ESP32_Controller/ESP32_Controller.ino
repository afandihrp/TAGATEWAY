#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// WiFi Configuration
const char* ssid = "BatuKhan";
const char* password = "momoygemoy";

// Camera Server Address
const char* cameraServerUrl = "http://192.168.11.249";

// Display Configuration
static const uint32_t screenWidth  = 480; // Landscape
static const uint32_t screenHeight = 320;

// LovyanGFX ILI9488 Display Configuration
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel_instance;
  lgfx::Bus_SPI       _bus_instance;

public:
  LGFX(void) {
    auto bcfg = _bus_instance.config();
    bcfg.spi_host   = VSPI_HOST;
    bcfg.spi_mode   = 0;
    bcfg.freq_write = 27000000;
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

    setPanel(&_panel_instance);
  }
};

LGFX tft;

// LVGL Display Buffers
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf = nullptr;

// LVGL Widgets
lv_obj_t * label_status;
lv_obj_t * label_ram;

HTTPClient http;
WebServer server(80);

// Image buffer management
uint8_t* imageBuffer = nullptr;
size_t imageBufferSize = 0;
// Reduced for standard ESP32 internal RAM (Internal is ~320KB total)
const size_t MAX_IMAGE_SIZE = 128 * 1024; 

// Function declarations
void connectToWiFi();
void controlCamera(const char* var, int val);
void setXCLK(int xclk);
void initPSRAM();
void initDisplay();
void handleRoot();
void handleControl();
void handleXCLK();
void handleCapture();
void handleImage();
void handleStatus();
const char* getHtmlUI();
void freeImageBuffer();
void captureImage();
void displayImageOrText();
void updateRAMUsage();

// LVGL Display Flush Callback
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushPixels((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\nESP32 HTTP Camera Client Starting (LVGL v8)...");
  
  // Initialize Display (LovyanGFX)
  tft.init();
  tft.setRotation(1); // Landscape
  
  // Initialize PSRAM info
  initPSRAM();

  // Initialize LVGL
  lv_init();
  
  // Allocate LVGL draw buffer
  size_t buf_lines = 20; // Reduced lines to save RAM
  size_t buf_size = screenWidth * buf_lines; 
  if (psramFound()) {
    buf = (lv_color_t *)heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    Serial.println("LVGL Buffer allocated in PSRAM");
  } else {
    buf = (lv_color_t *)malloc(buf_size * sizeof(lv_color_t));
    Serial.println("LVGL Buffer allocated in Internal RAM");
  }
  
  if (buf) {
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, buf_size);
  } else {
    Serial.println("FATAL: LVGL Buffer allocation failed!");
  }

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  // Create UI
  lv_obj_t * scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_palette_main(LV_PALETTE_BLUE_GREY), 0);

  label_status = lv_label_create(scr);
  lv_label_set_text(label_status, "Ready...");
  lv_obj_set_style_text_color(label_status, lv_color_white(), 0);
  lv_obj_align(label_status, LV_ALIGN_TOP_LEFT, 10, 5);

  label_ram = lv_label_create(scr);
  lv_label_set_text(label_ram, "RAM: --");
  lv_obj_set_style_text_color(label_ram, lv_color_white(), 0);
  lv_obj_align(label_ram, LV_ALIGN_TOP_RIGHT, -10, 5);

  // Connect to WiFi
  connectToWiFi();
  
  // Setup Web Server routes
  server.on("/", handleRoot);
  server.on("/control", handleControl);
  server.on("/xclk", handleXCLK);
  server.on("/capture", handleCapture);
  server.on("/image", handleImage);
  server.on("/status", handleStatus);
  
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not Found");
  });
  
  server.begin();
  Serial.println("Web server started on port 80");
}

void loop() {
  server.handleClient();
  lv_timer_handler();
  updateRAMUsage();
  delay(5);
}

void updateRAMUsage() {
  static uint32_t last_update = 0;
  if (millis() - last_update > 2000) {
    last_update = millis();
    uint32_t free_h = ESP.getFreeHeap();
    uint32_t total_h = ESP.getHeapSize();
    uint32_t used_h = total_h - free_h;
    lv_label_set_text_fmt(label_ram, "RAM: %u/%u KB", used_h/1024, total_h/1024);
  }
}

void connectToWiFi() {
  Serial.printf("Connecting to WiFi: %s\n", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
    lv_timer_handler();
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi connected!\nIP Address: %s\n", WiFi.localIP().toString().c_str());
    lv_label_set_text_fmt(label_status, "IP: %s", WiFi.localIP().toString().c_str());
  } else {
    lv_label_set_text(label_status, "WiFi Failed!");
  }
}

void initPSRAM() {
  if (psramFound()) {
    Serial.printf("PSRAM found! Total size: %d bytes\n", ESP.getPsramSize());
  } else {
    Serial.println("PSRAM not found. Using Internal RAM.");
  }
}

void controlCamera(const char* var, int val) {
  if (WiFi.status() != WL_CONNECTED) return;
  String url = String(cameraServerUrl) + "/control?var=" + String(var) + "&val=" + String(val);
  http.begin(url);
  http.GET();
  http.end();
}

void setXCLK(int xclk) {
  if (WiFi.status() != WL_CONNECTED) return;
  String url = String(cameraServerUrl) + "/xclk?xclk=" + String(xclk);
  http.begin(url);
  http.GET();
  http.end();
}

void handleStatus() {
  if (WiFi.status() != WL_CONNECTED) {
    server.send(503, "application/json", "{\"error\": \"WiFi disconnected\"}");
    return;
  }
  String url = String(cameraServerUrl) + "/status";
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode == 200) {
    server.send(200, "application/json", http.getString());
  } else {
    server.send(httpCode, "application/json", "{\"error\": \"Status fetch error\"}");
  }
  http.end();
}

void handleRoot() {
  // Use send_P for large flash-based strings
  server.send(200, "text/html", getHtmlUI());
}

void handleControl() {
  if (server.args() < 2) {
    server.send(400, "application/json", "{\"error\": \"Missing params\"}");
    return;
  }
  controlCamera(server.arg("var").c_str(), server.arg("val").toInt());
  server.send(200, "application/json", "{\"status\": \"ok\"}");
}

void handleXCLK() {
  if (!server.hasArg("xclk")) {
    server.send(400, "application/json", "{\"error\": \"Missing xclk\"}");
    return;
  }
  setXCLK(server.arg("xclk").toInt());
  server.send(200, "application/json", "{\"status\": \"ok\"}");
}

void freeImageBuffer() {
  if (imageBuffer != nullptr) {
    heap_caps_free(imageBuffer);
    imageBuffer = nullptr;
    imageBufferSize = 0;
  }
}

void captureImage() {
  if (WiFi.status() != WL_CONNECTED) return;
  freeImageBuffer();
  
  String url = String(cameraServerUrl) + "/capture";
  http.begin(url);
  http.setTimeout(8000);
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    int contentLength = http.getSize();
    WiFiClient* stream = http.getStreamPtr();
    
    // Allocate only if within limits
    if (contentLength > 0 && contentLength <= MAX_IMAGE_SIZE) {
      if (psramFound()) {
        imageBuffer = (uint8_t*)heap_caps_malloc(contentLength, MALLOC_CAP_SPIRAM);
      } else {
        imageBuffer = (uint8_t*)malloc(contentLength);
      }
      
      if (imageBuffer) {
        size_t bytesRead = 0;
        unsigned long start = millis();
        while (http.connected() && bytesRead < contentLength && (millis() - start < 5000)) {
          if (stream->available()) {
            int len = stream->readBytes(imageBuffer + bytesRead, stream->available());
            bytesRead += len;
          }
          delay(1);
        }
        imageBufferSize = bytesRead;
      }
    }
  }
  http.end();
}

void handleCapture() {
  captureImage();
  if (imageBuffer && imageBufferSize > 0) {
    displayImageOrText();
    server.send(200, "application/json", "{\"status\": \"ok\", \"size\": " + String(imageBufferSize) + "}");
  } else {
    server.send(500, "application/json", "{\"error\": \"Capture fail\"}");
  }
}

void handleImage() {
  if (imageBuffer == nullptr || imageBufferSize == 0) {
    server.send(404, "application/json", "{\"error\": \"No image\"}");
    return;
  }
  server.setContentLength(imageBufferSize);
  server.sendHeader("Content-Type", "image/jpeg");
  for (size_t i = 0; i < imageBufferSize; i += 2048) {
    size_t chunkLen = (i + 2048 < imageBufferSize) ? 2048 : (imageBufferSize - i);
    server.sendContent((const char*)(imageBuffer + i), chunkLen);
  }
}

void displayImageOrText() {
  if (imageBuffer && imageBufferSize > 0) {
    tft.drawJpg(imageBuffer, imageBufferSize, 0, 30, screenWidth, screenHeight - 30);
    lv_label_set_text_fmt(label_status, "Captured: %d Bytes", imageBufferSize);
  } else {
    lv_label_set_text(label_status, "Capture Failed!");
  }
}

const char* getHtmlUI() {
  return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Camera</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { font-family: sans-serif; background: #f0f2f5; padding: 15px; }
        .card { background: white; border-radius: 12px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); max-width: 600px; margin: auto; padding: 20px; }
        h1 { text-align: center; color: #1a73e8; margin-bottom: 20px; font-size: 24px; }
        .btn { display: block; width: 100%; background: #1a73e8; color: white; border: none; padding: 12px; border-radius: 8px; font-weight: bold; cursor: pointer; margin-bottom: 20px; }
        .img-box { background: #eee; border-radius: 8px; min-height: 200px; display: flex; align-items: center; justify-content: center; margin-bottom: 20px; overflow: hidden; }
        img { max-width: 100%; height: auto; display: block; }
        .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
        .item { background: #f8f9fa; padding: 10px; border-radius: 8px; border-left: 3px solid #1a73e8; }
        label { display: block; font-size: 12px; color: #666; margin-bottom: 4px; }
        input { width: 100%; padding: 6px; border: 1px solid #ddd; border-radius: 4px; }
        .toggle { display: flex; gap: 4px; }
        .t-btn { flex: 1; font-size: 11px; padding: 6px; border: 1px solid #ddd; border-radius: 4px; background: #fff; cursor: pointer; }
        .t-btn.active { background: #34a853; color: white; border-color: #34a853; }
        #msg { position: fixed; top: 10px; right: 10px; padding: 10px; border-radius: 5px; color: white; display: none; z-index: 100; }
    </style>
</head>
<body>
    <div class="card">
        <h1>📷 Camera Control</h1>
        <button class="btn" onclick="cap()">CAPTURE PHOTO</button>
        <div class="img-box" id="view">No image.</div>
        <div class="grid" id="items"></div>
    </div>
    <div id="msg"></div>
    <script>
        const cfg = [
            { id: 'framesize', n: 'Frame Size', t: 'num', min: 0, max: 13 },
            { id: 'quality', n: 'Quality', t: 'range', min: 0, max: 63 },
            { id: 'brightness', n: 'Brightness', t: 'range', min: -2, max: 2 },
            { id: 'contrast', n: 'Contrast', t: 'range', min: -2, max: 2 },
            { id: 'awb', n: 'Auto White Balance', t: 'tog' },
            { id: 'aec', n: 'Auto Exposure', t: 'tog' },
            { id: 'agc', n: 'Auto Gain', t: 'tog' },
            { id: 'hmirror', n: 'Mirror', t: 'tog' },
            { id: 'vflip', n: 'Flip', t: 'tog' }
        ];
        function init() {
            const container = document.getElementById('items');
            cfg.forEach(i => {
                const div = document.createElement('div');
                div.className = 'item';
                div.innerHTML = `<label>${i.n}</label>`;
                if (i.t === 'tog') {
                    div.innerHTML += `<div class="toggle" id="g-${i.id}"><button class="t-btn" onclick="set('${i.id}',1)">ON</button><button class="t-btn" onclick="set('${i.id}',0)">OFF</button></div>`;
                } else {
                    div.innerHTML += `<input type="${i.t==='range'?'range':'number'}" id="${i.id}" min="${i.min}" max="${i.max}" onchange="send('${i.id}')">`;
                }
                container.appendChild(div);
            });
            fetch('/status').then(r => r.json()).then(d => {
                cfg.forEach(i => {
                    const el = document.getElementById(i.id);
                    if (el) el.value = d[i.id];
                    if (i.t === 'tog') upd(i.id, d[i.id]);
                });
            });
        }
        function upd(id, v) {
            const bs = document.getElementById('g-'+id).querySelectorAll('button');
            bs[0].className = v == 1 ? 't-btn active' : 't-btn';
            bs[1].className = v == 0 ? 't-btn active' : 't-btn';
        }
        function send(id) {
            const v = document.getElementById(id).value;
            fetch(`/control?var=${id}&val=${v}`).then(ok);
        }
        function set(id, v) {
            fetch(`/control?var=${id}&val=${v}`).then(() => { upd(id, v); ok(); });
        }
        function cap() {
            fetch('/capture').then(r => r.json()).then(d => {
                if (d.status === 'ok') {
                    document.getElementById('view').innerHTML = `<img src="/image?t=${Date.now()}">`;
                    ok();
                }
            });
        }
        function ok() {
            const m = document.getElementById('msg');
            m.innerText = 'Updated!'; m.style.background = '#34a853'; m.style.display = 'block';
            setTimeout(() => m.style.display = 'none', 2000);
        }
        window.onload = init;
    </script>
</body>
</html>
)rawliteral";
}
