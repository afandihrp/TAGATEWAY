#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>

// ===========================
// Select camera model in board_config.h
// ===========================
#include "board_config.h"

// ===========================
// Enter your WiFi credentials
// ===========================
const char *ssid = "BatuKhan";
const char *password = "momoygemoy";

String macAddress;
WiFiUDP udp;
const int udpPort = 8888;

// PIR Sensor Configuration
const int pirLeft = 13;
const int pirMiddle = 15;
const int pirRight = 14;

bool lastLeft = LOW;
bool lastMiddle = LOW;
bool lastRight = LOW;

// Servo Configuration (Pin 14)
const int servoPin = 12;
const int servoFreq = 50;       // 50Hz for standard servos
const int servoResolution = 14; // 14-bit resolution (0-16383)

void startCameraServer();
void setupLedFlash();
void registerCamera();
void handlePIR();
void triggerRemoteCapture(String zone);

void setupServo() {
  pinMode(servoPin, OUTPUT);
  digitalWrite(servoPin, LOW);
  
  if (ledcAttach(servoPin, servoFreq, servoResolution) == 0) {
    Serial.println("[ERROR] Servo PWM initialization failed!");
  } else {
    Serial.println("[OK] Servo initialized on Pin 14");
    // Boot-up wiggle test: 0 -> 90 -> 0
    moveServo(0);
    delay(300);
    moveServo(90);
    delay(300);
    moveServo(0);
  }
}

void moveServo(int degree) {
  // Constrain degree to 0-180 just in case
  degree = constrain(degree, 0, 180);
  
  // Standard servos: 500us to 2400us pulses
  // 500us  / 20000us * 16384 = 410
  // 2400us / 20000us * 16384 = 1966
  int duty = map(degree, 0, 180, 410, 1966);
  ledcWrite(servoPin, duty);
}

void handleUDP() {
  int packetSize = udp.parsePacket();
  if (packetSize) {
    uint8_t degree = udp.read();
    Serial.printf("[UDP] Servo Degree: %d\n", degree);
    moveServo(degree);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 21000000;
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG;  // for streaming
  //config.pixel_format = PIXFORMAT_RGB565; // for face detection/recognition
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  //                      for larger pre-allocated frame buffer.
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      // Limit the frame size when PSRAM is not available
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    // Best option for face detection/recognition
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);        // flip it back
    s->set_brightness(s, 1);   // up the brightness just a bit
    s->set_saturation(s, -2);  // lower the saturation
  }
  // drop down frame size for higher initial frame rate
  if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, FRAMESIZE_HD);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

// Setup LED FLash if LED pin is defined in camera_pins.h
#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  setupServo();

  // Initialize PIR pins
  pinMode(pirLeft, INPUT_PULLDOWN);
  pinMode(pirMiddle, INPUT_PULLDOWN);
  pinMode(pirRight, INPUT_PULLDOWN);

  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

  startCameraServer();
  udp.begin(udpPort);

  macAddress = WiFi.macAddress();
  registerCamera();

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");
}

void loop() {
  handleUDP();
  handlePIR();
  static unsigned long lastRegister = 0;
  if (millis() - lastRegister >= 15000) {
    lastRegister = millis();
    registerCamera();
  }
}

void registerCamera() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin("http://gateway.local/register");
    http.addHeader("Content-Type", "application/json");
    String payload = "{\"mac\":\"" + macAddress + "\", \"rssi\":" + String(WiFi.RSSI()) + "}";
    int httpResponseCode = http.POST(payload);
    if (httpResponseCode > 0) {
      Serial.printf("[AUTO-REG] Code: %d\n", httpResponseCode);
    } else {
      Serial.printf("[AUTO-REG] Error: %s\n", http.errorToString(httpResponseCode).c_str());
    }
    http.end();
  }
}

void handlePIR() {
  bool curLeft = digitalRead(pirLeft);
  bool curMiddle = digitalRead(pirMiddle);
  bool curRight = digitalRead(pirRight);

  if (curLeft == HIGH && lastLeft == LOW) {
    moveServo(180);
    triggerRemoteCapture("Left");
  }
  if (curMiddle == HIGH && lastMiddle == LOW) {
    moveServo(90);
    triggerRemoteCapture("Middle");
  }
  if (curRight == HIGH && lastRight == LOW) {
    moveServo(0);
    triggerRemoteCapture("Right");
  }

  lastLeft = curLeft;
  lastMiddle = curMiddle;
  lastRight = curRight;
}

void triggerRemoteCapture(String zone) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    // gateway.local is the Controller/Gateway address
    String url = "http://gateway.local/capture?ip=" + WiFi.localIP().toString();
    
    Serial.printf("[PIR] Motion detected in %s zone! Requesting capture: %s\n", zone.c_str(), url.c_str());
    
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode > 0) {
      Serial.printf("[PIR] Gateway response: %d\n", httpCode);
    } else {
      Serial.printf("[PIR] Gateway request failed: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
  }
}
