#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <time.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// WiFi Configuration (Loaded from SD)
String ssid = "";
String password = "";

// SD Card Configuration (HSPI)
const int sdCS = 5;
SPIClass sdSPI(HSPI);

// Telegram Configuration (Loaded from SD)
String botToken = "";
String targetChatId = "";

// Display Configuration
static const uint32_t screenWidth  = 480; // Landscape
static const uint32_t screenHeight = 320;

// LovyanGFX ILI9488 Display Configuration
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel_instance;
  lgfx::Bus_SPI       _bus_instance;
  lgfx::Touch_XPT2046 _touch_instance;

public:
  LGFX(void) {
    auto bcfg = _bus_instance.config();
    bcfg.spi_host   = VSPI_HOST;
    bcfg.spi_mode   = 0;
    bcfg.freq_write = 75000000; // Lowered for ILI9488 stability
    bcfg.freq_read  = 16000000;
    bcfg.pin_sclk   = 18;
    bcfg.pin_mosi   = 23;
    bcfg.pin_miso   = -1;       // CRITICAL: Disconnect display MISO to fix touch corruption
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
    tcfg.y_min = 3800;   // swapped
    tcfg.y_max = 300;    // swapped
    tcfg.pin_cs     = 21;
    tcfg.pin_int    = 27;
    tcfg.bus_shared = true;
    tcfg.spi_host   = VSPI_HOST;
    tcfg.freq       = 1000000; // Lowered for reliable touch reads
    tcfg.pin_sclk   = 18;      // Explicitly define shared pins
    tcfg.pin_mosi   = 23;
    tcfg.pin_miso   = 19;      // Touch controller DOES need MISO
    _touch_instance.config(tcfg);
    _panel_instance.setTouch(&_touch_instance);

    setPanel(&_panel_instance);
  }
};

LGFX tft;

// LVGL Display Buffers
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf = nullptr;

// Device Info Struct
struct DeviceInfo {
  String mac;
  String ip;
  int rssi;
};

DeviceInfo devices[5] = {
  {"", "", -100},
  {"", "", -100},
  {"", "", -100},
  {"", "", -100},
  {"", "", -100}
};

// LVGL Widgets
lv_obj_t * scr_image;
lv_obj_t * top_panel;
lv_obj_t * scr_config;
lv_obj_t * scr_stats;
lv_obj_t * scr_devices;
lv_obj_t * scr_multi;
lv_obj_t * top_panel_multi;
lv_obj_t * label_status;
lv_obj_t * label_ram;
lv_obj_t * label_ram_multi;
lv_obj_t * label_notify_multi;
lv_obj_t * label_ram_stats;
lv_obj_t * label_ram_devices;
lv_obj_t * sd_bar;
lv_obj_t * sd_val_label;
lv_obj_t * scr_ip_select;
lv_obj_t * btn_select_ip;
lv_obj_t * label_select_ip;
lv_obj_t * label_ip_title;
lv_obj_t * btn_servo;
lv_obj_t * panel_servo;
lv_obj_t * sld_servo;
lv_obj_t * label_servo_val;
lv_obj_t * ui_wifi_bars[6][4];

// Top Layer Nav Buttons & Config Notifications
lv_obj_t * nav_btn_left;
lv_obj_t * nav_btn_right;
lv_obj_t * label_config_notify;
lv_obj_t * label_config_ip;

// Config Sliders/Switches
lv_obj_t * sld_quality;
lv_obj_t * sld_brightness;
lv_obj_t * sld_contrast;
lv_obj_t * sld_saturation;
lv_obj_t * sld_framesize;
lv_obj_t * sld_stream_framesize;
lv_obj_t * sld_led;
lv_obj_t * sw_awb;
lv_obj_t * sw_aec;
lv_obj_t * sw_agc;
lv_obj_t * sw_hmirror;
lv_obj_t * sw_vflip;

// Global flags
int current_screen = 0; // 0: Image, 1: Config
String configTargetIP = "";
String multiTargetIP = "Select IP";
String lastGlobalIP = "";
int lastImgW = 0;
int lastImgH = 0;
String res_names_config[30];
bool toggleHeader = false;
bool capture_requested = false;
bool capture_requested_multi = false;
uint32_t notify_done_time = 0;
bool ip_reloaded = false;
uint32_t ip_notify_time = 0;
bool is_streaming = false;
bool stream_paused = false;
bool servo_control_active = false;

// Telegram Progress Tracking
volatile size_t tele_progress_bytes = 0;
volatile size_t tele_total_bytes = 0;
volatile bool tele_updating = false;

HTTPClient http;
WiFiClient streamClient;
WiFiUDP udp;
WebServer server(80);
uint32_t last_udp_send_time = 0;
const int udpPort = 8888;
const int buzzerPin = 26; // Moved to 26 to free HSPI MISO (12)

TaskHandle_t telegramTaskHandle = NULL;

// SD Card Pipeline Configuration
static uint8_t chunkBuffer[8192];                // 8KB chunk buffer for HTTP/SD/Stream
const char* IMAGE_PATH       = "/img.jpg";       // Primary camera capture
const char* IMAGE_PATH_MULTI = "/multi.jpg";     // Multi-camera capture
bool sdImageReady      = false;
bool sdMultiImageReady = false;
bool sdAvailable       = false;

const uint32_t CAPTURE_TIMEOUT_MS = 10000;       // 10 second timeout for picture fetching
uint8_t telegramChunkBuffer[1024];               // Permanent 1KB buffer for SSL DMA writes

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
void handleDevices();
void handleRegister();
bool captureImage(String targetIP, const char* path, const char* path2 = NULL);
void processStatistic(bool increment = false);
String getArchiveFilename();
void runGlobalCapture();
bool mountSD();
void unmountSD();
bool loadConfig();
void displayImageOrText();
void updateRAMUsage(bool force = false);
void buildConfigScreen();
void buildStatsScreen();
void buildDevicesScreen();
void buildMultiScreen();
void buildIpSelectScreen();
void fetchAndApplyConfig();
void sendConfigChanges();
void switchScreen(int scr_id);
bool captureMultiImage();
void handleCaptureMulti();
void displayMultiImageOrText();
bool connectToStream();
void stopStream();
void processStream();
String streamReadLine(uint32_t timeoutMs = 2000);
bool streamReadExact(uint8_t* dst, size_t len, uint32_t timeoutMs = 5000);
void skipStreamHeaders();
size_t readStreamFrame();
void playCaptureBeep();
void createWiFiIcon(lv_obj_t * parent);
void updateWiFiSignal();
bool sendPhotoToTelegram(String chatId, const char* path);
void telegramUploadTask(void *pvParameters);

// Simple JSON value extractor
int getJsonVal(String json, String key) {
  int idx = json.indexOf("\"" + key + "\":");
  if (idx == -1) return -999;
  idx += key.length() + 3;
  int endIdx1 = json.indexOf(",", idx);
  int endIdx2 = json.indexOf("}", idx);
  int endIdx = -1;
  if (endIdx1 != -1 && endIdx2 != -1) endIdx = min(endIdx1, endIdx2);
  else if (endIdx1 != -1) endIdx = endIdx1;
  else if (endIdx2 != -1) endIdx = endIdx2;
  if (endIdx == -1) return -999;
  return json.substring(idx, endIdx).toInt();
}

String getJsonStr(String json, String key) {
  int idx = json.indexOf("\"" + key + "\":");
  if (idx == -1) return "";
  idx += key.length() + 3;
  int startQuote = json.indexOf("\"", idx);
  if (startQuote == -1) return "";
  int endQuote = json.indexOf("\"", startQuote + 1);
  if (endQuote == -1) return "";
  return json.substring(startQuote + 1, endQuote);
}

// LVGL Touch Read Callback
void my_touch_read(lv_indev_drv_t * indev_driver, lv_indev_data_t * data) {
  uint16_t touchX, touchY;
  bool touched = tft.getTouch(&touchX, &touchY);
  if (!touched) {
    data->state = LV_INDEV_STATE_REL;
  } else {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = touchX;
    data->point.y = touchY;
  }
}

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

void createWiFiIcon(lv_obj_t * parent, int scr_idx) {
  lv_obj_t * cont = lv_obj_create(parent);
  lv_obj_set_size(cont, 40, 25);
  lv_obj_align(cont, LV_ALIGN_RIGHT_MID, 1, -3);
  lv_obj_set_style_bg_opa(cont, 0, 0);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

  for (int i = 0; i < 4; i++) {
    ui_wifi_bars[scr_idx][i] = lv_obj_create(cont);
    lv_obj_set_size(ui_wifi_bars[scr_idx][i], 5, 5 + (i * 5));
    lv_obj_align(ui_wifi_bars[scr_idx][i], LV_ALIGN_BOTTOM_LEFT, i * 8, 0);
    lv_obj_set_style_bg_color(ui_wifi_bars[scr_idx][i], lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_radius(ui_wifi_bars[scr_idx][i], 1, 0);
    lv_obj_set_style_border_width(ui_wifi_bars[scr_idx][i], 0, 0);
  }
}

void updateWiFiSignal() {
  int32_t rssi = WiFi.RSSI();
  int bars = 0;
  lv_color_t color = lv_palette_main(LV_PALETTE_RED);

  if (rssi >= -60) {
    bars = 4;
    color = lv_palette_main(LV_PALETTE_GREEN);
  } else if (rssi >= -70) {
    bars = 3;
    color = lv_palette_main(LV_PALETTE_GREEN);
  } else if (rssi >= -80) {
    bars = 2;
    color = lv_palette_main(LV_PALETTE_YELLOW);
  } else if (rssi >= -90) {
    bars = 1;
    color = lv_palette_main(LV_PALETTE_RED);
  } else {
    bars = 0;
    color = lv_palette_main(LV_PALETTE_RED);
  }

  for (int s = 0; s < 6; s++) {
    if (ui_wifi_bars[s][0] == NULL) continue;
    for (int i = 0; i < 4; i++) {
      if (i < bars) {
        lv_obj_set_style_bg_color(ui_wifi_bars[s][i], color, 0);
        lv_obj_set_style_bg_opa(ui_wifi_bars[s][i], 255, 0);
      } else {
        lv_obj_set_style_bg_color(ui_wifi_bars[s][i], lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_set_style_bg_opa(ui_wifi_bars[s][i], 100, 0);
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  pinMode(buzzerPin, OUTPUT);
  digitalWrite(buzzerPin, LOW);

  // [0] Ensure all CS pins are HIGH to avoid SPI bus contention
  pinMode(5, OUTPUT);  digitalWrite(5, HIGH);  // SD CS
  pinMode(15, OUTPUT); digitalWrite(15, HIGH); // TFT CS
  pinMode(21, OUTPUT); digitalWrite(21, HIGH); // Touch CS

  Serial.println("\n\n--- ESP32 Gateway (Stability Mode) ---");

  // [1] Initialize Display (LovyanGFX) on VSPI
  Serial.println("1. Initializing Display...");
  tft.init();
  tft.setRotation(1); // Landscape
  tft.fillScreen(TFT_BLACK); // Prevent white flash on boot

  // Diagnostic Boot Screen
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 10);
  tft.println("System Diagnostic...");
  
  tft.setCursor(10, 40);
  tft.print("SD Card: ");

  // [2] Initialize SD Card (HSPI)
  // Ensure VSPI pins are stable before touching HSPI
  delay(100); 
  if (mountSD()) {
    uint64_t totalSize = SD.totalBytes() / (1024 * 1024);
    Serial.printf("[SD] SUCCESS: Max Size %llu MB detected. System Ready.\n", totalSize);
    sdAvailable = true;
    
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.println("OK");
    
    // Load credentials from config.json
    loadConfig();

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 70);
    tft.print("WiFi Credential: ");
    if (ssid.length() > 0) {
      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.println("OK");
    } else {
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.println("MISSING");
    }
    
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 100);
    tft.print("Telegram Credential: ");
    if (botToken.length() > 0 && targetChatId.length() > 0) {
      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.println("OK");
    } else {
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.println("MISSING");
    }

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 130);
    tft.println("WiFi: Connecting...");
    
    delay(1000);
    tft.fillScreen(TFT_BLACK); // Clear for LVGL
  } else {
    Serial.println("[SD] Mount Failed. Storage disabled.");
    sdAvailable = false;
    
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.println("FAIL");
    tft.setCursor(10, 70);
    tft.println("System Halted.");
    tft.setCursor(10, 100);
    tft.println("Tap Screen to Reboot");
    
    while(true) {
      uint16_t x, y;
      if (tft.getTouch(&x, &y)) {
        ESP.restart();
      }
      delay(50);
    }
  }

  // Proceed with system load
  Serial.println("2. Initializing PSRAM...");
  initPSRAM();

  // Initialize LVGL
  Serial.println("3. Initializing LVGL...");
  lv_init();
  
  // Allocate LVGL draw buffer
  size_t buf_lines = 5; // Ultra-minimal lines to save RAM for SSL
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

  // Register Touch Input Device
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touch_read;
  lv_indev_drv_register(&indev_drv);

  // Create Screens
  scr_image = lv_obj_create(NULL);
  lv_obj_set_style_pad_all(scr_image, 0, 0);
  lv_obj_set_style_bg_color(scr_image, lv_color_hex(0x202020), 0);
  lv_obj_set_style_bg_opa(scr_image, 255, 0);

  // Top Section (Information)
  top_panel = lv_obj_create(scr_image);
  lv_obj_set_size(top_panel, screenWidth, 30);
  lv_obj_align(top_panel, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_pad_all(top_panel, 0, 0);
  lv_obj_set_style_border_width(top_panel, 0, 0);
  lv_obj_set_style_radius(top_panel, 0, 0);
  lv_obj_set_style_bg_color(top_panel, lv_palette_main(LV_PALETTE_BLUE_GREY), 0);
  lv_obj_clear_flag(top_panel, LV_OBJ_FLAG_SCROLLABLE);

  // Body Section (Image View)
  lv_obj_t * body_panel = lv_obj_create(scr_image);
  lv_obj_set_size(body_panel, screenWidth, screenHeight - 30);
  lv_obj_align(body_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_pad_all(body_panel, 0, 0);
  lv_obj_set_style_border_width(body_panel, 0, 0);
  lv_obj_set_style_radius(body_panel, 0, 0);
  lv_obj_set_style_bg_color(body_panel, lv_color_hex(0x202020), 0); // Dark grey background
  lv_obj_set_style_bg_opa(body_panel, 0, 0); // Transparent to avoid overlapping JPEG

  scr_config = lv_obj_create(NULL);
  lv_obj_set_style_pad_all(scr_config, 0, 0);
  lv_obj_set_style_bg_color(scr_config, lv_color_hex(0x202020), 0);
  lv_obj_set_style_bg_opa(scr_config, 255, 0);

  scr_stats = lv_obj_create(NULL);
  lv_obj_set_style_pad_all(scr_stats, 0, 0);
  lv_obj_set_style_bg_color(scr_stats, lv_color_hex(0x202020), 0);
  lv_obj_set_style_bg_opa(scr_stats, 255, 0);

  scr_devices = lv_obj_create(NULL);
  lv_obj_set_style_pad_all(scr_devices, 0, 0);
  lv_obj_set_style_bg_color(scr_devices, lv_color_hex(0x202020), 0);
  lv_obj_set_style_bg_opa(scr_devices, 255, 0);

  scr_multi = lv_obj_create(NULL);
  lv_obj_set_style_pad_all(scr_multi, 0, 0);
  lv_obj_set_style_bg_color(scr_multi, lv_color_hex(0x202020), 0);
  lv_obj_set_style_bg_opa(scr_multi, 255, 0);

  scr_ip_select = lv_obj_create(NULL);
  lv_obj_set_style_pad_all(scr_ip_select, 0, 0);
  lv_obj_set_style_bg_color(scr_ip_select, lv_color_hex(0x202020), 0);
  lv_obj_set_style_bg_opa(scr_ip_select, 255, 0);

  // Initialize UI components on Image Screen
  label_status = lv_label_create(top_panel);
  lv_label_set_text(label_status, "Ready...");
  lv_obj_set_style_text_color(label_status, lv_color_white(), 0);
  lv_obj_align(label_status, LV_ALIGN_LEFT_MID, 10, 0);

  label_ram = lv_label_create(top_panel);
  lv_label_set_text(label_ram, "RAM: --");
  lv_obj_set_style_text_color(label_ram, lv_color_white(), 0);
  lv_obj_align(label_ram, LV_ALIGN_RIGHT_MID, -45, 0);

  createWiFiIcon(top_panel, 0);

  buildConfigScreen();
  buildStatsScreen();
  buildDevicesScreen();
  buildMultiScreen();

  // Create Top Layer Navigation Buttons
  nav_btn_left = lv_btn_create(lv_layer_top());
  lv_obj_set_size(nav_btn_left, 30, 100);
  lv_obj_align(nav_btn_left, LV_ALIGN_LEFT_MID, 5, 0);
  lv_obj_set_style_bg_color(nav_btn_left, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(nav_btn_left, LV_OPA_TRANSP, 0); // Fully transparent background
  lv_obj_set_style_border_width(nav_btn_left, 1, 0);
  lv_obj_set_style_border_color(nav_btn_left, lv_color_white(), 0);
  lv_obj_set_style_radius(nav_btn_left, 5, 0); // Rounded corners to match manual drawing
  lv_obj_add_event_cb(nav_btn_left, [](lv_event_t *e) { 
    if (current_screen == 0) switchScreen(3);
    else if (current_screen == 3) switchScreen(2);
    else if (current_screen == 2) switchScreen(4);
    else {
      switchScreen(0);
      displayImageOrText();
    }
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t * lbl_l = lv_label_create(nav_btn_left);
  lv_label_set_text(lbl_l, "<");
  lv_obj_set_style_text_color(lbl_l, lv_color_white(), 0);
  lv_obj_center(lbl_l);

  nav_btn_right = lv_btn_create(lv_layer_top());
  lv_obj_set_size(nav_btn_right, 30, 100);
  lv_obj_align(nav_btn_right, LV_ALIGN_RIGHT_MID, -5, 0);
  lv_obj_set_style_bg_color(nav_btn_right, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(nav_btn_right, LV_OPA_TRANSP, 0); // Fully transparent background
  lv_obj_set_style_border_width(nav_btn_right, 1, 0);
  lv_obj_set_style_border_color(nav_btn_right, lv_color_white(), 0);
  lv_obj_set_style_radius(nav_btn_right, 5, 0); // Rounded corners to match manual drawing
  lv_obj_add_event_cb(nav_btn_right, [](lv_event_t *e) { 
    if (current_screen == 0) switchScreen(4);
    else if (current_screen == 4) switchScreen(2);
    else if (current_screen == 2) switchScreen(3);
    else {
      switchScreen(0);
      displayImageOrText();
    }
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t * lbl_r = lv_label_create(nav_btn_right);
  lv_label_set_text(lbl_r, ">");
  lv_obj_set_style_text_color(lbl_r, lv_color_white(), 0);
  lv_obj_center(lbl_r);

  switchScreen(0);
  displayImageOrText();

  // Connect to WiFi
  connectToWiFi();
  
  // Setup Web Server routes
  server.on("/", handleRoot);
  server.on("/control", handleControl);
  server.on("/xclk", handleXCLK);
  server.on("/capture", handleCapture);
  server.on("/image", handleImage);
  server.on("/status", handleStatus);
  server.on("/devices", handleDevices);
  server.on("/register", HTTP_POST, handleRegister);
  
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

  // Polling touch
  uint16_t x, y;
  static bool was_touched = false;
  bool is_touched = tft.getTouch(&x, &y);
  
  if (current_screen == 0) { // Image Screen manual touch handling
    if (is_touched && !was_touched) {
      if (y > 30) {
        if (x < 45 && y > 100 && y < 220) {
          switchScreen(3); // Go to Devices (left)
        } else if (x > screenWidth - 45 && y > 100 && y < 220) {
          switchScreen(4); // Go to Multi Camera (right)
        }
      }    }
  } else if (current_screen == 4) { // Multi Camera manual touch handling
    if (is_touched && !was_touched) {
      if (y > 30) {
        if (x < 45 && y > 100 && y < 220) {
          switchScreen(0); // Left to Image
          displayImageOrText();
        } else if (x > screenWidth - 45 && y > 100 && y < 220) {
          switchScreen(2); // Right to Stats
        } else if (x > 60 && x < screenWidth - 60 && (!servo_control_active || y < screenHeight - 40)) {
          if (!is_streaming) {
            capture_requested_multi = true;
            lv_label_set_text(label_notify_multi, "Capturing...");
            lv_timer_handler();
          } else {
            stream_paused = !stream_paused;
            if (stream_paused) {
              stopStream();
              lv_label_set_text(label_notify_multi, "Paused");
            } else {
              lv_label_set_text(label_notify_multi, "Connecting...");
              lv_timer_handler();
              connectToStream();
              lv_label_set_text(label_notify_multi, "Streaming");
            }
            lv_timer_handler();
          }
        }
      }
    }
  }
  was_touched = is_touched;

  // Real-time Telegram Upload Progress (Core 1)
  static bool last_tele_updating = false;
  static size_t last_tele_progress = 0;

  if (tele_updating && current_screen == 0 && label_status) {
    if (tele_progress_bytes != last_tele_progress || !last_tele_updating) {
      if (tele_progress_bytes == 0) {
        lv_label_set_text(label_status, "tele: connecting...");
      } else {
        lv_label_set_text_fmt(label_status, "tele: %d/%dk", (int)(tele_progress_bytes/1024), (int)(tele_total_bytes/1024));
      }
      last_tele_progress = tele_progress_bytes;
    }
    last_tele_updating = true;
  } else if (!tele_updating && last_tele_updating) {
    // Restore UI when Telegram task finishes
    if (current_screen == 0 && label_status) {
      lv_label_set_text_fmt(label_status, "Cam: %s", lastGlobalIP.c_str());
    }
    last_tele_updating = false;
    last_tele_progress = 0;
  }

  if (current_screen == 4 && is_streaming && !stream_paused) {
    processStream();
  }

  if (capture_requested) {
    capture_requested = false;
    runGlobalCapture();
  }

  if (capture_requested_multi) {
    capture_requested_multi = false;
    handleCaptureMulti();
  }
  
  if (notify_done_time > 0 && millis() - notify_done_time > 1000) {
    if (current_screen == 4) {
      if (!is_streaming) lv_label_set_text(label_notify_multi, "Tap to capture");
    }
    notify_done_time = 0;
  }
  
  if (ip_notify_time > 0 && millis() - ip_notify_time > 1000) {
    if (current_screen == 5) lv_label_set_text(label_ip_title, "Select Camera IP");
    ip_notify_time = 0;
  }
}

void updateRAMUsage(bool force) {
  static uint32_t last_update = 0;
  if (force || millis() - last_update > 2000) {
    last_update = millis();
    uint32_t free_h = ESP.getFreeHeap();
    uint32_t total_h = ESP.getHeapSize();
    uint32_t used_h = total_h - free_h;
    
    updateWiFiSignal();

    if (current_screen == 4) {
      lv_label_set_text_fmt(label_ram_multi, "RAM: %u/%u KB", used_h/1024, total_h/1024);
    } else if (current_screen == 2) {
      lv_label_set_text_fmt(label_ram_stats, "RAM: %u/%u KB", used_h/1024, total_h/1024);
      
      // Update SD Storage Stats
      if (sdAvailable) {
        uint64_t totalBytes = SD.totalBytes();
        uint64_t usedBytes = SD.usedBytes();
        int totalMB = (int)(totalBytes / (1024 * 1024));
        int usedMB = (int)(usedBytes / (1024 * 1024));
        
        if (sd_bar) {
          lv_bar_set_range(sd_bar, 0, totalMB);
          lv_bar_set_value(sd_bar, usedMB, LV_ANIM_OFF);
        }
        if (sd_val_label) {
          lv_label_set_text_fmt(sd_val_label, " %d/%dMB", usedMB, totalMB);
        }
      } else {
        if (sd_bar) {
          lv_bar_set_range(sd_bar, 0, 1);
          lv_bar_set_value(sd_bar, 0, LV_ANIM_OFF);
        }
        if (sd_val_label) {
          lv_label_set_text(sd_val_label, " No SD Card");
        }
      }
    } else if (current_screen == 3) {
      lv_obj_clean(scr_devices);
      buildDevicesScreen();
      updateWiFiSignal(); // Apply colors to the newly created bars
      lv_label_set_text_fmt(label_ram_devices, "RAM: %u/%u KB", used_h/1024, total_h/1024);
    } else {
      lv_label_set_text_fmt(label_ram, "RAM: %u/%u KB", used_h/1024, total_h/1024);
      if (current_screen == 0) {
        if (toggleHeader) {
          lv_label_set_text_fmt(label_status, "Cam: %s", lastGlobalIP.c_str());
        } else {
          lv_label_set_text_fmt(label_status, "Res: %dx%d", lastImgW, lastImgH);
        }
        toggleHeader = !toggleHeader;
      }
    }
  }
}

void switchScreen(int scr_id) {
  if (current_screen == 4 && scr_id != 4) {
    stopStream();
  }
  
  current_screen = scr_id;
  if (scr_id == 0) {
    lv_obj_add_flag(nav_btn_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(nav_btn_right, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load(scr_image);
    
    // UI Sync: Ensure screen is visible before raw drawing
    for (int i = 0; i < 15; i++) {
      lv_timer_handler();
      delay(10);
    }
    lv_refr_now(NULL);
  } else if (scr_id == 1) {
    lv_obj_add_flag(nav_btn_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(nav_btn_right, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(label_config_ip, configTargetIP.c_str());
    lv_scr_load(scr_config);
    lv_refr_now(NULL); // Force synchronous UI draw
    fetchAndApplyConfig();
  } else if (scr_id == 2) {
    lv_obj_clear_flag(nav_btn_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(nav_btn_right, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(scr_stats);
    buildStatsScreen();
    lv_scr_load(scr_stats);
    lv_refr_now(NULL);
  } else if (scr_id == 3) {
    lv_obj_clear_flag(nav_btn_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(nav_btn_right, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(scr_devices);
    buildDevicesScreen();
    lv_scr_load(scr_devices);
    lv_refr_now(NULL);
  } else if (scr_id == 4) {
    lv_obj_add_flag(nav_btn_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(nav_btn_right, LV_OBJ_FLAG_HIDDEN);
    
    if (multiTargetIP == "Select IP") {
      for (int i = 0; i < 5; i++) {
        if (devices[i].ip != "") {
          multiTargetIP = devices[i].ip;
          break;
        }
      }
    }
    
    lv_label_set_text(label_select_ip, multiTargetIP.c_str());
    
    if (!is_streaming) {
      lv_obj_add_flag(btn_servo, LV_OBJ_FLAG_HIDDEN);
      servo_control_active = false;
      if (panel_servo) lv_obj_add_flag(panel_servo, LV_OBJ_FLAG_HIDDEN);
      if (btn_servo) lv_obj_set_style_bg_opa(btn_servo, LV_OPA_TRANSP, 0);
      lv_label_set_text(label_notify_multi, "Tap to capture");
    } else {
      lv_obj_clear_flag(btn_servo, LV_OBJ_FLAG_HIDDEN);
      stream_paused = false;
      lv_label_set_text(label_notify_multi, "Streaming");
    }

    lv_scr_load(scr_multi);
    
    // UI Sync: Ensure the new screen is fully established and visible on TFT
    // before we start any raw drawing or blocking operations.
    for (int i = 0; i < 15; i++) {
      lv_timer_handler();
      delay(10);
    }
    lv_refr_now(NULL); // Force final sync
    
    if (is_streaming) {
      connectToStream();
    } else {
      displayMultiImageOrText();
    }
  } else if (scr_id == 5) {
    lv_obj_add_flag(nav_btn_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(nav_btn_right, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(scr_ip_select);
    buildIpSelectScreen();
    lv_scr_load(scr_ip_select);
    lv_refr_now(NULL);
  }
  updateRAMUsage(true);
}

void fetchAndApplyConfig() {
  if (WiFi.status() != WL_CONNECTED || configTargetIP == "") return;
  lv_label_set_text(label_config_notify, "Fetching status...");
  lv_timer_handler();

  http.begin("http://" + configTargetIP + "/status");
  http.setTimeout(5000);
  int code = http.GET();
  if (code == 200) {
    String json = http.getString();
    
    // Parse Resolutions array
    int resIdx = json.indexOf("\"resolutions\":[");
    if (resIdx != -1) {
      resIdx += 15;
      int endRes = json.indexOf("]", resIdx);
      String resStr = json.substring(resIdx, endRes);
      for (int i = 0; i < 30; i++) res_names_config[i] = "";
      int count = 0;
      int startQuote = -1;
      for (size_t i = 0; i < resStr.length(); i++) {
        if (resStr[i] == '\"') {
          if (startQuote == -1) startQuote = i + 1;
          else {
            res_names_config[count++] = resStr.substring(startQuote, i);
            startQuote = -1;
            if (count >= 30) break;
          }
        }
      }
    }

    auto setSld = [&](lv_obj_t* sld, String key) {
      int val = getJsonVal(json, key);
      int minV = getJsonVal(json, key + "_min");
      int maxV = getJsonVal(json, key + "_max");
      
      if (minV != -999 && maxV != -999) {
        lv_slider_set_range(sld, minV, maxV);
      }

      if (val != -999) {
        lv_slider_set_value(sld, val, LV_ANIM_OFF);
        lv_event_send(sld, LV_EVENT_VALUE_CHANGED, NULL);
      }
    };

    setSld(sld_framesize, "framesize");
    setSld(sld_stream_framesize, "stream_framesize");
    setSld(sld_quality, "quality");
    setSld(sld_brightness, "brightness");
    setSld(sld_contrast, "contrast");
    setSld(sld_saturation, "saturation");
    
    // LED Slider (not necessarily in status with min/max, but let's update value)
    int ledVal = getJsonVal(json, "led_intensity");
    if (ledVal != -999 && ledVal != -1) {
      lv_slider_set_value(sld_led, ledVal, LV_ANIM_OFF);
      lv_event_send(sld_led, LV_EVENT_VALUE_CHANGED, NULL);
    }

    auto setSw = [&](lv_obj_t* sw, String key) {
      int val = getJsonVal(json, key);
      if (val != -999) {
        if (val) lv_obj_add_state(sw, LV_STATE_CHECKED);
        else lv_obj_clear_state(sw, LV_STATE_CHECKED);
      }
    };

    setSw(sw_awb, "awb");
    setSw(sw_aec, "aec");
    setSw(sw_agc, "agc");
    setSw(sw_hmirror, "hmirror");
    setSw(sw_vflip, "vflip");
    
    lv_label_set_text(label_config_notify, "Status loaded");
  } else {
    lv_label_set_text(label_config_notify, "Error: No respond");
  }
  http.end();
}

void sendConfigChanges() {
  if (WiFi.status() != WL_CONNECTED) {
    lv_label_set_text(label_config_notify, "Error: No WiFi");
    return;
  }
  if (configTargetIP == "") return;
  
  lv_label_set_text(label_config_notify, "applying..");
  lv_timer_handler();
  
  int success_count = 0;
  int fail_count = 0;
  
  auto sendVal = [&](String key, int val) {
    String url = "http://" + configTargetIP + "/control?var=" + key + "&val=" + String(val);
    http.begin(url);
    http.setTimeout(2000);
    int code = http.GET();
    if (code == 200) success_count++;
    else fail_count++;
    http.end();
  };
  
  sendVal("framesize", lv_slider_get_value(sld_framesize));
  sendVal("stream_framesize", lv_slider_get_value(sld_stream_framesize));
  sendVal("quality", lv_slider_get_value(sld_quality));
  sendVal("brightness", lv_slider_get_value(sld_brightness));
  sendVal("contrast", lv_slider_get_value(sld_contrast));
  sendVal("saturation", lv_slider_get_value(sld_saturation));
  sendVal("led_intensity", lv_slider_get_value(sld_led));
  sendVal("awb", lv_obj_has_state(sw_awb, LV_STATE_CHECKED) ? 1 : 0);
  sendVal("aec", lv_obj_has_state(sw_aec, LV_STATE_CHECKED) ? 1 : 0);
  sendVal("agc", lv_obj_has_state(sw_agc, LV_STATE_CHECKED) ? 1 : 0);
  sendVal("hmirror", lv_obj_has_state(sw_hmirror, LV_STATE_CHECKED) ? 1 : 0);
  sendVal("vflip", lv_obj_has_state(sw_vflip, LV_STATE_CHECKED) ? 1 : 0);
  
  if (fail_count > 0 && success_count == 0) {
    lv_label_set_text(label_config_notify, "Error: No respond");
  } else if (fail_count > 0) {
    lv_label_set_text(label_config_notify, "Error: Can't apply all");
  } else {
    lv_label_set_text(label_config_notify, "applied");
  }
}

lv_obj_t * create_slider(lv_obj_t * parent, const char * name, int min, int max, lv_obj_t ** slider) {
    lv_obj_t * wrapper = lv_obj_create(parent);
    lv_obj_set_size(wrapper, 440, 60);
    lv_obj_set_style_pad_all(wrapper, 5, 0);
    lv_obj_t * lbl = lv_label_create(wrapper);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
    *slider = lv_slider_create(wrapper);
    lv_slider_set_range(*slider, min, max);
    lv_obj_set_size(*slider, 380, 10);
    lv_obj_align(*slider, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t * val_lbl = lv_label_create(wrapper);
    lv_label_set_text(val_lbl, "0");
    lv_obj_align(val_lbl, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(*slider, [](lv_event_t * e) {
        lv_obj_t * s = lv_event_get_target(e);
        lv_obj_t * v = (lv_obj_t *)lv_event_get_user_data(e);
        lv_label_set_text_fmt(v, "%d", lv_slider_get_value(s));
    }, LV_EVENT_VALUE_CHANGED, val_lbl);
    return wrapper;
}

lv_obj_t * create_switch(lv_obj_t * parent, const char * name, lv_obj_t ** sw) {
    lv_obj_t * wrapper = lv_obj_create(parent);
    lv_obj_set_size(wrapper, 440, 45);
    lv_obj_set_style_pad_all(wrapper, 5, 0);
    lv_obj_t * lbl = lv_label_create(wrapper);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    *sw = lv_switch_create(wrapper);
    lv_obj_set_size(*sw, 40, 20);
    lv_obj_align(*sw, LV_ALIGN_RIGHT_MID, 0, 0);
    return wrapper;
}

void chart_draw_event_cb(lv_event_t * e) {
    lv_obj_draw_part_dsc_t * dsc = lv_event_get_draw_part_dsc(e);
    if(dsc->part == LV_PART_TICKS && dsc->id == LV_CHART_AXIS_PRIMARY_X && dsc->text) {
        const char * days[] = {"1", "2", "3", "4", "5", "6", "7"};
        if(dsc->value >= 0 && dsc->value < 7) {
            lv_snprintf(dsc->text, dsc->text_length, "%s", days[dsc->value]);
        }
    }
}

void buildStatsScreen() {
  // Update/Shift statistics before building screen
  processStatistic(false);
  
  StaticJsonDocument<512> doc;
  File file = SD.open("/statistic.json", FILE_READ);
  if (file) {
    deserializeJson(doc, file);
    file.close();
  } else {
    // Fallback if read fails
    doc["today"] = 0;
    for(int i=1; i<=7; i++) doc[String(i)] = 0;
  }

  // Top Section
  lv_obj_t * top_panel_stats = lv_obj_create(scr_stats);
  lv_obj_set_size(top_panel_stats, screenWidth, 30);
  lv_obj_align(top_panel_stats, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_pad_all(top_panel_stats, 0, 0);
  lv_obj_set_style_border_width(top_panel_stats, 0, 0);
  lv_obj_set_style_radius(top_panel_stats, 0, 0);
  lv_obj_set_style_bg_color(top_panel_stats, lv_palette_main(LV_PALETTE_BLUE_GREY), 0);
  lv_obj_clear_flag(top_panel_stats, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t * title = lv_label_create(top_panel_stats);
  lv_label_set_text(title, "Statistics Dashboard");
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_align(title, LV_ALIGN_LEFT_MID, 10, 0);

  label_ram_stats = lv_label_create(top_panel_stats);
  lv_label_set_text(label_ram_stats, "RAM: --");
  lv_obj_set_style_text_color(label_ram_stats, lv_color_white(), 0);
  lv_obj_align(label_ram_stats, LV_ALIGN_RIGHT_MID, -45, 0);

  createWiFiIcon(top_panel_stats, 2);

  // Body container (Centered, 400px)
  lv_obj_t * cont = lv_obj_create(scr_stats);
  lv_obj_set_size(cont, 400, screenHeight - 30);
  lv_obj_align(cont, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(cont, 15, 0);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_radius(cont, 0, 0);
  lv_obj_set_style_bg_color(cont, lv_color_hex(0x202020), 0);
  lv_obj_set_style_text_color(cont, lv_color_white(), 0);

  // Stats content
  lv_obj_t * lbl_today = lv_label_create(cont);
  lv_label_set_text_fmt(lbl_today, "Camera Triggered Today: %d", (int)(doc["today"] | 0));
  lv_obj_set_width(lbl_today, 360);
  lv_obj_set_style_text_align(lbl_today, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_bottom(lbl_today, 5, 0); // Reduced from 20

  lv_obj_t * lbl_chart = lv_label_create(cont);
  lv_label_set_text(lbl_chart, "Camera triggered last 7 days");
  lv_obj_set_width(lbl_chart, 360);
  lv_obj_set_style_text_align(lbl_chart, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_bottom(lbl_chart, 5, 0);

  // Calculate dynamic Y range based on max value in history
  int max_history = 0;
  for (int i = 1; i <= 7; i++) {
    int val = doc[String(i)] | 0;
    if (val > max_history) max_history = val;
  }
  int y_max = (max_history < 10) ? 10 : (max_history + (max_history / 5) + 1); // Buffer of 20% or min 10

  // Line chart
  lv_obj_t * chart = lv_chart_create(cont);
  lv_obj_set_size(chart, 300, 130); // Reduced height from 150
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(chart, 7);
  lv_obj_set_style_bg_color(chart, lv_color_black(), 0);
  lv_obj_set_style_border_color(chart, lv_color_white(), 0);
  lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);

  // Add Axis Labels (Dynamic Y, X: 1-7 days)
  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, y_max);
  lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_Y, 10, 5, 6, 2, true, 40);
  
  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_X, 0, 6);
  lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_X, 10, 5, 7, 1, true, 40);
  lv_obj_add_event_cb(chart, chart_draw_event_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
  
  // Add some padding to make room for labels
  lv_obj_set_style_pad_left(chart, 40, 0);
  lv_obj_set_style_pad_bottom(chart, 15, 0); // Reduced from 20

  lv_chart_series_t * ser = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_PRIMARY_Y);
  // Populate chart from JSON (indices 1 to 7)
  lv_chart_set_next_value(chart, ser, (int)(doc["1"] | 0));
  lv_chart_set_next_value(chart, ser, (int)(doc["2"] | 0));
  lv_chart_set_next_value(chart, ser, (int)(doc["3"] | 0));
  lv_chart_set_next_value(chart, ser, (int)(doc["4"] | 0));
  lv_chart_set_next_value(chart, ser, (int)(doc["5"] | 0));
  lv_chart_set_next_value(chart, ser, (int)(doc["6"] | 0));
  lv_chart_set_next_value(chart, ser, (int)(doc["7"] | 0));

  // SD Card Storage Bar
  lv_obj_t * sd_cont = lv_obj_create(cont);
  lv_obj_set_size(sd_cont, 360, 30);
  lv_obj_set_style_bg_opa(sd_cont, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(sd_cont, 0, 0);
  lv_obj_set_style_pad_all(sd_cont, 0, 0);
  lv_obj_clear_flag(sd_cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_top(sd_cont, 30, 0);
  lv_obj_set_flex_flow(sd_cont, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(sd_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t * sd_label = lv_label_create(sd_cont);
  lv_label_set_text(sd_label, "SD Card: ");
  lv_obj_set_style_text_color(sd_label, lv_color_white(), 0);

  sd_bar = lv_bar_create(sd_cont);
  lv_obj_set_size(sd_bar, 180, 15);
  lv_obj_set_style_radius(sd_bar, 0, 0); // Square corners
  lv_obj_set_style_radius(sd_bar, 0, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sd_bar, lv_color_hex(0x555555), 0);
  lv_obj_set_style_bg_color(sd_bar, lv_palette_main(LV_PALETTE_BLUE), LV_PART_INDICATOR);
  lv_bar_set_range(sd_bar, 0, 8000);
  lv_bar_set_value(sd_bar, 1, LV_ANIM_OFF); // Dummy data

  sd_val_label = lv_label_create(sd_cont);
  lv_label_set_text(sd_val_label, " 1/8000kb");
  lv_obj_set_style_text_color(sd_val_label, lv_color_white(), 0);
}

void buildDevicesScreen() {
  // Top Section
  lv_obj_t * top_panel_dev = lv_obj_create(scr_devices);
  lv_obj_set_size(top_panel_dev, screenWidth, 30);
  lv_obj_align(top_panel_dev, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_pad_all(top_panel_dev, 0, 0);
  lv_obj_set_style_border_width(top_panel_dev, 0, 0);
  lv_obj_set_style_radius(top_panel_dev, 0, 0);
  lv_obj_set_style_bg_color(top_panel_dev, lv_palette_main(LV_PALETTE_BLUE_GREY), 0);
  lv_obj_clear_flag(top_panel_dev, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t * title = lv_label_create(top_panel_dev);
  lv_label_set_text(title, "List Devices");
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_align(title, LV_ALIGN_LEFT_MID, 10, 0);

  label_ram_devices = lv_label_create(top_panel_dev);
  lv_label_set_text(label_ram_devices, "RAM: --");
  lv_obj_set_style_text_color(label_ram_devices, lv_color_white(), 0);
  lv_obj_align(label_ram_devices, LV_ALIGN_RIGHT_MID, -45, 0);

  createWiFiIcon(top_panel_dev, 3);

  // Body container (Centered, 400px)
  lv_obj_t * cont = lv_obj_create(scr_devices);
  lv_obj_set_size(cont, 400, screenHeight - 30);
  lv_obj_align(cont, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(cont, 10, 0);
  lv_obj_set_style_pad_row(cont, 5, 0);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_radius(cont, 0, 0);
  lv_obj_set_style_bg_color(cont, lv_color_hex(0x202020), 0);

  // Table Header Row
  lv_obj_t * row_hdr = lv_obj_create(cont);
  lv_obj_set_size(row_hdr, 380, 30);
  lv_obj_set_style_bg_color(row_hdr, lv_palette_main(LV_PALETTE_BLUE_GREY), 0);
  lv_obj_set_style_border_width(row_hdr, 0, 0);
  lv_obj_set_style_radius(row_hdr, 3, 0);
  lv_obj_set_style_pad_all(row_hdr, 0, 0);
  lv_obj_set_flex_flow(row_hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row_hdr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  auto add_col = [](lv_obj_t* parent, const char* txt, int w) {
    lv_obj_t * lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_width(lbl, w);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  };

  add_col(row_hdr, "No.", 30);
  add_col(row_hdr, "MAC", 150);
  add_col(row_hdr, "IP", 120);
  add_col(row_hdr, "SIGNAL", 60);

  // Dummy Data Rows
  auto add_row = [&](int no, const char* mac, const char* ip, int rssi) {
    lv_obj_t * row = lv_obj_create(cont);
    lv_obj_set_size(row, 380, 40);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x303030), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 3, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    char buf[8];
    sprintf(buf, "%d", no);
    add_col(row, buf, 30);
    add_col(row, mac, 150);
    add_col(row, ip, 120);

    // Signal Icon instead of PING button
    lv_obj_t * sig_cont = lv_obj_create(row);
    lv_obj_set_size(sig_cont, 50, 30);
    lv_obj_set_style_bg_opa(sig_cont, 0, 0);
    lv_obj_set_style_border_width(sig_cont, 0, 0);
    lv_obj_set_style_pad_all(sig_cont, 0, 0);
    lv_obj_clear_flag(sig_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(sig_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sig_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(sig_cont, 2, 0);
    lv_obj_set_style_translate_y(sig_cont, -5, 0);

    int bars = 0;
    lv_color_t color = lv_palette_main(LV_PALETTE_RED);
    if (rssi >= -60) { bars = 4; color = lv_palette_main(LV_PALETTE_GREEN); }
    else if (rssi >= -70) { bars = 3; color = lv_palette_main(LV_PALETTE_GREEN); }
    else if (rssi >= -80) { bars = 2; color = lv_palette_main(LV_PALETTE_YELLOW); }
    else if (rssi >= -90) { bars = 1; color = lv_palette_main(LV_PALETTE_RED); }
    else { bars = 0; color = lv_palette_main(LV_PALETTE_RED); }

    for (int j = 0; j < 4; j++) {
      lv_obj_t * b = lv_obj_create(sig_cont);
      lv_obj_set_size(b, 4, 5 + (j * 4));
      if (j < bars) {
        lv_obj_set_style_bg_color(b, color, 0);
        lv_obj_set_style_bg_opa(b, 255, 0);
      } else {
        lv_obj_set_style_bg_color(b, lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_set_style_bg_opa(b, 100, 0);
      }
      lv_obj_set_style_border_width(b, 0, 0);
      lv_obj_set_style_radius(b, 1, 0);
    }
  };

  int row_count = 1;
  for (int i = 0; i < 5; i++) {
    if (devices[i].mac != "" && devices[i].ip != "") {
      add_row(row_count++, devices[i].mac.c_str(), devices[i].ip.c_str(), devices[i].rssi);
    }
  }
}

void buildMultiScreen() {
  // Top Section
  top_panel_multi = lv_obj_create(scr_multi);
  lv_obj_set_size(top_panel_multi, screenWidth, 30);
  lv_obj_align(top_panel_multi, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_pad_all(top_panel_multi, 0, 0);
  lv_obj_set_style_border_width(top_panel_multi, 0, 0);
  lv_obj_set_style_radius(top_panel_multi, 0, 0);
  lv_obj_set_style_bg_color(top_panel_multi, lv_palette_main(LV_PALETTE_BLUE_GREY), 0);
  lv_obj_clear_flag(top_panel_multi, LV_OBJ_FLAG_SCROLLABLE);

  // CFG Button with Cogwheel
  lv_obj_t * btn_cfg = lv_btn_create(top_panel_multi);
  lv_obj_set_size(btn_cfg, 35, 28);
  lv_obj_align(btn_cfg, LV_ALIGN_LEFT_MID, 5, 0);
  lv_obj_set_style_bg_opa(btn_cfg, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(btn_cfg, 1, 0);
  lv_obj_set_style_border_color(btn_cfg, lv_color_white(), 0);
  lv_obj_set_style_shadow_width(btn_cfg, 0, 0);
  lv_obj_add_event_cb(btn_cfg, [](lv_event_t *e) {
    configTargetIP = multiTargetIP;
    if (configTargetIP != "Select IP") {
      switchScreen(1);
    }
  }, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t * lbl_cfg = lv_label_create(btn_cfg);
  lv_label_set_text(lbl_cfg, LV_SYMBOL_SETTINGS);
  lv_obj_center(lbl_cfg);

  // Button for IP selection
  btn_select_ip = lv_btn_create(top_panel_multi);
  lv_obj_set_size(btn_select_ip, 110, 28);
  lv_obj_align(btn_select_ip, LV_ALIGN_LEFT_MID, 45, 0);
  lv_obj_set_style_bg_opa(btn_select_ip, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(btn_select_ip, 1, 0);
  lv_obj_set_style_border_color(btn_select_ip, lv_color_white(), 0);
  lv_obj_set_style_shadow_width(btn_select_ip, 0, 0);
  lv_obj_add_event_cb(btn_select_ip, [](lv_event_t *e) { switchScreen(5); }, LV_EVENT_CLICKED, NULL);

  label_select_ip = lv_label_create(btn_select_ip);
  lv_label_set_text(label_select_ip, multiTargetIP.c_str());
  lv_obj_set_style_text_font(label_select_ip, &lv_font_montserrat_14, 0);
  lv_obj_center(label_select_ip);

  // Servo Button
  btn_servo = lv_btn_create(top_panel_multi);
  lv_obj_set_size(btn_servo, 35, 28);
  lv_obj_align(btn_servo, LV_ALIGN_LEFT_MID, 160, 0);
  lv_obj_set_style_bg_opa(btn_servo, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(btn_servo, 1, 0);
  lv_obj_set_style_border_color(btn_servo, lv_color_white(), 0);
  lv_obj_set_style_shadow_width(btn_servo, 0, 0);
  lv_obj_add_event_cb(btn_servo, [](lv_event_t *e) {
    servo_control_active = !servo_control_active;
    if (servo_control_active) {
      lv_obj_clear_flag(panel_servo, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_bg_color(btn_servo, lv_palette_main(LV_PALETTE_GREEN), 0);
      lv_obj_set_style_bg_opa(btn_servo, LV_OPA_50, 0);
    } else {
      lv_obj_add_flag(panel_servo, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_bg_opa(btn_servo, LV_OPA_TRANSP, 0);
    }
    // Force a redraw to resize the image/stream area
    if (is_streaming && !stream_paused) {
      // Stream will naturally resize on next frame
    } else {
      displayMultiImageOrText();
    }
  }, LV_EVENT_CLICKED, NULL);

  lv_obj_t * lbl_srv = lv_label_create(btn_servo);
  lv_label_set_text(lbl_srv, "S");
  lv_obj_center(lbl_srv);
  lv_obj_add_flag(btn_servo, LV_OBJ_FLAG_HIDDEN); // Hidden by default

  label_ram_multi = lv_label_create(top_panel_multi);
  lv_label_set_text(label_ram_multi, "RAM: --");
  lv_obj_set_style_text_color(label_ram_multi, lv_color_white(), 0);
  lv_obj_align(label_ram_multi, LV_ALIGN_RIGHT_MID, -45, 0);

  createWiFiIcon(top_panel_multi, 4);

  label_notify_multi = lv_label_create(top_panel_multi);
  lv_label_set_text(label_notify_multi, "Tap to capture");
  lv_obj_set_style_text_color(label_notify_multi, lv_color_white(), 0);
  lv_obj_align(label_notify_multi, LV_ALIGN_CENTER, 0, 0);

  // Body container (Image View)
  lv_obj_t * body_panel_multi = lv_obj_create(scr_multi);
  lv_obj_set_size(body_panel_multi, screenWidth, screenHeight - 30);
  lv_obj_align(body_panel_multi, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_pad_all(body_panel_multi, 0, 0);
  lv_obj_set_style_border_width(body_panel_multi, 0, 0);
  lv_obj_set_style_radius(body_panel_multi, 0, 0);
  lv_obj_set_style_bg_color(body_panel_multi, lv_color_hex(0x202020), 0);
  lv_obj_set_style_bg_opa(body_panel_multi, 0, 0); // Transparent to avoid overlapping JPEG

  // Servo Slider Panel (Bottom)
  panel_servo = lv_obj_create(scr_multi);
  lv_obj_set_size(panel_servo, screenWidth, 40);
  lv_obj_align(panel_servo, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(panel_servo, lv_color_hex(0x303030), 0);
  lv_obj_set_style_pad_all(panel_servo, 5, 0);
  lv_obj_set_style_border_width(panel_servo, 0, 0);
  lv_obj_set_style_radius(panel_servo, 0, 0);
  lv_obj_add_flag(panel_servo, LV_OBJ_FLAG_HIDDEN); // Hidden by default

  sld_servo = lv_slider_create(panel_servo);
  lv_obj_set_size(sld_servo, 400, 15);
  lv_obj_align(sld_servo, LV_ALIGN_CENTER, -20, 0);
  lv_slider_set_range(sld_servo, 0, 180);
  lv_slider_set_value(sld_servo, 90, LV_ANIM_OFF);

  label_servo_val = lv_label_create(panel_servo);
  lv_label_set_text(label_servo_val, "90°");
  lv_obj_set_style_text_color(label_servo_val, lv_color_white(), 0);
  lv_obj_align(label_servo_val, LV_ALIGN_RIGHT_MID, 0, 0);

  lv_obj_add_event_cb(sld_servo, [](lv_event_t *e) {
    int val = lv_slider_get_value(lv_event_get_target(e));
    lv_label_set_text_fmt(label_servo_val, "%d°", val);
    
    // Throttled UDP Send (every 50ms)
    if (millis() - last_udp_send_time > 50 && multiTargetIP != "Select IP") {
      last_udp_send_time = millis();
      udp.beginPacket(multiTargetIP.c_str(), udpPort);
      udp.write((uint8_t)val);
      udp.endPacket();
    }
  }, LV_EVENT_VALUE_CHANGED, NULL);
}

void buildConfigScreen() {
  // Top Section (Information)
  lv_obj_t * top_panel_cfg = lv_obj_create(scr_config);
  lv_obj_set_size(top_panel_cfg, screenWidth, 30);
  lv_obj_align(top_panel_cfg, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_pad_all(top_panel_cfg, 0, 0);
  lv_obj_set_style_border_width(top_panel_cfg, 0, 0);
  lv_obj_set_style_radius(top_panel_cfg, 0, 0);
  lv_obj_set_style_bg_color(top_panel_cfg, lv_palette_main(LV_PALETTE_BLUE_GREY), 0);
  lv_obj_clear_flag(top_panel_cfg, LV_OBJ_FLAG_SCROLLABLE);

  // Back Button
  lv_obj_t * btn_back_cfg = lv_btn_create(top_panel_cfg);
  lv_obj_set_size(btn_back_cfg, 100, 30);
  lv_obj_align(btn_back_cfg, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_bg_opa(btn_back_cfg, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(btn_back_cfg, 0, 0);
  lv_obj_add_event_cb(btn_back_cfg, [](lv_event_t *e) { switchScreen(4); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t * lbl_back_cfg = lv_label_create(btn_back_cfg);
  lv_label_set_text(lbl_back_cfg, LV_SYMBOL_LEFT " Config");
  lv_obj_set_style_text_color(lbl_back_cfg, lv_color_white(), 0);
  lv_obj_center(lbl_back_cfg);

  label_config_notify = lv_label_create(top_panel_cfg);
  lv_label_set_text(label_config_notify, "");
  lv_obj_set_style_text_color(label_config_notify, lv_color_white(), 0);
  lv_obj_align(label_config_notify, LV_ALIGN_CENTER, 0, 0);

  label_config_ip = lv_label_create(top_panel_cfg);
  lv_label_set_text(label_config_ip, "");
  lv_obj_set_style_text_color(label_config_ip, lv_color_white(), 0);
  lv_obj_align(label_config_ip, LV_ALIGN_RIGHT_MID, -10, 0);

  // Body container (Scrollable)
  lv_obj_t * cont = lv_obj_create(scr_config);
  lv_obj_set_size(cont, screenWidth, screenHeight - 30); // Full width now that side buttons are gone
  lv_obj_align(cont, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START); // Center items horizontally
  lv_obj_set_style_pad_all(cont, 10, 0);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_radius(cont, 0, 0);
  lv_obj_set_style_bg_color(cont, lv_color_hex(0x202020), 0); // Dark grey background
  lv_obj_set_style_text_color(cont, lv_color_white(), 0);

  // Apply button inside the container at the top
  lv_obj_t * btn_apply = lv_btn_create(cont);
  lv_obj_set_size(btn_apply, 440, 40); // Expanded to fit screenWidth (minus padding)
  lv_obj_t * lbl_apply = lv_label_create(btn_apply);
  lv_label_set_text(lbl_apply, "Apply Settings");
  lv_obj_center(lbl_apply);
  lv_obj_add_event_cb(btn_apply, [](lv_event_t *e) { sendConfigChanges(); }, LV_EVENT_CLICKED, NULL);

  create_slider(cont, "Capture Framesize", 0, 13, &sld_framesize);
  create_slider(cont, "Stream Framesize", 0, 13, &sld_stream_framesize);
  create_slider(cont, "Quality", 0, 63, &sld_quality);
  create_slider(cont, "Brightness", -2, 2, &sld_brightness);
  create_slider(cont, "Contrast", -2, 2, &sld_contrast);
  create_slider(cont, "Saturation", -2, 2, &sld_saturation);
  create_slider(cont, "LED Flash", 0, 255, &sld_led);
  
  auto res_cb = [](lv_event_t * e) {
      lv_obj_t * s = lv_event_get_target(e);
      lv_obj_t * v = (lv_obj_t *)lv_event_get_user_data(e);
      int idx = lv_slider_get_value(s);
      if (idx >= 0 && idx < 30 && res_names_config[idx] != "") {
        lv_label_set_text(v, res_names_config[idx].c_str());
      } else {
        lv_label_set_text_fmt(v, "%d", idx);
      }
  };

  lv_obj_add_event_cb(sld_framesize, res_cb, LV_EVENT_VALUE_CHANGED, lv_obj_get_child(lv_obj_get_parent(sld_framesize), 2));
  lv_obj_add_event_cb(sld_stream_framesize, res_cb, LV_EVENT_VALUE_CHANGED, lv_obj_get_child(lv_obj_get_parent(sld_stream_framesize), 2));

  // Custom Callback for LED Flash to show percentage
  lv_obj_add_event_cb(sld_led, [](lv_event_t * e) {
      lv_obj_t * s = lv_event_get_target(e);
      lv_obj_t * v = (lv_obj_t *)lv_event_get_user_data(e);
      int val = lv_slider_get_value(s);
      lv_label_set_text_fmt(v, "%d%%", (val * 100) / 255);
  }, LV_EVENT_VALUE_CHANGED, lv_obj_get_child(lv_obj_get_parent(sld_led), 2));

  create_switch(cont, "AWB", &sw_awb);
  create_switch(cont, "AEC", &sw_aec);
  create_switch(cont, "AGC", &sw_agc);
  create_switch(cont, "Mirror", &sw_hmirror);
  create_switch(cont, "Flip", &sw_vflip);
}

void connectToWiFi() {
  Serial.printf("Connecting to WiFi: %s\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  
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

    // Initialize mDNS
    if (MDNS.begin("gateway")) {
      Serial.println("mDNS responder started: http://gateway.local");
      MDNS.addService("http", "tcp", 80);
    }

    // Initialize NTP (Jakarta UTC+7)
    Serial.println("[NTP] Synchronizing time...");
    configTime(25200, 0, "pool.ntp.org", "time.nist.gov");
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
  if (WiFi.status() != WL_CONNECTED || configTargetIP == "") return;
  String url = "http://" + configTargetIP + "/control?var=" + String(var) + "&val=" + String(val);
  http.begin(url);
  http.setTimeout(5000);
  http.GET();
  http.end();
}

void setXCLK(int xclk) {
  if (WiFi.status() != WL_CONNECTED || configTargetIP == "") return;
  String url = "http://" + configTargetIP + "/xclk?xclk=" + String(xclk);
  http.begin(url);
  http.setTimeout(5000);
  http.GET();
  http.end();
}

void handleStatus() {
  if (WiFi.status() != WL_CONNECTED) {
    server.send(503, "application/json", "{\"error\": \"WiFi disconnected\"}");
    return;
  }
  if (configTargetIP == "") {
    server.send(400, "application/json", "{\"error\": \"No target camera selected\"}");
    return;
  }
  String url = "http://" + configTargetIP + "/status";
  http.begin(url);
  http.setTimeout(5000);
  int httpCode = http.GET();
  if (httpCode == 200) {
    server.send(200, "application/json", http.getString());
  } else {
    server.send(httpCode, "application/json", "{\"error\": \"Status fetch error\"}");
  }
  http.end();
}

void handleDevices() {
  String json = "[";
  bool first = true;
  for (int i = 0; i < 5; i++) {
    if (devices[i].mac != "" && devices[i].ip != "") {
      if (!first) json += ",";
      json += "{\"mac\":\"" + devices[i].mac + "\",\"ip\":\"" + devices[i].ip + "\"}";
      first = false;
    }
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleRegister() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }
  String body = server.arg("plain");
  if (body == "") {
    server.send(400, "application/json", "{\"error\": \"Empty body\"}");
    return;
  }
  String mac = getJsonStr(body, "mac");
  if (mac == "") {
    server.send(400, "application/json", "{\"error\": \"Missing mac\"}");
    return;
  }
  int rssi = getJsonVal(body, "rssi");
  if (rssi == -999) rssi = -100; // Default to weak if not provided
  
  String clientIP = server.client().remoteIP().toString();
  bool found = false;
  
  for (int i = 0; i < 5; i++) {
    if (devices[i].mac == mac) {
      found = true;
      devices[i].rssi = rssi;
      if (devices[i].ip != clientIP) {
        devices[i].ip = clientIP;
      }
      break;
    }
  }
  
  if (!found) {
    // Find empty slot
    bool added = false;
    for (int i = 0; i < 5; i++) {
      if (devices[i].mac == "") {
        devices[i].mac = mac;
        devices[i].ip = clientIP;
        devices[i].rssi = rssi;
        added = true;
        break;
      }
    }
    if (!added) {
      server.send(507, "application/json", "{\"error\": \"Device limit reached\"}");
      return;
    }
  }
  
  server.send(200, "application/json", "{\"status\": \"ok\"}");
}

void handleRoot() {
  if (sdAvailable) {
    File file = SD.open("/index.html", FILE_READ);
    if (file && !file.isDirectory()) {
      Serial.println("[HTTP] Serving index.html from SD Card.");
      server.streamFile(file, "text/html");
      file.close();
      return;
    }
  }

  // Fallback if SD card is missing or file not found
  Serial.println("[HTTP] SD index.html missing.");
  server.send(404, "text/plain", "Error: /index.html not found on SD card.");
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

String getArchiveFilename() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10)) {
    char buffer[32];
    strftime(buffer, sizeof(buffer), "/%d-%m-%y-%H-%M-%S.jpg", &timeinfo);
    return String(buffer);
  }

  // Fallback to JSON index
  Serial.println("[SD] NTP time unavailable, using index fallback.");
  int index = 0;
  StaticJsonDocument<128> doc;
  
  File file = SD.open("/image_info.json", FILE_READ);
  if (file) {
    deserializeJson(doc, file);
    index = doc["index"] | 0;
    file.close();
  }
  
  index++;
  doc["index"] = index;
  
  file = SD.open("/image_info.json", FILE_WRITE);
  if (file) {
    serializeJson(doc, file);
    file.close();
  }
  
  return "/" + String(index) + ".jpg";
}

void processStatistic(bool increment) {
  if (!sdAvailable) return;
  
  StaticJsonDocument<512> doc;
  File file = SD.open("/statistic.json", FILE_READ);
  if (file) {
    deserializeJson(doc, file);
    file.close();
  } else {
    // Default values if file doesn't exist
    doc["today_dd"] = 0;
    doc["today"] = 0;
    doc["1"] = 0; doc["2"] = 0; doc["3"] = 0; doc["4"] = 0;
    doc["5"] = 0; doc["6"] = 0; doc["7"] = 0;
  }
  
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10)) {
    int current_day = timeinfo.tm_mday;
    int saved_day = doc["today_dd"] | 0;
    
    if (saved_day != 0 && saved_day != current_day) {
      Serial.println("[STATS] Date change detected. Shifting values...");
      doc["7"] = doc["6"] | 0;
      doc["6"] = doc["5"] | 0;
      doc["5"] = doc["4"] | 0;
      doc["4"] = doc["3"] | 0;
      doc["3"] = doc["2"] | 0;
      doc["2"] = doc["1"] | 0;
      doc["1"] = doc["today"] | 0;
      doc["today"] = 0;
    }
    doc["today_dd"] = current_day;
  }
  
  if (increment) {
    int count = doc["today"] | 0;
    doc["today"] = count + 1;
    Serial.printf("[STATS] Capture incremented. Today: %d\n", (int)doc["today"]);
  }
  
  file = SD.open("/statistic.json", FILE_WRITE);
  if (file) {
    serializeJson(doc, file);
    file.close();
  }
}

bool captureImage(String targetIP, const char* path, const char* path2) {
  if (!sdAvailable) {
    Serial.println("[ERR] Capture aborted: SD card not available.");
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERR] Capture failed: WiFi disconnected.");
    return false;
  }

  String url = "http://" + targetIP + "/capture";
  Serial.printf("[INFO] Fetching image from: %s to %s\n", url.c_str(), path);

  // Set initial status
  lv_obj_t* startLabel = (strcmp(path, IMAGE_PATH) == 0) ? label_status : label_notify_multi;
  if (startLabel) {
    lv_label_set_text(startLabel, "Capturing...");
    lv_timer_handler();
  }

  http.begin(url);
  http.setTimeout(CAPTURE_TIMEOUT_MS);
  int httpCode = http.GET();

  if (httpCode == 200) {
    int contentLength = http.getSize();
    Serial.printf("[INFO] Image size: %d bytes\n", contentLength);

    File file = SD.open(path, FILE_WRITE);
    if (!file) {
      Serial.println("[ERR] Failed to open primary SD file for writing.");
      http.end();
      return false;
    }

    File file2;
    bool hasArchive = (path2 != NULL);
    if (hasArchive) {
      file2 = SD.open(path2, FILE_WRITE);
      if (!file2) {
        Serial.println("[WARN] Failed to open archive SD file for writing. Proceeding with primary only.");
        hasArchive = false;
      } else {
        Serial.printf("[INFO] Archiving copy to: %s\n", path2);
      }
    }

    WiFiClient * stream = http.getStreamPtr();
    int bytesDownloaded = 0;
    uint32_t startMs = millis();

    while (http.connected() && (bytesDownloaded < contentLength || contentLength == -1)) {
      size_t available = stream->available();
      if (available > 0) {
        // Limit download chunk to 4KB for stability as requested by user
        size_t maxChunk = 4096;
        size_t readLen = stream->readBytes(chunkBuffer, min(available, maxChunk));
        file.write(chunkBuffer, readLen);
        if (hasArchive) file2.write(chunkBuffer, readLen);
        bytesDownloaded += readLen;
        
        // Update download progress UI
        lv_obj_t* targetLabel = (strcmp(path, IMAGE_PATH) == 0) ? label_status : label_notify_multi;
        if (targetLabel) {
          static uint32_t last_down_ui = 0;
          if (millis() - last_down_ui > 150) {
            lv_label_set_text_fmt(targetLabel, "DOWN: %d/%dk", bytesDownloaded/1024, contentLength/1024);
            lv_timer_handler();
            last_down_ui = millis();
          }
        }
        
        startMs = millis(); // Reset timeout on activity
      }
      
      if (millis() - startMs > CAPTURE_TIMEOUT_MS) {
        Serial.println("[ERR] Capture timeout during download.");
        break;
      }
      lv_timer_handler(); // Keep UI alive
      delay(1);
    }

    file.close();
    if (hasArchive) file2.close();
    http.end();

    if (bytesDownloaded > 0 && (contentLength == -1 || bytesDownloaded == contentLength)) {
      Serial.printf("[EVENT] Save complete. Total: %d bytes saved.\n", bytesDownloaded);
      if (strcmp(path, IMAGE_PATH) == 0) {
        sdImageReady = true;
        processStatistic(true); // Increment capture count for primary capture
      }
      else if (strcmp(path, IMAGE_PATH_MULTI) == 0) sdMultiImageReady = true;
      return true;
    } else {
      Serial.printf("[ERR] Download incomplete. Got %d of %d\n", bytesDownloaded, contentLength);
      SD.remove(path); // Clean up partial file
      if (hasArchive) SD.remove(path2);
      if (strcmp(path, IMAGE_PATH) == 0) sdImageReady = false;
      else if (strcmp(path, IMAGE_PATH_MULTI) == 0) sdMultiImageReady = false;
      return false;
    }
  } else {
    Serial.printf("[ERR] HTTP request failed, code: %d\n", httpCode);
    http.end();
    return false;
  }
}

void handleCapture() {
  if (!server.hasArg("ip")) {
    server.send(400, "application/json", "{\"error\": \"Missing ip param\"}");
    return;
  }
  
  lastGlobalIP = server.arg("ip");
  switchScreen(0);
  runGlobalCapture();
  
  if (sdAvailable && sdImageReady) {
    server.send(200, "application/json", "{\"status\": \"ok\", \"path\": \"" + String(IMAGE_PATH) + "\"}");
  } else {
    server.send(500, "application/json", "{\"error\": \"Capture fail\"}");
  }
}

void telegramUploadTask(void *pvParameters) {
  Serial.println("[TASK] Telegram upload task started.");
  tele_progress_bytes = 0;
  tele_total_bytes = 0;
  tele_updating = true;
  sendPhotoToTelegram(targetChatId, IMAGE_PATH);
  tele_updating = false;
  Serial.println("[TASK] Telegram upload task finished.");
  telegramTaskHandle = NULL;
  vTaskDelete(NULL);
}

void runGlobalCapture() {
  if (lastGlobalIP == "") {
    return;
  }
  
  String archivePath = getArchiveFilename();
  if (captureImage(lastGlobalIP, IMAGE_PATH, archivePath.c_str())) {
    displayImageOrText();
    lv_label_set_text(label_status, lastGlobalIP.c_str());
    playCaptureBeep();
    
    // Upload to Telegram in background
    Serial.println("External trigger: Starting Telegram background task...");
    if (telegramTaskHandle == NULL) {
      xTaskCreatePinnedToCore(
        telegramUploadTask,   // Task function
        "TelegramTask",       // Task name
        5120,                 // Stack size (5KB optimized)
        NULL,                 // Parameters
        1,                    // Priority
        &telegramTaskHandle,  // Task handle
        0                     // Core 0 (Network)
      );
    }
  }
  notify_done_time = millis();
  if (notify_done_time == 0) notify_done_time = 1;
}

bool sendPhotoToTelegram(String chatId, const char* path) {
  if (!sdAvailable) return false;
  
  const char* host = "api.telegram.org";
  const int port = 443;
  int maxRetries = 3;
  
  for (int attempt = 1; attempt <= maxRetries; attempt++) {
    File file = SD.open(path, FILE_READ);
    if (!file) {
      Serial.printf("[ERR] Telegram: Could not open %s\n", path);
      return false;
    }
    size_t imageSize = file.size();
    tele_total_bytes = imageSize; // Set total early for UI

    Serial.printf("\n--- Telegram Upload Attempt %d/%d ---\n", attempt, maxRetries);
    
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(30000);
    
    Serial.printf("Connecting to %s:%d...\n", host, port);
    if (!client.connect(host, port)) {
      Serial.println("[ERR] Telegram: Connection failed.");
      file.close();
      if (attempt < maxRetries) {
        delay(2000);
        continue;
      }
      return false;
    }

    String boundary = "----ESP32Boundary" + String(millis());
    String head = "--" + boundary + "\r\n"
                + "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
                + chatId + "\r\n"
                + "--" + boundary + "\r\n"
                + "Content-Disposition: form-data; name=\"photo\"; filename=\"image.jpg\"\r\n"
                + "Content-Type: image/jpeg\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";
    
    uint32_t totalLength = head.length() + imageSize + tail.length();
    
    client.println("POST /bot" + botToken + "/sendPhoto HTTP/1.1");
    client.println("Host: " + String(host));
    client.println("Content-Length: " + String(totalLength));
    client.println("Content-Type: multipart/form-data; boundary=" + boundary);
    client.println();
    
    client.print(head);
    
    bool uploadFailed = false;
    size_t bytesSent = 0;
    while (file.available()) {
      size_t toRead = min((size_t)file.available(), sizeof(telegramChunkBuffer));
      file.read(telegramChunkBuffer, toRead);
      if (client.write(telegramChunkBuffer, toRead) != toRead) {
        uploadFailed = true;
        break;
      }
      bytesSent += toRead;
      tele_progress_bytes = bytesSent; // Core 0 update
      delay(1);
    }
    file.close();
    
    if (uploadFailed) {
      client.stop();
      if (attempt < maxRetries) {
        delay(2000);
        continue;
      }
      return false;
    }
    
    client.print(tail);
    
    String response = "";
    uint32_t startMs = millis();
    while (client.connected() && millis() - startMs < 15000) {
      if (client.available()) {
        response += (char)client.read();
        startMs = millis();
      }
      delay(1);
    }
    client.stop();

    if (response.indexOf("\"ok\":true") > 0) {
      Serial.println("[OK] Telegram: Photo sent successfully.");
      return true;
    } else {
      Serial.println("[ERR] Telegram: Server returned error.");
      if (attempt < maxRetries) {
        delay(2000);
        continue;
      }
    }
  }
  return false;
}

void handleCaptureMulti() {
  if (captureMultiImage()) {
    displayMultiImageOrText();
    lv_label_set_text(label_notify_multi, "Done");
  } else {
    lv_label_set_text(label_notify_multi, "Error");
  }
  notify_done_time = millis();
  if (notify_done_time == 0) notify_done_time = 1;
}

void handleImage() {
  if (!sdAvailable || !sdImageReady) {
    server.send(404, "application/json", "{\"error\": \"No image on SD\"}");
    return;
  }
  
  File f = SD.open(IMAGE_PATH, FILE_READ);
  if (!f) {
    server.send(500, "application/json", "{\"error\": \"SD read fail\"}");
    return;
  }
  
  size_t fileSize = f.size();
  server.setContentLength(fileSize);
  server.sendHeader("Content-Type", "image/jpeg");
  server.send(200, "image/jpeg", ""); // Send header

  while (f.available()) {
    size_t toRead = min((size_t)f.available(), sizeof(chunkBuffer));
    f.read(chunkBuffer, toRead);
    server.sendContent((const char*)chunkBuffer, toRead);
  }
  f.close();
}

bool getJpgSize(const uint8_t* data, size_t len, uint16_t *w, uint16_t *h) {
  size_t i = 0;
  if (data[0] != 0xFF || data[1] != 0xD8) return false;
  i = 2;
  while (i < len - 8) {
    if (data[i] == 0xFF) {
      uint8_t marker = data[i+1];
      if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) { // SOF markers
        *h = (data[i+5] << 8) | data[i+6];
        *w = (data[i+7] << 8) | data[i+8];
        return true;
      }
      i += 2 + ((data[i+2] << 8) | data[i+3]);
    } else {
      i++;
    }
  }
  return false;
}

// Custom DataWrapper for SD reading with progress reporting
class SDProgressWrapper : public lgfx::DataWrapper {
public:
  File file;
  lv_obj_t* label;
  size_t total;
  size_t read_bytes;
  uint32_t last_ui;

  SDProgressWrapper(const char* path, lv_obj_t* l) : label(l), read_bytes(0), last_ui(0) {
    file = SD.open(path, FILE_READ);
    total = file ? file.size() : 0;
    need_transaction = true;
  }
  
  ~SDProgressWrapper() { if (file) file.close(); }

  int read(uint8_t* buf, uint32_t len) override {
    if (!file) return 0;
    size_t r = file.read(buf, len);
    read_bytes += r;
    if (label && total > 0) {
      if (millis() - last_ui > 150) {
        lv_label_set_text_fmt(label, "RNDR: %d/%dk", (int)(read_bytes/1024), (int)(total/1024));
        lv_timer_handler();
        last_ui = millis();
      }
    }
    return r;
  }

  void skip(int32_t len) override { if (file) file.seek(file.position() + len); }
  bool seek(uint32_t offset) override { return file ? file.seek(offset) : false; }
  void close() override { if (file) file.close(); }
  int32_t tell(void) override { return file ? file.position() : 0; }
};

void displayImageOrText() {
  if (current_screen != 0) return; // Only draw on image screen

  if (!sdAvailable || !sdImageReady) {
    tft.fillRect(0, 30, screenWidth, screenHeight - 30, tft.color565(32, 32, 32));
    lv_label_set_text(label_status, "Waiting...");
    return;
  }

  // Clear the image area
  tft.fillRect(0, 30, screenWidth, screenHeight - 30, tft.color565(32, 32, 32));

  File f = SD.open(IMAGE_PATH, FILE_READ);
  if (!f) {
    Serial.println("[ERR] displayImage: Could not open IMAGE_PATH");
    return;
  }

  // Read larger header to ensure we find SOF marker
  uint8_t header[1024]; 
  size_t readLen = f.read(header, sizeof(header));
  f.close();

  uint16_t img_w = 0, img_h = 0;
  if (getJpgSize(header, readLen, &img_w, &img_h)) {
    Serial.printf("[DEBUG] SD JPEG found: %dx%d\n", img_w, img_h);
    
    float target_w = screenWidth;
    float target_h = screenHeight - 30;
    float ratio_w = target_w / img_w;
    float ratio_h = target_h / img_h;
    float scale = (ratio_w < ratio_h) ? ratio_w : ratio_h;

    int32_t x_offset = (screenWidth - (img_w * scale)) / 2;
    int32_t y_offset = 30 + (target_h - (img_h * scale)) / 2;
    if (x_offset < 0) x_offset = 0;
    if (y_offset < 30) y_offset = 30;

    Serial.printf("[DEBUG] Render: x=%d, y=%d, scale=%.2f\n", x_offset, y_offset, scale);

    SDProgressWrapper wrapper(IMAGE_PATH, label_status);
    if (wrapper.file) {
      if (!tft.drawJpg(&wrapper, x_offset, y_offset, 0, 0, 0, 0, scale, scale)) {
        Serial.println("[ERR] drawJpg failed!");
      }
    } else {
      Serial.println("[ERR] Could not open IMAGE_PATH for rendering.");
    }
    
    lv_label_set_text_fmt(label_status, "Cam: %s", lastGlobalIP.c_str());
  } else {
    Serial.println("[ERR] getJpgSize failed to parse SD file header.");
  }

  // Force LVGL to redraw the top header on top of the image
  lv_obj_invalidate(top_panel); 
  lv_timer_handler();

  // Draw Nav UI elements after the image to keep them on top
  tft.drawRoundRect(5, 110, 30, 100, 5, TFT_WHITE);
  tft.drawRoundRect(screenWidth - 35, 110, 30, 100, 5, TFT_WHITE);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(12, 150);
  tft.print("<");
  tft.setCursor(screenWidth - 25, 150);
  tft.print(">");
}

bool captureMultiImage() {
  if (multiTargetIP == "Select IP") return false;
  return captureImage(multiTargetIP, IMAGE_PATH_MULTI);
}

void displayMultiImageOrText() {
  if (current_screen != 4) return;

  if (!sdAvailable || !sdMultiImageReady) {
    tft.fillRect(0, 30, screenWidth, screenHeight - 30, tft.color565(32, 32, 32));
    return;
  }

  int available_h = screenHeight - 30 - (servo_control_active ? 40 : 0);
  tft.fillRect(0, 30, screenWidth, screenHeight - 30, tft.color565(32, 32, 32));

  File f = SD.open(IMAGE_PATH_MULTI, FILE_READ);
  if (!f) {
    Serial.println("[ERR] displayMulti: Could not open IMAGE_PATH_MULTI");
    return;
  }

  uint8_t header[1024]; 
  size_t readLen = f.read(header, sizeof(header));
  f.close();

  uint16_t img_w = 0, img_h = 0;
  if (getJpgSize(header, readLen, &img_w, &img_h)) {
    Serial.printf("[DEBUG] SD Multi JPEG found: %dx%d\n", img_w, img_h);

    float ratio_w = (float)screenWidth / img_w;
    float ratio_h = (float)available_h / img_h;
    float scale = (ratio_w < ratio_h) ? ratio_w : ratio_h;

    int32_t x_offset = (screenWidth - (img_w * scale)) / 2;
    int32_t y_offset = 30 + (available_h - (img_h * scale)) / 2;
    if (x_offset < 0) x_offset = 0;
    if (y_offset < 30) y_offset = 30;

    Serial.printf("[DEBUG] Multi Render: x=%d, y=%d, scale=%.2f\n", x_offset, y_offset, scale);

    SDProgressWrapper wrapper(IMAGE_PATH_MULTI, label_notify_multi);
    if (wrapper.file) {
      if (!tft.drawJpg(&wrapper, x_offset, y_offset, 0, 0, 0, 0, scale, scale)) {
        Serial.println("[ERR] drawJpg (Multi) failed!");
      }
    } else {
      Serial.println("[ERR] Could not open IMAGE_PATH_MULTI for rendering.");
    }
  } else {
    Serial.println("[ERR] getJpgSize failed to parse Multi SD file header.");
  }
  lv_obj_invalidate(top_panel_multi);
  lv_timer_handler();

  tft.drawRoundRect(5, 110, 30, 100, 5, TFT_WHITE);
  tft.drawRoundRect(screenWidth - 35, 110, 30, 100, 5, TFT_WHITE);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(12, 150);
  tft.print("<");
  tft.setCursor(screenWidth - 25, 150);
  tft.print(">");
}


void buildIpSelectScreen() {
  // Top Section
  lv_obj_t * top_panel_ip = lv_obj_create(scr_ip_select);
  lv_obj_set_size(top_panel_ip, screenWidth, 30);
  lv_obj_align(top_panel_ip, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_pad_all(top_panel_ip, 0, 0);
  lv_obj_set_style_border_width(top_panel_ip, 0, 0);
  lv_obj_set_style_radius(top_panel_ip, 0, 0);
  lv_obj_set_style_bg_color(top_panel_ip, lv_palette_main(LV_PALETTE_BLUE_GREY), 0);
  lv_obj_clear_flag(top_panel_ip, LV_OBJ_FLAG_SCROLLABLE);

  // Back Button
  lv_obj_t * btn_back = lv_btn_create(top_panel_ip);
  lv_obj_set_size(btn_back, 80, 28);
  lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 5, 0);
  lv_obj_set_style_bg_opa(btn_back, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(btn_back, 1, 0);
  lv_obj_set_style_border_color(btn_back, lv_color_white(), 0);
  lv_obj_set_style_shadow_width(btn_back, 0, 0);
  lv_obj_add_event_cb(btn_back, [](lv_event_t *e) { switchScreen(4); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t * lbl_back = lv_label_create(btn_back);
  lv_label_set_text(lbl_back, LV_SYMBOL_LEFT " Back");
  lv_obj_center(lbl_back);

  label_ip_title = lv_label_create(top_panel_ip);
  if (ip_reloaded) {
    lv_label_set_text(label_ip_title, "Table refreshed");
    ip_notify_time = millis();
    if (ip_notify_time == 0) ip_notify_time = 1;
    ip_reloaded = false;
  } else {
    lv_label_set_text(label_ip_title, "Select Camera IP");
  }
  lv_obj_set_style_text_color(label_ip_title, lv_color_white(), 0);
  lv_obj_align(label_ip_title, LV_ALIGN_CENTER, 20, 0);

  // Reload Button
  lv_obj_t * btn_reload = lv_btn_create(top_panel_ip);
  lv_obj_set_size(btn_reload, 35, 28);
  lv_obj_align(btn_reload, LV_ALIGN_RIGHT_MID, -5, 0);
  lv_obj_set_style_bg_opa(btn_reload, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(btn_reload, 1, 0);
  lv_obj_set_style_border_color(btn_reload, lv_color_white(), 0);
  lv_obj_set_style_shadow_width(btn_reload, 0, 0);
  lv_obj_add_event_cb(btn_reload, [](lv_event_t *e) { 
    ip_reloaded = true;
    switchScreen(5); 
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t * lbl_reload = lv_label_create(btn_reload);
  lv_label_set_text(lbl_reload, LV_SYMBOL_REFRESH);
  lv_obj_center(lbl_reload);

  // Body container (Scrollable)
  lv_obj_t * cont = lv_obj_create(scr_ip_select);
  lv_obj_set_size(cont, 400, screenHeight - 30);
  lv_obj_align(cont, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(cont, 10, 0);
  lv_obj_set_style_pad_row(cont, 5, 0);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_radius(cont, 0, 0);
  lv_obj_set_style_bg_color(cont, lv_color_hex(0x202020), 0);

  // Header Row
  lv_obj_t * row_hdr = lv_obj_create(cont);
  lv_obj_set_size(row_hdr, 380, 30);
  lv_obj_set_style_bg_color(row_hdr, lv_palette_main(LV_PALETTE_BLUE_GREY), 0);
  lv_obj_set_style_border_width(row_hdr, 0, 0);
  lv_obj_set_style_radius(row_hdr, 3, 0);
  lv_obj_set_style_pad_all(row_hdr, 0, 0);
  lv_obj_set_flex_flow(row_hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row_hdr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  auto add_hdr_col = [](lv_obj_t* parent, const char* txt, int w) {
    lv_obj_t * lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_width(lbl, w);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  };

  add_hdr_col(row_hdr, "IP Address", 200);
  add_hdr_col(row_hdr, "Sel | Stream", 140);

  // Rows
  for (int i = 0; i < 5; i++) {
    if (devices[i].ip != "") {
      lv_obj_t * row = lv_obj_create(cont);
      lv_obj_set_size(row, 380, 45);
      lv_obj_set_style_bg_color(row, lv_color_hex(0x303030), 0);
      lv_obj_set_style_border_width(row, 0, 0);
      lv_obj_set_style_radius(row, 3, 0);
      lv_obj_set_style_pad_all(row, 0, 0);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

      lv_obj_t * lbl_ip = lv_label_create(row);
      lv_label_set_text(lbl_ip, devices[i].ip.c_str());
      lv_obj_set_style_text_color(lbl_ip, lv_color_white(), 0);
      lv_obj_set_width(lbl_ip, 200);
      lv_obj_set_style_text_align(lbl_ip, LV_TEXT_ALIGN_CENTER, 0);

      lv_obj_t * btn_sel = lv_btn_create(row);
      lv_obj_set_size(btn_sel, 65, 30);
      lv_obj_set_style_bg_color(btn_sel, lv_palette_main(LV_PALETTE_BLUE), 0);
      lv_obj_t * lbl_btn = lv_label_create(btn_sel);
      lv_label_set_text(lbl_btn, "SELECT");
      lv_obj_center(lbl_btn);

      lv_obj_set_user_data(btn_sel, (void*)devices[i].ip.c_str());
      lv_obj_add_event_cb(btn_sel, [](lv_event_t *e) {
        const char * ip = (const char *)lv_obj_get_user_data(lv_event_get_target(e));
        multiTargetIP = String(ip);
        is_streaming = false;
        switchScreen(4);
      }, LV_EVENT_CLICKED, NULL);

      lv_obj_t * btn_stream = lv_btn_create(row);
      lv_obj_set_size(btn_stream, 65, 30);
      if (is_streaming && multiTargetIP == devices[i].ip) {
        lv_obj_set_style_bg_color(btn_stream, lv_palette_main(LV_PALETTE_GREEN), 0);
      } else {
        lv_obj_set_style_bg_color(btn_stream, lv_palette_main(LV_PALETTE_GREY), 0);
      }
      lv_obj_t * lbl_strm = lv_label_create(btn_stream);
      lv_label_set_text(lbl_strm, "STREAM");
      lv_obj_center(lbl_strm);

      lv_obj_set_user_data(btn_stream, (void*)devices[i].ip.c_str());
      lv_obj_add_event_cb(btn_stream, [](lv_event_t *e) {
        const char * ip = (const char *)lv_obj_get_user_data(lv_event_get_target(e));
        if (multiTargetIP == String(ip)) {
          is_streaming = !is_streaming;
        } else {
          multiTargetIP = String(ip);
          is_streaming = true;
        }
        switchScreen(4);
      }, LV_EVENT_CLICKED, NULL);
    }
  }
}

// ==========================================
// Stream Management Functions
// ==========================================

String streamReadLine(uint32_t timeoutMs) {
  String s;
  s.reserve(64);
  uint32_t deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    if (streamClient.available()) {
      char c = (char)streamClient.read();
      if (c == '\n') break;
      if (c != '\r') s += c;
      deadline = millis() + timeoutMs;
    }
    lv_timer_handler(); // Process touch/UI while waiting
  }
  return s;
}

bool streamReadExact(uint8_t* dst, size_t len, uint32_t timeoutMs) {
  size_t got = 0;
  uint32_t deadline = millis() + timeoutMs;
  while (got < len && millis() < deadline) {
    int avail = streamClient.available();
    if (avail > 0) {
      size_t take = min((size_t)avail, len - got);
      streamClient.readBytes(dst + got, take);
      got += take;
      deadline = millis() + timeoutMs;
    }
    lv_timer_handler(); // Process touch/UI while waiting
  }
  return got == len;
}

void skipStreamHeaders() {
  while (streamClient.connected()) {
    if (streamReadLine().length() == 0) break;
  }
}

bool connectToStream() {
  streamClient.stop();
  if (multiTargetIP == "Select IP") return false;

  Serial.printf("Connecting to stream: %s:81\n", multiTargetIP.c_str());
  if (!streamClient.connect(multiTargetIP.c_str(), 81)) {
    Serial.println("Stream connection failed");
    return false;
  }

  streamClient.setNoDelay(true);
  streamClient.printf(
    "GET /stream HTTP/1.1\r\n"
    "Host: %s:81\r\n"
    "Connection: keep-alive\r\n"
    "\r\n",
    multiTargetIP.c_str());

  uint32_t t = millis();
  while (!streamClient.available()) {
    if (millis() - t > 5000) return false;
    delay(1);
  }

  skipStreamHeaders();
  Serial.println("Stream started");
  return true;
}

void stopStream() {
  streamClient.stop();
  Serial.println("Stream stopped");
}

size_t readStreamFrame() {
  size_t total = 0;
  while (streamClient.connected()) {
    String sizeLine = streamReadLine();
    sizeLine.trim();
    if (sizeLine.length() == 0) continue;

    size_t chunkSize = strtoul(sizeLine.c_str(), nullptr, 16);
    if (chunkSize == 0) { streamReadLine(); break; }

    if (total + chunkSize > sizeof(chunkBuffer)) return 0;
    if (!streamReadExact(chunkBuffer + total, chunkSize)) return 0;

    total += chunkSize;
    streamReadLine(); // trailing CRLF

    if (total >= 2 && chunkBuffer[total - 2] == 0xFF && chunkBuffer[total - 1] == 0xD9) break;
  }
  return total;
}

void processStream() {
  if (!streamClient.connected()) return;

  static uint16_t last_stream_w = 0;
  static uint16_t last_stream_h = 0;
  static bool last_servo_state = false;

  if (streamClient.available()) {
    String line = streamReadLine(50);
    line.trim();
    if (line.length() == 0) return;

    if (!line.startsWith("--")) {
      unsigned long maybeSize = strtoul(line.c_str(), nullptr, 16);
      if (maybeSize > 0 && maybeSize < 256) {
        line = streamReadLine(50);
        line.trim();
      }
      if (!line.startsWith("--")) return;
    }

    skipStreamHeaders();
    size_t frameLen = readStreamFrame();
    if (frameLen < 4) return;

    uint8_t* jpegStart = chunkBuffer;
    size_t jpegLen = frameLen;
    for (size_t i = 0; i < frameLen - 1; i++) {
      if (chunkBuffer[i] == 0xFF && chunkBuffer[i + 1] == 0xD8) {
        jpegStart = chunkBuffer + i;
        jpegLen = frameLen - i;
        break;
      }
    }

    uint16_t img_w = 0, img_h = 0;
    float scale = 1.0f;
    int available_h = screenHeight - 30 - (servo_control_active ? 40 : 0);

    if (getJpgSize(jpegStart, jpegLen, &img_w, &img_h)) {
      if (img_w != last_stream_w || img_h != last_stream_h || servo_control_active != last_servo_state) {
        last_stream_w = img_w;
        last_stream_h = img_h;
        last_servo_state = servo_control_active;
        tft.fillRect(0, 30, screenWidth, screenHeight - 30, tft.color565(32, 32, 32));
      }
      
      float ratio_w = (float)screenWidth / img_w;
      float ratio_h = (float)available_h / img_h;
      scale = (ratio_w < ratio_h) ? ratio_w : ratio_h;
      
      int32_t x_off = (screenWidth - (img_w * scale)) / 2;
      int32_t y_off = 30 + (available_h - (img_h * scale)) / 2;
      
      lv_timer_handler(); // Catch touch before drawing
      tft.drawJpg(jpegStart, jpegLen, x_off, y_off, 0, 0, 0, 0, scale, scale);
      lv_timer_handler(); // Catch touch after drawing
    }

    lv_obj_invalidate(top_panel_multi);
    if (servo_control_active) {
      if (panel_servo && !lv_obj_has_flag(panel_servo, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_invalidate(panel_servo);
      }
    }
  }
}

void playCaptureBeep() {
  for (int i = 0; i < 2; i++) {
    digitalWrite(buzzerPin, HIGH);
    uint32_t start = millis();
    while (millis() - start < 500) {
      lv_timer_handler();
      delay(1);
    }
    digitalWrite(buzzerPin, LOW);
    start = millis();
    while (millis() - start < 200) {
      lv_timer_handler();
      delay(1);
    }
  }
}

bool mountSD() {
  Serial.println("[SD] Powering on HSPI and mounting card...");
  // Initialize HSPI Pins: SCK=14, MISO=12, MOSI=13, CS=5
  sdSPI.begin(14, 12, 13, sdCS);
  
  if (SD.begin(sdCS, sdSPI)) {
    return true;
  }
  return false;
}

void unmountSD() {
  Serial.println("[SD] Unmounting and powering off HSPI...");
  SD.end();
  sdSPI.end(); // Stop the SPI hardware entirely

  // Explicitly hold CS HIGH to avoid floating bus state
  pinMode(sdCS, OUTPUT);
  digitalWrite(sdCS, HIGH);

  // Release HSPI data pins to INPUT to avoid bus contention during touch reads
  pinMode(12, INPUT); // MISO
  pinMode(13, INPUT); // MOSI
  pinMode(14, INPUT); // SCK

  Serial.println("[SD] Shutdown complete. HSPI pins released.");
}

bool loadConfig() {
  Serial.println("[SD] Loading config.json...");
  File file = SD.open("/config.json");
  if (!file) {
    Serial.println("[SD] Config file missing! Using default credentials.");
    return false;
  }

  // Use a buffer for JSON parsing
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.printf("[SD] JSON parse failed: %s. Using default credentials.\n", error.c_str());
    return false;
  }

  // Load variables safely if they exist in the JSON
  if (doc.containsKey("ssid")) {
    ssid = doc["ssid"].as<String>();
    Serial.printf("[SD] WiFi SSID loaded: %s\n", ssid.c_str());
  }
  if (doc.containsKey("password")) {
    password = doc["password"].as<String>();
    Serial.println("[SD] WiFi Password loaded.");
  }
  if (doc.containsKey("botToken")) {
    botToken = doc["botToken"].as<String>();
    Serial.println("[SD] Telegram Bot Token loaded.");
  }
  if (doc.containsKey("chatId")) {
    targetChatId = doc["chatId"].as<String>();
    Serial.printf("[SD] Telegram Chat ID loaded: %s\n", targetChatId.c_str());
  }

  return true;
}
