#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <esp_heap_caps.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// WiFi Configuration
const char* ssid = "BatuKhan";
const char* password = "momoygemoy";

// Camera Server Address
const char* cameraServerUrl = "http://192.168.11.100";

// LovyanGFX ILI9488 Display Configuration
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel_instance;
  lgfx::Bus_SPI       _bus_instance;

public:
  LGFX(void) {
    auto bcfg = _bus_instance.config();
    bcfg.spi_host   = SPI2_HOST;
    bcfg.spi_mode   = 0;
    bcfg.freq_write = 27000000;
    bcfg.freq_read  = 16000000;
    bcfg.pin_sclk   = 12;
    bcfg.pin_mosi   = 11;
    bcfg.pin_miso   = 13;
    bcfg.pin_dc     =  9;
    _bus_instance.config(bcfg);
    _panel_instance.setBus(&_bus_instance);

    auto pcfg = _panel_instance.config();
    pcfg.pin_cs   = 10;
    pcfg.pin_rst  = 14;
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

HTTPClient http;
WebServer server(80);

// Image buffer management (PSRAM)
uint8_t* imageBuffer = nullptr;
size_t imageBufferSize = 0;
const size_t MAX_IMAGE_SIZE = 512 * 1024; // 512KB max for JPEG

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
String getHtmlUI();
void freeImageBuffer();
void captureImage();
void displayImageOrText();

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\nESP32-S3 HTTP Camera Client Starting...");
  
  // Initialize Display
  initDisplay();
  
  // Initialize PSRAM
  initPSRAM();
  
  // Connect to WiFi
  connectToWiFi();
  
  // Setup Web Server routes
  server.on("/", handleRoot);
  server.on("/control", handleControl);
  server.on("/xclk", handleXCLK);
  server.on("/capture", handleCapture);
  server.on("/image", handleImage);
  
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not Found");
  });
  
  server.begin();
  Serial.println("Web server started on port 80");
  
  // Set initial camera settings
  delay(1000);
  controlCamera("framesize", 1);      // Set frame size
  controlCamera("quality", 8);        // Set quality
  controlCamera("contrast", 0);       // Set contrast
  controlCamera("brightness", 0);     // Set brightness
  controlCamera("saturation", 0);     // Set saturation
  controlCamera("gainceiling", 0);    // Set gain ceiling
  controlCamera("colorbar", 0);       // Disable color bar
  controlCamera("awb", 1);            // Enable auto white balance
  controlCamera("agc", 1);            // Enable auto gain control
  controlCamera("aec", 1);            // Enable auto exposure
  controlCamera("hmirror", 0);        // Disable horizontal mirror
  controlCamera("vflip", 0);          // Disable vertical flip
  controlCamera("awb_gain", 1);       // Enable AWB gain
  controlCamera("agc_gain", 0);       // Set AGC gain
  controlCamera("aec_value", 204);    // Set AEC value
  controlCamera("aec2", 0);           // Disable AEC2
  controlCamera("dcw", 1);            // Enable DCW
  controlCamera("bpc", 0);            // Disable BPC
  controlCamera("wpc", 1);            // Enable WPC
  controlCamera("raw_gma", 1);        // Enable raw GMA
  controlCamera("lenc", 1);           // Enable lens correction
  controlCamera("special_effect", 0); // No special effect
  controlCamera("wb_mode", 0);        // Auto white balance mode
  controlCamera("ae_level", 0);       // Auto exposure level
  controlCamera("led_intensity", 0);  // LED intensity
  
  // Set XCLK frequency
  setXCLK(21);
  
  Serial.println("\nInitial camera settings configured!");
}

void loop() {
  server.handleClient();
  delay(10);
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
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi connected!\nIP Address: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nFailed to connect to WiFi");
  }
}

void initPSRAM() {
  Serial.println("Initializing PSRAM...");
  
  if (psramFound()) {
    Serial.printf("PSRAM found! Total size: %d bytes\n", ESP.getPsramSize());
    Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
  } else {
    Serial.println("WARNING: PSRAM not found!");
    Serial.println("Will use regular heap memory for image buffer.");
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  }
}

void initDisplay() {
  Serial.println("Initializing ILI9488 Display...");
  
  tft.init();
  tft.setRotation(1); // Landscape mode
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 150);
  tft.println("ESP32-S3 Camera");
  tft.setCursor(10, 190);
  tft.println("Waiting for image...");
  
  Serial.println("Display initialized successfully!");
}

void controlCamera(const char* var, int val) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected!");
    return;
  }
  
  String url = String(cameraServerUrl) + "/control?var=" + String(var) + "&val=" + String(val);
  
  Serial.printf("Sending: %s\n", url.c_str());
  
  http.begin(url);
  int httpCode = http.GET();
  
  if (httpCode > 0) {
    String payload = http.getString();
    Serial.printf("Response code: %d\n", httpCode);
    Serial.printf("Response: %s\n\n", payload.c_str());
  } else {
    Serial.printf("Error: %s\n\n", http.errorToString(httpCode).c_str());
  }
  
  http.end();
}

void setXCLK(int xclk) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected!");
    return;
  }
  
  String url = String(cameraServerUrl) + "/xclk?xclk=" + String(xclk);
  
  Serial.printf("Sending: %s\n", url.c_str());
  
  http.begin(url);
  int httpCode = http.GET();
  
  if (httpCode > 0) {
    String payload = http.getString();
    Serial.printf("Response code: %d\n", httpCode);
    Serial.printf("Response: %s\n\n", payload.c_str());
  } else {
    Serial.printf("Error: %s\n\n", http.errorToString(httpCode).c_str());
  }
  
  http.end();
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", getHtmlUI());
}

void handleControl() {
  if (server.args() < 2) {
    server.send(400, "application/json", "{\"error\": \"Missing parameters: var and val\"}");
    return;
  }
  
  String var = server.arg("var");
  String val_str = server.arg("val");
  int val = val_str.toInt();
  
  controlCamera(var.c_str(), val);
  server.send(200, "application/json", "{\"status\": \"ok\", \"var\": \"" + var + "\", \"val\": " + String(val) + "}");
}

void handleXCLK() {
  if (!server.hasArg("xclk")) {
    server.send(400, "application/json", "{\"error\": \"Missing xclk parameter\"}");
    return;
  }
  
  int xclk = server.arg("xclk").toInt();
  if (xclk < 5 || xclk > 40) {
    server.send(400, "application/json", "{\"error\": \"xclk must be between 5 and 40 MHz\"}");
    return;
  }
  
  setXCLK(xclk);
  server.send(200, "application/json", "{\"status\": \"ok\", \"xclk\": " + String(xclk) + "}");
}

void freeImageBuffer() {
  if (imageBuffer != nullptr) {
    heap_caps_free(imageBuffer);
    imageBuffer = nullptr;
    imageBufferSize = 0;
    Serial.println("Image buffer freed from PSRAM");
  }
}

void captureImage() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected!");
    return;
  }
  
  Serial.println("Capturing image from camera server...");
  freeImageBuffer(); // Free any existing buffer
  
  // Try multiple endpoints to find JPEG
  const char* endpoints[] = {"/capture"};//{"/capture", "/jpg", "/stream", "/photo"};
  String url;
  int httpCode = -1;
  
  for (int i = 0; i < 4; i++) {
    url = String(cameraServerUrl) + endpoints[i];
    Serial.printf("Trying endpoint: %s\n", url.c_str());
    
    http.begin(url);
    http.setTimeout(10000);
    httpCode = http.GET();
    
    if (httpCode == 200) {
      Serial.printf("Found working endpoint: %s\n", endpoints[i]);
      break;
    }
    
    http.end();
    delay(100);
  }
  
  if (httpCode != 200) {
    Serial.printf("Failed to get image from any endpoint. Last HTTP code: %d\n", httpCode);
    http.end();
    return;
  }
  
  int contentLength = http.getSize();
  Serial.printf("Content-Type: %s\n", http.header("Content-Type").c_str());
  Serial.printf("Content-Length: %d\n", contentLength);
  
  // Validate image size (should be at least 100 bytes for valid JPEG, usually > 5KB)
  if (contentLength < 100 || contentLength > MAX_IMAGE_SIZE) {
    Serial.printf("Invalid image size: %d bytes (min: 100, max: %d)\n", contentLength, MAX_IMAGE_SIZE);
    
    // Read a sample to see what we're getting
    WiFiClient* stream = http.getStreamPtr();
    uint8_t buffer[256] = {0};
    int readLen = stream->readBytes(buffer, 256);
    Serial.printf("Response preview: ");
    for (int i = 0; i < readLen && i < 50; i++) {
      Serial.printf("%02X ", buffer[i]);
    }
    Serial.println();
    
    http.end();
    return;
  }
  
  // Check for JPEG magic bytes (FF D8)
  WiFiClient* stream = http.getStreamPtr();
  uint8_t byte1 = stream->read();
  uint8_t byte2 = stream->read();
  
  if (byte1 != 0xFF || byte2 != 0xD8) {
    Serial.printf("Not a valid JPEG! Magic bytes: %02X %02X (expected FF D8)\n", byte1, byte2);
    http.end();
    return;
  }
  Serial.println("Valid JPEG header detected!");
  
  // Allocate buffer in PSRAM (or regular heap as fallback)
  Serial.printf("Allocating %d bytes for image\n", contentLength);
  
  if (psramFound()) {
    imageBuffer = (uint8_t*)heap_caps_malloc(contentLength, MALLOC_CAP_SPIRAM);
    if (imageBuffer != nullptr) {
      Serial.println("Using PSRAM for image buffer");
    } else {
      Serial.println("PSRAM allocation failed, trying regular heap...");
      imageBuffer = (uint8_t*)malloc(contentLength);
      if (imageBuffer != nullptr) {
        Serial.println("Using regular heap for image buffer");
      }
    }
  } else {
    Serial.println("PSRAM not available, using regular heap");
    imageBuffer = (uint8_t*)malloc(contentLength);
  }
  
  if (imageBuffer == nullptr) {
    Serial.printf("Failed to allocate %d bytes\n", contentLength);
    if (psramFound()) {
      Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
    }
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
    http.end();
    return;
  }
  
  // Copy already-read bytes
  imageBuffer[0] = byte1;
  imageBuffer[1] = byte2;
  size_t bytesRead = 2;
  
  // Download remaining image data
  while (http.connected() && bytesRead < contentLength) {
    size_t available = stream->available();
    if (available) {
      int len = stream->readBytes(imageBuffer + bytesRead, available);
      bytesRead += len;
      
      // Print progress every 64KB
      if (bytesRead % 65536 == 0 || bytesRead == contentLength) {
        Serial.printf("Downloaded: %d / %d bytes\n", bytesRead, contentLength);
      }
    }
    delay(1);
  }
  
  imageBufferSize = bytesRead;
  
  if (bytesRead == contentLength) {
    Serial.printf("Image captured successfully! Size: %d bytes\n", imageBufferSize);
    if (psramFound()) {
      Serial.printf("Free PSRAM after capture: %d bytes\n", ESP.getFreePsram());
    }
    Serial.printf("Free heap after capture: %d bytes\n", ESP.getFreeHeap());
  } else {
    Serial.printf("Download incomplete: %d / %d bytes\n", bytesRead, contentLength);
    freeImageBuffer();
  }
  
  http.end();
}

void handleCapture() {
  Serial.println("Capture request received");
  captureImage();
  
  if (imageBuffer != nullptr && imageBufferSize > 0) {
    displayImageOrText(); // Update display with new image
    server.send(200, "application/json", 
      "{\"status\": \"ok\", \"size\": " + String(imageBufferSize) + ", \"url\": \"/image\"}");
  } else {
    server.send(500, "application/json", "{\"error\": \"Failed to capture image\"}");
  }
}

void handleImage() {
  if (imageBuffer == nullptr || imageBufferSize == 0) {
    server.send(404, "application/json", "{\"error\": \"No image available\"}");
    return;
  }
  
  Serial.printf("Sending image (%d bytes) to client\n", imageBufferSize);
  server.setContentLength(imageBufferSize);
  server.sendHeader("Content-Type", "image/jpeg");
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  
  // Use sendContent for proper JPEG streaming
  for (size_t i = 0; i < imageBufferSize; i += 4096) {
    size_t chunkLen = (i + 4096 < imageBufferSize) ? 4096 : (imageBufferSize - i);
    server.sendContent((const char*)(imageBuffer + i), chunkLen);
  }
}

void displayImageOrText() {
  if (imageBuffer == nullptr || imageBufferSize == 0) {
    // No image - display text
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(2);
    tft.setCursor(10, 80);
    tft.println("ESP32-S3 Camera");
    tft.setCursor(10, 120);
    tft.println("No image captured");
    tft.setCursor(10, 160);
    tft.println("Click Capture JPEG");
    tft.setCursor(10, 200);
    tft.println("to get started");
  } else {
    // Image exists - try to draw it
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    tft.setCursor(10, 10);
    tft.printf("Image: %d bytes", imageBufferSize);
    
    Serial.printf("Drawing JPEG image (%d bytes) to TFT display\n", imageBufferSize);
    
    // Try to draw JPEG using drawJpg method
    // drawJpg(uint8_t *jpg_buf, size_t jpg_size, int x, int y, int maxWidth, int maxHeight)
    if (tft.drawJpg(imageBuffer, imageBufferSize, 0, 30, 320, 450)) {
      Serial.println("JPEG displayed successfully on TFT");
    } else {
      Serial.println("Failed to draw JPEG, showing placeholder");
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_WHITE);
      tft.setTextSize(2);
      tft.setCursor(10, 150);
      tft.println("Image captured");
      tft.setCursor(10, 190);
      tft.printf("Size: %d bytes", imageBufferSize);
    }
  }
}

String getHtmlUI() {
  return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32-S3 Camera Controller</title>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
            padding: 20px;
        }
        .container {
            background: white;
            border-radius: 20px;
            box-shadow: 0 20px 60px rgba(0,0,0,0.3);
            max-width: 900px;
            width: 100%;
            padding: 40px;
        }
        h1 {
            color: #333;
            text-align: center;
            margin-bottom: 30px;
            font-size: 28px;
        }
        .capture-section {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 30px;
            border-radius: 12px;
            margin-bottom: 30px;
            text-align: center;
        }
        .capture-section h2 {
            margin-bottom: 20px;
            font-size: 20px;
        }
        .capture-btn {
            background: white;
            color: #667eea;
            border: none;
            padding: 15px 40px;
            border-radius: 8px;
            cursor: pointer;
            font-weight: 700;
            font-size: 16px;
            transition: all 0.3s ease;
            margin: 10px;
        }
        .capture-btn:hover {
            transform: scale(1.05);
            box-shadow: 0 4px 12px rgba(0,0,0,0.2);
        }
        .capture-btn:active {
            transform: scale(0.98);
        }
        .image-display {
            background: #f0f0f0;
            border-radius: 12px;
            padding: 20px;
            margin-bottom: 30px;
            text-align: center;
            min-height: 300px;
            display: flex;
            align-items: center;
            justify-content: center;
        }
        .image-display img {
            max-width: 100%;
            max-height: 600px;
            border-radius: 8px;
            box-shadow: 0 4px 12px rgba(0,0,0,0.2);
        }
        .image-placeholder {
            color: #999;
            font-size: 16px;
        }
        .controls-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
            gap: 20px;
            margin-bottom: 30px;
        }
        .control-group {
            background: #f8f9fa;
            padding: 20px;
            border-radius: 12px;
            border-left: 4px solid #667eea;
        }
        .control-group label {
            display: block;
            color: #333;
            font-weight: 600;
            margin-bottom: 8px;
            font-size: 14px;
        }
        .control-group .description {
            color: #999;
            font-size: 11px;
            margin-bottom: 10px;
            min-height: 20px;
        }
        .input-group {
            display: flex;
            gap: 8px;
            align-items: center;
        }
        .input-group input {
            flex: 1;
            padding: 8px 12px;
            border: 1px solid #ddd;
            border-radius: 6px;
            font-size: 14px;
        }
        .input-group input[type="range"] {
            padding: 0;
        }
        .slider-value {
            min-width: 50px;
            text-align: center;
            font-weight: bold;
            color: #667eea;
        }
        button {
            background: #667eea;
            color: white;
            border: none;
            padding: 10px 20px;
            border-radius: 6px;
            cursor: pointer;
            font-weight: 600;
            transition: all 0.3s ease;
            font-size: 14px;
        }
        button:hover {
            background: #764ba2;
            transform: translateY(-2px);
            box-shadow: 0 4px 12px rgba(102, 126, 234, 0.4);
        }
        button:active {
            transform: translateY(0);
        }
        .toggle-group {
            display: flex;
            gap: 8px;
        }
        .toggle-btn {
            flex: 1;
            padding: 8px 12px;
            font-size: 12px;
            background: #ddd;
            color: #333;
        }
        .toggle-btn.active {
            background: #27ae60;
            color: white;
        }
        .section {
            margin-bottom: 30px;
        }
        .section-title {
            font-size: 16px;
            font-weight: 700;
            color: #333;
            margin-bottom: 15px;
            padding-bottom: 10px;
            border-bottom: 2px solid #667eea;
        }
        .response {
            background: #e8f5e9;
            border-left: 4px solid #27ae60;
            padding: 15px;
            border-radius: 6px;
            margin-top: 20px;
            font-size: 14px;
            color: #27ae60;
            display: none;
        }
        .response.error {
            background: #ffebee;
            border-left-color: #e53935;
            color: #e53935;
        }
        .loading {
            display: none;
            text-align: center;
            color: #667eea;
            font-weight: 600;
            margin: 10px 0;
        }
        .spinner {
            border: 4px solid #f3f3f3;
            border-top: 4px solid #667eea;
            border-radius: 50%;
            width: 30px;
            height: 30px;
            animation: spin 1s linear infinite;
            margin: 10px auto;
        }
        @keyframes spin {
            0% { transform: rotate(0deg); }
            100% { transform: rotate(360deg); }
        }
        @media (max-width: 600px) {
            .container {
                padding: 20px;
            }
            h1 {
                font-size: 22px;
            }
            .controls-grid {
                grid-template-columns: 1fr;
            }
            .image-display {
                min-height: 200px;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>📷 Camera Controller</h1>
        
        <div class="capture-section">
            <h2>Capture Image</h2>
            <button class="capture-btn" onclick="captureImage()">📸 Capture JPEG</button>
            <div class="loading" id="loading">
                <div class="spinner"></div>
                <p>Capturing image...</p>
            </div>
        </div>

        <div class="image-display" id="imageDisplay">
            <div class="image-placeholder">No image captured yet. Click "Capture JPEG" to start.</div>
        </div>

        <div class="section">
            <div class="section-title">Frame & Quality</div>
            <div class="controls-grid">
                <div class="control-group">
                    <label>Frame Size</label>
                    <div class="description">0-13</div>
                    <div class="input-group">
                        <input type="number" id="framesize" min="0" max="13" value="1">
                        <button onclick="sendControl('framesize')">Set</button>
                    </div>
                </div>
                
                <div class="control-group">
                    <label>Quality</label>
                    <div class="description">0-63</div>
                    <div class="input-group">
                        <input type="range" id="quality" min="0" max="63" value="8" 
                               oninput="document.getElementById('quality-val').textContent = this.value">
                        <span class="slider-value"><span id="quality-val">8</span></span>
                        <button onclick="sendControl('quality')">Set</button>
                    </div>
                </div>
            </div>
        </div>

        <div class="section">
            <div class="section-title">Image Adjustments</div>
            <div class="controls-grid">
                <div class="control-group">
                    <label>Contrast</label>
                    <div class="input-group">
                        <input type="range" id="contrast" min="-2" max="2" value="0"
                               oninput="document.getElementById('contrast-val').textContent = this.value">
                        <span class="slider-value"><span id="contrast-val">0</span></span>
                        <button onclick="sendControl('contrast')">Set</button>
                    </div>
                </div>
                
                <div class="control-group">
                    <label>Brightness</label>
                    <div class="input-group">
                        <input type="range" id="brightness" min="-2" max="2" value="0"
                               oninput="document.getElementById('brightness-val').textContent = this.value">
                        <span class="slider-value"><span id="brightness-val">0</span></span>
                        <button onclick="sendControl('brightness')">Set</button>
                    </div>
                </div>
                
                <div class="control-group">
                    <label>Saturation</label>
                    <div class="input-group">
                        <input type="range" id="saturation" min="-2" max="2" value="0"
                               oninput="document.getElementById('saturation-val').textContent = this.value">
                        <span class="slider-value"><span id="saturation-val">0</span></span>
                        <button onclick="sendControl('saturation')">Set</button>
                    </div>
                </div>
                
                <div class="control-group">
                    <label>Gain Ceiling</label>
                    <div class="input-group">
                        <input type="range" id="gainceiling" min="0" max="6" value="0"
                               oninput="document.getElementById('gainceiling-val').textContent = this.value">
                        <span class="slider-value"><span id="gainceiling-val">0</span></span>
                        <button onclick="sendControl('gainceiling')">Set</button>
                    </div>
                </div>
            </div>
        </div>

        <div class="section">
            <div class="section-title">Auto Controls</div>
            <div class="controls-grid">
                <div class="control-group">
                    <label>AWB</label>
                    <div class="toggle-group">
                        <button class="toggle-btn active" onclick="setToggle('awb', 1, this)">ON</button>
                        <button class="toggle-btn" onclick="setToggle('awb', 0, this)">OFF</button>
                    </div>
                </div>
                
                <div class="control-group">
                    <label>AGC</label>
                    <div class="toggle-group">
                        <button class="toggle-btn active" onclick="setToggle('agc', 1, this)">ON</button>
                        <button class="toggle-btn" onclick="setToggle('agc', 0, this)">OFF</button>
                    </div>
                </div>
                
                <div class="control-group">
                    <label>AEC</label>
                    <div class="toggle-group">
                        <button class="toggle-btn active" onclick="setToggle('aec', 1, this)">ON</button>
                        <button class="toggle-btn" onclick="setToggle('aec', 0, this)">OFF</button>
                    </div>
                </div>

                <div class="control-group">
                    <label>H-Mirror</label>
                    <div class="toggle-group">
                        <button class="toggle-btn" onclick="setToggle('hmirror', 1, this)">ON</button>
                        <button class="toggle-btn active" onclick="setToggle('hmirror', 0, this)">OFF</button>
                    </div>
                </div>
                
                <div class="control-group">
                    <label>V-Flip</label>
                    <div class="toggle-group">
                        <button class="toggle-btn" onclick="setToggle('vflip', 1, this)">ON</button>
                        <button class="toggle-btn active" onclick="setToggle('vflip', 0, this)">OFF</button>
                    </div>
                </div>
            </div>
        </div>

        <div class="section">
            <div class="section-title">Advanced Settings</div>
            <div class="controls-grid">
                <div class="control-group">
                    <label>AWB Gain</label>
                    <div class="toggle-group">
                        <button class="toggle-btn active" onclick="setToggle('awb_gain', 1, this)">ON</button>
                        <button class="toggle-btn" onclick="setToggle('awb_gain', 0, this)">OFF</button>
                    </div>
                </div>

                <div class="control-group">
                    <label>AGC Gain</label>
                    <div class="description">0-30</div>
                    <div class="input-group">
                        <input type="number" id="agc_gain" min="0" max="30" value="0">
                        <button onclick="sendControl('agc_gain')">Set</button>
                    </div>
                </div>

                <div class="control-group">
                    <label>AEC Value</label>
                    <div class="description">0-1200</div>
                    <div class="input-group">
                        <input type="number" id="aec_value" min="0" max="1200" value="204">
                        <button onclick="sendControl('aec_value')">Set</button>
                    </div>
                </div>

                <div class="control-group">
                    <label>Special Effect</label>
                    <div class="description">0-6</div>
                    <div class="input-group">
                        <input type="number" id="special_effect" min="0" max="6" value="0">
                        <button onclick="sendControl('special_effect')">Set</button>
                    </div>
                </div>

                <div class="control-group">
                    <label>WB Mode</label>
                    <div class="description">0-4</div>
                    <div class="input-group">
                        <input type="number" id="wb_mode" min="0" max="4" value="0">
                        <button onclick="sendControl('wb_mode')">Set</button>
                    </div>
                </div>

                <div class="control-group">
                    <label>LED Intensity</label>
                    <div class="description">0-255</div>
                    <div class="input-group">
                        <input type="range" id="led_intensity" min="0" max="255" value="0"
                               oninput="document.getElementById('led_intensity-val').textContent = this.value">
                        <span class="slider-value"><span id="led_intensity-val">0</span></span>
                        <button onclick="sendControl('led_intensity')">Set</button>
                    </div>
                </div>
            </div>
        </div>

        <div class="section">
            <div class="section-title">Clock Settings</div>
            <div class="control-group">
                <label>XCLK Frequency (MHz)</label>
                <div class="description">5-40 MHz</div>
                <div class="input-group">
                    <input type="number" id="xclk" min="5" max="40" value="21">
                    <button onclick="sendXCLK()">Set</button>
                </div>
            </div>
        </div>

        <div id="response" class="response"></div>
    </div>

    <script>
        function captureImage() {
            const loading = document.getElementById('loading');
            const imageDisplay = document.getElementById('imageDisplay');
            
            loading.style.display = 'block';
            
            fetch('/capture')
                .then(response => response.json())
                .then(data => {
                    if (data.status === 'ok') {
                        showResponse('✓ Image captured successfully!', false);
                        loadImage();
                    } else {
                        showResponse(`✗ Error: ${data.error}`, true);
                        loading.style.display = 'none';
                    }
                })
                .catch(error => {
                    showResponse(`✗ Capture failed: ${error}`, true);
                    loading.style.display = 'none';
                });
        }

        function loadImage() {
            const imageDisplay = document.getElementById('imageDisplay');
            const loading = document.getElementById('loading');
            
            // Create image element
            const img = new Image();
            img.onload = function() {
                imageDisplay.innerHTML = '';
                imageDisplay.appendChild(img);
                loading.style.display = 'none';
            };
            img.onerror = function() {
                showResponse('✗ Failed to load image', true);
                loading.style.display = 'none';
            };
            
            // Use fetch to get image as blob, then create object URL
            fetch('/image')
                .then(response => {
                    if (!response.ok) throw new Error('Failed to fetch image');
                    return response.blob();
                })
                .then(blob => {
                    const url = URL.createObjectURL(blob);
                    img.src = url;
                })
                .catch(error => {
                    showResponse(`✗ Failed to load image: ${error}`, true);
                    loading.style.display = 'none';
                });
        }

        function sendControl(variable) {
            const element = document.getElementById(variable);
            const value = element ? element.value : 0;
            
            fetch(`/control?var=${variable}&val=${value}`)
                .then(response => response.json())
                .then(data => {
                    showResponse(`✓ ${variable}: ${value}`, false);
                })
                .catch(error => {
                    showResponse(`✗ Error: ${error}`, true);
                });
        }

        function setToggle(variable, value, button) {
            const parent = button.parentElement;
            parent.querySelectorAll('button').forEach(btn => btn.classList.remove('active'));
            button.classList.add('active');
            
            fetch(`/control?var=${variable}&val=${value}`)
                .then(response => response.json())
                .then(data => {
                    showResponse(`✓ ${variable}: ${value ? 'ON' : 'OFF'}`, false);
                })
                .catch(error => {
                    showResponse(`✗ Error: ${error}`, true);
                });
        }

        function sendXCLK() {
            const xclk = document.getElementById('xclk').value;
            
            fetch(`/xclk?xclk=${xclk}`)
                .then(response => response.json())
                .then(data => {
                    showResponse(`✓ XCLK set to ${xclk} MHz`, false);
                })
                .catch(error => {
                    showResponse(`✗ Error: ${error}`, true);
                });
        }

        function showResponse(message, isError) {
            const responseDiv = document.getElementById('response');
            responseDiv.textContent = message;
            responseDiv.style.display = 'block';
            responseDiv.classList.toggle('error', isError);
            
            setTimeout(() => {
                responseDiv.style.display = 'none';
            }, 3000);
        }
    </script>
</body>
</html>
)rawliteral";
}