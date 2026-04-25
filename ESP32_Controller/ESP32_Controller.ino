#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// WiFi Configuration
const char* ssid = "BatuKhan";
const char* password = "momoygemoy";

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
    bcfg.freq_write = 65000000;
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
    tcfg.y_min = 3800;   // swapped
    tcfg.y_max = 300;    // swapped
    tcfg.pin_cs     = 21;
    tcfg.pin_int    = 27;
    tcfg.bus_shared = true;
    tcfg.spi_host   = VSPI_HOST;
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
};

DeviceInfo devices[5] = {
  {"", ""},
  {"", ""},
  {"", ""},
  {"", ""},
  {"", ""}
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
lv_obj_t * label_notify;
lv_obj_t * label_ram_multi;
lv_obj_t * label_notify_multi;
lv_obj_t * label_ram_stats;
lv_obj_t * label_ram_devices;
lv_obj_t * dd_cameras;

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
lv_obj_t * sw_awb;
lv_obj_t * sw_aec;
lv_obj_t * sw_agc;
lv_obj_t * sw_hmirror;
lv_obj_t * sw_vflip;

// Global flags
int current_screen = 0; // 0: Image, 1: Config
String configTargetIP = "";
String lastGlobalIP = "";
int lastImgW = 0;
int lastImgH = 0;
bool toggleHeader = false;
bool capture_requested = false;
bool capture_requested_multi = false;
uint32_t notify_done_time = 0;

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
void handleDevices();
void handleRegister();
const char* getHtmlUI();
void freeImageBuffer();
void captureImage(String targetIP);
void runGlobalCapture();
void displayImageOrText();
void updateRAMUsage();
void buildConfigScreen();
void buildStatsScreen();
void buildDevicesScreen();
void buildMultiScreen();
void fetchAndApplyConfig();
void sendConfigChanges();
void switchScreen(int scr_id);
void captureMultiImage();
void handleCaptureMulti();
void displayMultiImageOrText();

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

  // Register Touch Input Device
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touch_read;
  lv_indev_drv_register(&indev_drv);

  // Create Screens
  scr_image = lv_obj_create(NULL);
  lv_obj_set_style_pad_all(scr_image, 0, 0);

  // Top Section (Information)
  top_panel = lv_obj_create(scr_image);
  lv_obj_set_size(top_panel, screenWidth, 30);
  lv_obj_align(top_panel, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_pad_all(top_panel, 0, 0);
  lv_obj_set_style_border_width(top_panel, 0, 0);
  lv_obj_set_style_radius(top_panel, 0, 0);
  lv_obj_set_style_bg_color(top_panel, lv_palette_main(LV_PALETTE_BLUE_GREY), 0);

  // Body Section (Image View)
  lv_obj_t * body_panel = lv_obj_create(scr_image);
  lv_obj_set_size(body_panel, screenWidth, screenHeight - 30);
  lv_obj_align(body_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_pad_all(body_panel, 0, 0);
  lv_obj_set_style_border_width(body_panel, 0, 0);
  lv_obj_set_style_radius(body_panel, 0, 0);
  lv_obj_set_style_bg_color(body_panel, lv_color_hex(0x202020), 0); // Dark grey background

  scr_config = lv_obj_create(NULL);
  lv_obj_set_style_pad_all(scr_config, 0, 0);
  lv_obj_set_style_bg_color(scr_config, lv_color_hex(0x202020), 0); // Unified dark grey for entire screen area

  scr_stats = lv_obj_create(NULL);
  lv_obj_set_style_pad_all(scr_stats, 0, 0);
  lv_obj_set_style_bg_color(scr_stats, lv_color_hex(0x202020), 0);

  scr_devices = lv_obj_create(NULL);
  lv_obj_set_style_pad_all(scr_devices, 0, 0);
  lv_obj_set_style_bg_color(scr_devices, lv_color_hex(0x202020), 0);

  scr_multi = lv_obj_create(NULL);
  lv_obj_set_style_pad_all(scr_multi, 0, 0);
  lv_obj_set_style_bg_color(scr_multi, lv_color_hex(0x202020), 0);

  // Initialize UI components on Image Screen
  label_status = lv_label_create(top_panel);
  lv_label_set_text(label_status, "Ready...");
  lv_obj_set_style_text_color(label_status, lv_color_white(), 0);
  lv_obj_align(label_status, LV_ALIGN_LEFT_MID, 10, 0);

  label_ram = lv_label_create(top_panel);
  lv_label_set_text(label_ram, "RAM: --");
  lv_obj_set_style_text_color(label_ram, lv_color_white(), 0);
  lv_obj_align(label_ram, LV_ALIGN_RIGHT_MID, -10, 0);

  label_notify = lv_label_create(top_panel);
  lv_label_set_text(label_notify, "Tap to capture");
  lv_obj_set_style_text_color(label_notify, lv_color_white(), 0);
  lv_obj_align(label_notify, LV_ALIGN_CENTER, 0, 0);

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
    else switchScreen(0);
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
    else switchScreen(0);
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t * lbl_r = lv_label_create(nav_btn_right);
  lv_label_set_text(lbl_r, ">");
  lv_obj_set_style_text_color(lbl_r, lv_color_white(), 0);
  lv_obj_center(lbl_r);

  switchScreen(0);

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

  if (notify_done_time > 0 && millis() - notify_done_time > 2000) {
    lv_label_set_text(label_notify, "Tap to capture");
    notify_done_time = 0;
  }

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
        } else {
          capture_requested = true;
          lv_label_set_text(label_notify, "Capturing...");
          lv_timer_handler();
        }
      }    }
  } else if (current_screen == 4) { // Multi Camera manual touch handling
    if (is_touched && !was_touched) {
      if (y > 30) {
        if (x < 45 && y > 100 && y < 220) {
          switchScreen(0); // Left to Image
        } else if (x > screenWidth - 45 && y > 100 && y < 220) {
          switchScreen(2); // Right to Stats
        } else if (x > 60 && x < screenWidth - 60) {
          capture_requested_multi = true;
          lv_label_set_text(label_notify_multi, "Capturing...");
          lv_timer_handler();
        }
      }
    }
  }
  was_touched = is_touched;

  if (capture_requested) {
    capture_requested = false;
    runGlobalCapture();
  }

  if (capture_requested_multi) {
    capture_requested_multi = false;
    handleCaptureMulti();
  }
  
  if (notify_done_time > 0 && millis() - notify_done_time > 2000) {
    if (current_screen == 4) lv_label_set_text(label_notify_multi, "Tap to capture");
    else lv_label_set_text(label_notify, "Tap to capture");
    notify_done_time = 0;
  }
  
  delay(5);
}

void updateRAMUsage() {
  static uint32_t last_update = 0;
  if (millis() - last_update > 2000) {
    last_update = millis();
    uint32_t free_h = ESP.getFreeHeap();
    uint32_t total_h = ESP.getHeapSize();
    uint32_t used_h = total_h - free_h;
    if (current_screen == 4) {
      lv_label_set_text_fmt(label_ram_multi, "RAM: %u/%u KB", used_h/1024, total_h/1024);
    } else if (current_screen == 2) {
      lv_label_set_text_fmt(label_ram_stats, "RAM: %u/%u KB", used_h/1024, total_h/1024);
    } else if (current_screen == 3) {
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
  current_screen = scr_id;
  if (scr_id == 0) {
    lv_obj_add_flag(nav_btn_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(nav_btn_right, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load(scr_image);
    
    // Force LVGL to render the full screen before we draw raw TFT items
    // LVGL refresh timer is ~30ms, so we wait slightly longer while processing tasks
    uint32_t t = millis();
    while (millis() - t < 50) {
      lv_timer_handler();
      delay(5);
    }
    
    displayImageOrText();
  } else if (scr_id == 1) {
    lv_obj_add_flag(nav_btn_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(nav_btn_right, LV_OBJ_FLAG_HIDDEN);
    freeImageBuffer(); // Clear current image from RAM when switching to config
    lv_label_set_text(label_config_ip, configTargetIP.c_str());
    lv_scr_load(scr_config);
    fetchAndApplyConfig();
  } else if (scr_id == 2) {
    lv_obj_clear_flag(nav_btn_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(nav_btn_right, LV_OBJ_FLAG_HIDDEN);
    freeImageBuffer();
    lv_scr_load(scr_stats);
  } else if (scr_id == 3) {
    lv_obj_clear_flag(nav_btn_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(nav_btn_right, LV_OBJ_FLAG_HIDDEN);
    freeImageBuffer();
    lv_obj_clean(scr_devices); // Clear old table
    buildDevicesScreen();     // Re-render with new data
    lv_scr_load(scr_devices);
  } else if (scr_id == 4) {
    lv_obj_add_flag(nav_btn_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(nav_btn_right, LV_OBJ_FLAG_HIDDEN);
    freeImageBuffer();
    
    // Populate Dropdown
    lv_dropdown_clear_options(dd_cameras);
    bool has_dev = false;
    for (int i = 0; i < 5; i++) {
      if (devices[i].ip != "") {
        lv_dropdown_add_option(dd_cameras, devices[i].ip.c_str(), LV_DROPDOWN_POS_LAST);
        has_dev = true;
      }
    }
    if (!has_dev) lv_dropdown_add_option(dd_cameras, "No Devices", LV_DROPDOWN_POS_LAST);
    
    lv_scr_load(scr_multi);
    
    uint32_t t = millis();
    while (millis() - t < 50) {
      lv_timer_handler();
      delay(5);
    }
    displayMultiImageOrText();
  }
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
    auto setSld = [&](lv_obj_t* sld, String key) {
      int val = getJsonVal(json, key);
      if (val != -999) {
        lv_slider_set_value(sld, val, LV_ANIM_OFF);
        lv_event_send(sld, LV_EVENT_VALUE_CHANGED, NULL);
      }
    };
    auto setSw = [&](lv_obj_t* sw, String key) {
      int val = getJsonVal(json, key);
      if (val != -999) {
        if (val) lv_obj_add_state(sw, LV_STATE_CHECKED);
        else lv_obj_clear_state(sw, LV_STATE_CHECKED);
      }
    };
    setSld(sld_framesize, "framesize");
    setSld(sld_quality, "quality");
    setSld(sld_brightness, "brightness");
    setSld(sld_contrast, "contrast");
    setSld(sld_saturation, "saturation");
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
  sendVal("quality", lv_slider_get_value(sld_quality));
  sendVal("brightness", lv_slider_get_value(sld_brightness));
  sendVal("contrast", lv_slider_get_value(sld_contrast));
  sendVal("saturation", lv_slider_get_value(sld_saturation));
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

void buildStatsScreen() {
  // Top Section
  lv_obj_t * top_panel_stats = lv_obj_create(scr_stats);
  lv_obj_set_size(top_panel_stats, screenWidth, 30);
  lv_obj_align(top_panel_stats, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_pad_all(top_panel_stats, 0, 0);
  lv_obj_set_style_border_width(top_panel_stats, 0, 0);
  lv_obj_set_style_radius(top_panel_stats, 0, 0);
  lv_obj_set_style_bg_color(top_panel_stats, lv_palette_main(LV_PALETTE_BLUE_GREY), 0);

  lv_obj_t * title = lv_label_create(top_panel_stats);
  lv_label_set_text(title, "Statistics Dashboard");
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_align(title, LV_ALIGN_LEFT_MID, 10, 0);

  label_ram_stats = lv_label_create(top_panel_stats);
  lv_label_set_text(label_ram_stats, "RAM: --");
  lv_obj_set_style_text_color(label_ram_stats, lv_color_white(), 0);
  lv_obj_align(label_ram_stats, LV_ALIGN_RIGHT_MID, -10, 0);

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
  lv_label_set_text(lbl_today, "Camera Triggered Today: 12");
  lv_obj_set_width(lbl_today, 360);
  lv_obj_set_style_text_align(lbl_today, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_bottom(lbl_today, 20, 0);

  lv_obj_t * lbl_chart = lv_label_create(cont);
  lv_label_set_text(lbl_chart, "Camera triggered last 7 days");
  lv_obj_set_width(lbl_chart, 360);
  lv_obj_set_style_text_align(lbl_chart, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_bottom(lbl_chart, 5, 0);

  // Line chart
  lv_obj_t * chart = lv_chart_create(cont);
  lv_obj_set_size(chart, 360, 160);
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(chart, 7);
  lv_obj_set_style_bg_color(chart, lv_color_black(), 0);
  lv_obj_set_style_border_color(chart, lv_color_white(), 0);
  lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);

  lv_chart_series_t * ser = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_set_next_value(chart, ser, 5);
  lv_chart_set_next_value(chart, ser, 12);
  lv_chart_set_next_value(chart, ser, 8);
  lv_chart_set_next_value(chart, ser, 15);
  lv_chart_set_next_value(chart, ser, 4);
  lv_chart_set_next_value(chart, ser, 20);
  lv_chart_set_next_value(chart, ser, 12);
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

  lv_obj_t * title = lv_label_create(top_panel_dev);
  lv_label_set_text(title, "List Devices");
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_align(title, LV_ALIGN_LEFT_MID, 10, 0);

  label_ram_devices = lv_label_create(top_panel_dev);
  lv_label_set_text(label_ram_devices, "RAM: --");
  lv_obj_set_style_text_color(label_ram_devices, lv_color_white(), 0);
  lv_obj_align(label_ram_devices, LV_ALIGN_RIGHT_MID, -10, 0);

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
  add_col(row_hdr, "TEST", 60);

  // Dummy Data Rows
  auto add_row = [&](int no, const char* mac, const char* ip) {
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

    lv_obj_t * btn = lv_btn_create(row);
    lv_obj_set_size(btn, 50, 25);
    lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_t * lbl_btn = lv_label_create(btn);
    lv_label_set_text(lbl_btn, "PING");
    lv_obj_set_style_text_font(lbl_btn, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_btn);
  };

  int row_count = 1;
  for (int i = 0; i < 5; i++) {
    if (devices[i].mac != "" && devices[i].ip != "") {
      add_row(row_count++, devices[i].mac.c_str(), devices[i].ip.c_str());
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

  // CFG Button with Cogwheel
  lv_obj_t * btn_cfg = lv_btn_create(top_panel_multi);
  lv_obj_set_size(btn_cfg, 35, 28);
  lv_obj_align(btn_cfg, LV_ALIGN_LEFT_MID, 5, 0);
  lv_obj_set_style_bg_opa(btn_cfg, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(btn_cfg, 1, 0);
  lv_obj_set_style_border_color(btn_cfg, lv_color_white(), 0);
  lv_obj_set_style_shadow_width(btn_cfg, 0, 0);
  lv_obj_add_event_cb(btn_cfg, [](lv_event_t *e) {
    char ip_buf[32];
    lv_dropdown_get_selected_str(dd_cameras, ip_buf, sizeof(ip_buf));
    configTargetIP = String(ip_buf);
    if (configTargetIP != "No Devices") {
      switchScreen(1);
    }
  }, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t * lbl_cfg = lv_label_create(btn_cfg);
  lv_label_set_text(lbl_cfg, LV_SYMBOL_SETTINGS);
  lv_obj_center(lbl_cfg);

  // Dropdown for IP selection
  dd_cameras = lv_dropdown_create(top_panel_multi);
  lv_obj_set_size(dd_cameras, 150, 28);
  lv_obj_align(dd_cameras, LV_ALIGN_LEFT_MID, 45, 0);
  lv_obj_set_style_text_font(dd_cameras, &lv_font_montserrat_14, 0);
  lv_obj_set_style_pad_all(dd_cameras, 2, 0);

  label_ram_multi = lv_label_create(top_panel_multi);
  lv_label_set_text(label_ram_multi, "RAM: --");
  lv_obj_set_style_text_color(label_ram_multi, lv_color_white(), 0);
  lv_obj_align(label_ram_multi, LV_ALIGN_RIGHT_MID, -10, 0);

  label_notify_multi = lv_label_create(top_panel_multi);
  lv_label_set_text(label_notify_multi, "Tap to capture");
  lv_obj_set_style_text_color(label_notify_multi, lv_color_white(), 0);
  lv_obj_align(label_notify_multi, LV_ALIGN_CENTER, 40, 0); // Shift right to avoid dropdown

  // Body container (Image View)
  lv_obj_t * body_panel_multi = lv_obj_create(scr_multi);
  lv_obj_set_size(body_panel_multi, screenWidth, screenHeight - 30);
  lv_obj_align(body_panel_multi, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_pad_all(body_panel_multi, 0, 0);
  lv_obj_set_style_border_width(body_panel_multi, 0, 0);
  lv_obj_set_style_radius(body_panel_multi, 0, 0);
  lv_obj_set_style_bg_color(body_panel_multi, lv_color_hex(0x202020), 0);
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

  create_slider(cont, "Framesize", 0, 13, &sld_framesize);
  create_slider(cont, "Quality", 0, 63, &sld_quality);
  create_slider(cont, "Brightness", -2, 2, &sld_brightness);
  create_slider(cont, "Contrast", -2, 2, &sld_contrast);
  create_slider(cont, "Saturation", -2, 2, &sld_saturation);
  
  create_switch(cont, "AWB", &sw_awb);
  create_switch(cont, "AEC", &sw_aec);
  create_switch(cont, "AGC", &sw_agc);
  create_switch(cont, "Mirror", &sw_hmirror);
  create_switch(cont, "Flip", &sw_vflip);
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

    // Initialize mDNS
    if (MDNS.begin("gateway")) {
      Serial.println("mDNS responder started: http://gateway.local");
      MDNS.addService("http", "tcp", 80);
    }
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
  
  String clientIP = server.client().remoteIP().toString();
  bool found = false;
  
  for (int i = 0; i < 5; i++) {
    if (devices[i].mac == mac) {
      found = true;
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

void captureImage(String targetIP) {
  if (WiFi.status() != WL_CONNECTED) return;
  freeImageBuffer();
  
  String url = "http://" + targetIP + "/capture";
  http.begin(url);
  http.setTimeout(5000); // 5 second connection timeout
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    int contentLength = http.getSize();
    WiFiClient* stream = http.getStreamPtr();
    
    if (contentLength > 0 && contentLength <= MAX_IMAGE_SIZE) {
      if (psramFound()) {
        imageBuffer = (uint8_t*)heap_caps_malloc(contentLength, MALLOC_CAP_SPIRAM);
      } else {
        imageBuffer = (uint8_t*)malloc(contentLength);
      }
      
      if (imageBuffer) {
        size_t bytesRead = 0;
        unsigned long start = millis();
        // 5 second total download timeout
        while (http.connected() && bytesRead < contentLength && (millis() - start < 5000)) {
          if (stream->available()) {
            int len = stream->readBytes(imageBuffer + bytesRead, stream->available());
            bytesRead += len;
          }
          delay(1);
        }
        
        if (bytesRead == (size_t)contentLength) {
          imageBufferSize = bytesRead;
          uint16_t w=0, h=0;
          if (getJpgSize(imageBuffer, imageBufferSize, &w, &h)) {
            lastImgW = w;
            lastImgH = h;
          }
        } else {
          // Download incomplete or timeout
          freeImageBuffer();
        }
      }
    }
  }
  http.end();
}

void handleCapture() {
  if (!server.hasArg("ip")) {
    server.send(400, "application/json", "{\"error\": \"Missing ip param\"}");
    return;
  }
  
  lastGlobalIP = server.arg("ip");
  switchScreen(0);
  runGlobalCapture();
  
  if (imageBuffer && imageBufferSize > 0) {
    server.send(200, "application/json", "{\"status\": \"ok\", \"size\": " + String(imageBufferSize) + "}");
  } else {
    server.send(500, "application/json", "{\"error\": \"Capture fail\"}");
  }
}

void runGlobalCapture() {
  if (lastGlobalIP == "") {
    lv_label_set_text(label_notify, "No Camera IP");
    return;
  }
  captureImage(lastGlobalIP);
  if (imageBuffer && imageBufferSize > 0) {
    displayImageOrText();
    lv_label_set_text(label_status, lastGlobalIP.c_str());
    lv_label_set_text(label_notify, "Done");
  } else {
    lv_label_set_text(label_notify, "Error");
  }
  notify_done_time = millis();
  if (notify_done_time == 0) notify_done_time = 1;
}

void handleCaptureMulti() {
  captureMultiImage();
  if (imageBuffer && imageBufferSize > 0) {
    displayMultiImageOrText();
    lv_label_set_text(label_notify_multi, "Done");
  } else {
    lv_label_set_text(label_notify_multi, "Error");
  }
  notify_done_time = millis();
  if (notify_done_time == 0) notify_done_time = 1;
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

void displayImageOrText() {
  if (current_screen != 0) return; // Only draw on image screen
  
  // Clear the image area with dark grey before drawing to match LVGL background
  tft.fillRect(0, 30, screenWidth, screenHeight - 30, tft.color565(32, 32, 32));

  if (imageBuffer && imageBufferSize > 0) {
    uint16_t img_w = 0, img_h = 0;
    float scale = 1.0f;
    
    // Parse JPEG header to find width and height
    if (getJpgSize(imageBuffer, imageBufferSize, &img_w, &img_h)) {
      // Calculate uniform scale to fit the screen
      float target_w = screenWidth;
      float target_h = screenHeight - 30; // 30 pixels reserved for top labels
      float ratio_w = target_w / img_w;
      float ratio_h = target_h / img_h;
      scale = (ratio_w < ratio_h) ? ratio_w : ratio_h;
    }

    // Center the image horizontally and vertically
    int32_t x_offset = (screenWidth - (img_w * scale)) / 2;
    int32_t y_offset = 30 + ((screenHeight - 30) - (img_h * scale)) / 2;
    if (x_offset < 0) x_offset = 0;
    if (y_offset < 30) y_offset = 30;

    // Draw scaled JPEG
    tft.drawJpg(imageBuffer, imageBufferSize, x_offset, y_offset, 0, 0, 0, 0, scale, scale);
    
    lv_label_set_text_fmt(label_status, "Cam: %s", lastGlobalIP.c_str());
  } else {
    lv_label_set_text(label_status, "Waiting...");
  }

  // Force LVGL to redraw the top header on top of the image
  lv_obj_invalidate(top_panel); 
  lv_timer_handler();

  // Draw Glass-like buttons (outline) - ALWAYS on screen 0
  tft.drawRoundRect(5, 110, 30, 100, 5, TFT_WHITE);
  tft.drawRoundRect(screenWidth - 35, 110, 30, 100, 5, TFT_WHITE);
  
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE);
  
  // Navigation Arrows
  tft.setCursor(12, 150);
  tft.print("<");
  tft.setCursor(screenWidth - 25, 150);
  tft.print(">");
}

void captureMultiImage() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  char ip_buf[32];
  lv_dropdown_get_selected_str(dd_cameras, ip_buf, sizeof(ip_buf));
  if (String(ip_buf) == "No Devices") return;

  freeImageBuffer();
  String url = "http://" + String(ip_buf) + "/capture";
  
  http.begin(url);
  http.setTimeout(5000);
  int httpCode = http.GET();
  if (httpCode == 200) {
    int contentLength = http.getSize();
    WiFiClient* stream = http.getStreamPtr();
    if (contentLength > 0 && contentLength <= MAX_IMAGE_SIZE) {
      if (psramFound()) imageBuffer = (uint8_t*)heap_caps_malloc(contentLength, MALLOC_CAP_SPIRAM);
      else imageBuffer = (uint8_t*)malloc(contentLength);
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
        if (bytesRead == (size_t)contentLength) imageBufferSize = bytesRead;
        else freeImageBuffer();
      }
    }
  }
  http.end();
}

void displayMultiImageOrText() {
  if (current_screen != 4) return;
  tft.fillRect(0, 30, screenWidth, screenHeight - 30, tft.color565(32, 32, 32));
  if (imageBuffer && imageBufferSize > 0) {
    uint16_t img_w = 0, img_h = 0;
    float scale = 1.0f;
    if (getJpgSize(imageBuffer, imageBufferSize, &img_w, &img_h)) {
      float ratio_w = (float)screenWidth / img_w;
      float ratio_h = (float)(screenHeight - 30) / img_h;
      scale = (ratio_w < ratio_h) ? ratio_w : ratio_h;
    }
    int32_t x_off = (screenWidth - (img_w * scale)) / 2;
    int32_t y_off = 30 + ((screenHeight - 30) - (img_h * scale)) / 2;
    tft.drawJpg(imageBuffer, imageBufferSize, x_off, y_off, 0, 0, 0, 0, scale, scale);
  }
  lv_obj_invalidate(top_panel_multi);
  lv_timer_handler();

  tft.drawRoundRect(5, 110, 30, 100, 5, TFT_WHITE);
  tft.drawRoundRect(screenWidth - 35, 110, 30, 100, 5, TFT_WHITE);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(12, 150); tft.print("<");
  tft.setCursor(screenWidth - 25, 150); tft.print(">");
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
        body { font-family: sans-serif; background: #202020; padding: 15px; color: #eee; }
        .card { background: #2c2c2c; border-radius: 12px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); max-width: 600px; margin: auto; padding: 20px; border: 1px solid #444; }
        h1 { text-align: center; color: #1a73e8; margin-bottom: 20px; font-size: 24px; }
        .btn { display: block; width: 100%; background: #1a73e8; color: white; border: none; padding: 12px; border-radius: 8px; font-weight: bold; cursor: pointer; margin-bottom: 20px; }
        .img-box { background: #111; border-radius: 8px; min-height: 200px; display: flex; align-items: center; justify-content: center; margin-bottom: 20px; overflow: hidden; border: 1px solid #444; }
        img { max-width: 100%; height: auto; display: block; }
        .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
        .item { background: #333; padding: 10px; border-radius: 8px; border-left: 3px solid #1a73e8; }
        label { display: block; font-size: 12px; color: #aaa; margin-bottom: 4px; }
        input { width: 100%; padding: 6px; border: 1px solid #444; border-radius: 4px; background: #222; color: #fff; }
        .toggle { display: flex; gap: 4px; }
        .t-btn { flex: 1; font-size: 11px; padding: 6px; border: 1px solid #444; border-radius: 4px; background: #222; color: #eee; cursor: pointer; }
        .t-btn.active { background: #34a853; color: white; border-color: #34a853; }
        table { width: 100%; border-collapse: collapse; margin-top: 10px; font-size: 14px; color: #eee; }
        th { background: #1a73e8; color: white; padding: 10px; text-align: left; }
        td { padding: 10px; border-bottom: 1px solid #444; }
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

    <div class="card" style="margin-top: 20px;">
        <h1>📱 Devices</h1>
        <table>
            <thead>
                <tr>
                    <th>No.</th>
                    <th>MAC</th>
                    <th>IP</th>
                    <th>TEST</th>
                </tr>
            </thead>
            <tbody id="dev-list"></tbody>
        </table>
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
            loadDevices();
        }
        function loadDevices() {
            fetch('/devices').then(r => r.json()).then(d => {
                const list = document.getElementById('dev-list');
                list.innerHTML = '';
                d.forEach((dev, idx) => {
                    const tr = document.createElement('tr');
                    tr.innerHTML = `
                        <td>${idx + 1}</td>
                        <td>${dev.mac}</td>
                        <td>${dev.ip}</td>
                        <td><button class="t-btn active" style="padding:4px 8px;" onclick="ping('${dev.ip}')">PING</button></td>
                    `;
                    list.appendChild(tr);
                });
            });
        }
        function ping(ip) {
            alert('Pinging ' + ip + '...');
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
