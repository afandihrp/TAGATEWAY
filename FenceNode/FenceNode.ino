#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>

#define ADC_PIN 2

// ================= WIFI =================
const char* ssid = "BatuKhan";
const char* password = "momoygemoy";

// ================= GATEWAY =================
String gatewayIP = ""; // Will be discovered via mDNS

// ================= STATIC IP =================
IPAddress local_IP(192, 168, 11, 60);
IPAddress gateway(192, 168, 11, 22);
IPAddress subnet(255, 255, 255, 0);

// ================= SETTINGS =================
const float VREF = 3.3;
const int ADC_MAX = 4095;
unsigned long lastSendTime = 0;
const unsigned long sendInterval = 1000; // Send data every 1 second

// ================= DISCOVERY =================
bool discoverGateway() {
  Serial.println("[mDNS] Searching for gateway.local...");
  IPAddress serverIP = MDNS.queryHost("gateway");
  if (serverIP != IPAddress(0,0,0,0)) {
    gatewayIP = serverIP.toString();
    Serial.printf("[mDNS] Gateway found: %s\n", gatewayIP.c_str());
    return true;
  }
  Serial.println("[mDNS] Gateway not found. Retrying...");
  return false;
}

// ================= WIFI CONNECT =================
void connectWiFi() {
  Serial.printf("\nConnecting to %s...", ssid);
  
  WiFi.mode(WIFI_STA);
  WiFi.config(local_IP, gateway, subnet);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    if (!MDNS.begin("fencenode")) {
      Serial.println("Error setting up MDNS responder!");
    } else {
      Serial.println("mDNS responder started: fencenode.local");
    }
  } else {
    Serial.println("\nWiFi Connection Failed.");
  }
}

// ================= SEND DATA =================
void sendVoltage(float voltage) {
  if (WiFi.status() != WL_CONNECTED) return;

  // Try to discover gateway if we don't have its IP yet
  if (gatewayIP == "") {
    if (!discoverGateway()) return;
  }

  HTTPClient http;
  String url = "http://" + gatewayIP + "/data?value=" + String(voltage, 2);

  Serial.printf("[SEND] To %s: %.2fV\n", gatewayIP.c_str(), voltage);

  http.begin(url);
  http.setTimeout(2000);
  int httpCode = http.GET();

  if (httpCode > 0) {
    Serial.printf("[HTTP] Response: %d\n", httpCode);
  } else {
    Serial.printf("[HTTP] Error: %s\n", http.errorToString(httpCode).c_str());
    if (httpCode == -1) gatewayIP = ""; // Reset IP to rediscover on next attempt
  }
  
  http.end();
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\nFence Node (mDNS Version) Starting...");
  
  analogReadResolution(12);
  analogSetPinAttenuation(ADC_PIN, ADC_11db);

  connectWiFi();
}

// ================= LOOP =================
void loop() {
  if (millis() - lastSendTime >= sendInterval) {
    lastSendTime = millis();

    if (WiFi.status() != WL_CONNECTED) {
      WiFi.begin(ssid, password);
      return;
    }

    // Averaging ADC
    long sum = 0;
    for (int i = 0; i < 10; i++) {
      sum += analogRead(ADC_PIN);
      delay(5);
    }
    
    int rawADC = sum / 10;
    float voltage = ((float)rawADC / ADC_MAX) * VREF;

    sendVoltage(voltage);
  }
}


