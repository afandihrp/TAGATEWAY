#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// Replace with your network credentials
const char* ssid = "BatuKhan";
const char* password = "momoygemoy";

// Replace with your Telegram bot token
const String botToken = "7910361449:AAFMjzZxkDQAg1y6oeIJ0gVapBXbd2e11DU";

// The IP and endpoint of the ESP32-CAM (CameraWebServer)
const char* cameraUrl = "http://192.168.11.249/capture";

// Polling interval for new messages (in ms)
const int botRequestDelay = 3000;
unsigned long lastTimeBotRan;
int lastUpdateId = 0;

// Function declarations
void handleTelegramUpdates();
void sendTelegramMessage(String chatId, String text);
void fetchAndSendPhoto(String chatId);
void sendPhotoToTelegram(String chatId, uint8_t* imageBuffer, int imageSize);

void setup() {
  Serial.begin(115200);

  // Initialize PSRAM for the ESP32-S3 (N16R8 has 8MB PSRAM)
  if (psramInit()) {
    Serial.println("PSRAM is correctly initialized.");
  } else {
    Serial.println("PSRAM initialization failed! Falling back to Internal RAM.");
    Serial.printf("Free Internal RAM: %d bytes\n", ESP.getFreeHeap());
  }

  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected.");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  lastTimeBotRan = millis();
}

void loop() {
  // Poll for new Telegram messages every few seconds
  if (millis() - lastTimeBotRan > botRequestDelay) {
    handleTelegramUpdates();
    lastTimeBotRan = millis();
  }
}

// Function to fetch updates from Telegram API
void handleTelegramUpdates() {
  String commandToRun = "";
  String commandChatId = "";
  String fromName = "";

  // Use a local scope to ensure the WiFiClientSecure is destroyed/closed
  // BEFORE we try to capture the photo or send other messages.
  // This prevents multiple SSL connections from exhausting the heap.
  {
    WiFiClientSecure client;
    client.setInsecure(); // Disable certificate verification
    HTTPClient https;

    String url = "https://api.telegram.org/bot" + botToken + "/getUpdates?offset=" + String(lastUpdateId + 1) + "&timeout=5";

    if (https.begin(client, url)) {
      int httpCode = https.GET();
      if (httpCode == HTTP_CODE_OK) {
        String payload = https.getString();
        
        // Parse JSON using ArduinoJson
        DynamicJsonDocument doc(4096);
        DeserializationError error = deserializeJson(doc, payload);

        if (!error && doc["ok"]) {
          JsonArray result = doc["result"];
          for (JsonObject update : result) {
            lastUpdateId = update["update_id"];
            if (update["message"]["text"]) {
              commandToRun = update["message"]["text"].as<String>();
              commandChatId = update["message"]["chat"]["id"].as<String>();
              fromName = update["message"]["from"]["first_name"].as<String>();
            }
          }
        }
      } else {
        Serial.printf("Error getting updates. HTTP code: %d\n", httpCode);
      }
      https.end();
      client.stop();
    } else {
      Serial.println("Unable to connect to Telegram server.");
    }
  } // Scope ends, SSL connection is completely freed

  // Process the command if we received one
  if (commandToRun != "") {
    Serial.println("Received message: " + commandToRun + " from " + fromName);

    if (commandToRun == "/photo") {
      sendTelegramMessage(commandChatId, "Fetching photo from ESP32-CAM...");
      fetchAndSendPhoto(commandChatId);
    } else if (commandToRun == "/start") {
      String welcome = "Welcome " + fromName + ".\nUse /photo to capture an image.";
      sendTelegramMessage(commandChatId, welcome);
    }
  }
}

// Function to send a simple text message to a specific chat ID
void sendTelegramMessage(String chatId, String text) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String url = "https://api.telegram.org/bot" + botToken + "/sendMessage";

  if (https.begin(client, url)) {
    https.addHeader("Content-Type", "application/json");
    
    DynamicJsonDocument doc(1024);
    doc["chat_id"] = chatId;
    doc["text"] = text;
    
    String payload;
    serializeJson(doc, payload);

    int httpCode = https.POST(payload);
    if (httpCode != HTTP_CODE_OK) {
      Serial.printf("Failed to send message. HTTP code: %d\n", httpCode);
    }
    https.end();
    client.stop();
  }
}

// Function to request image from camera and allocate space in PSRAM
void fetchAndSendPhoto(String chatId) {
  HTTPClient http;
  
  Serial.print("Connecting to camera: ");
  Serial.println(cameraUrl);
  
  http.begin(cameraUrl);
  
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    sendTelegramMessage(chatId, "Failed to fetch image from camera. HTTP Code: " + String(httpCode));
    http.end();
    return;
  }
  
  int len = http.getSize();
  if (len <= 0) {
    sendTelegramMessage(chatId, "Received empty image from camera.");
    http.end();
    return;
  }

  Serial.printf("Image size: %d bytes\n", len);

  // Allocate memory in RAM buffer (PSRAM preferred)
  uint8_t* imageBuffer = nullptr;
  if (psramFound()) {
    imageBuffer = (uint8_t*)ps_malloc(len);
    if (imageBuffer) Serial.println("PSRAM memory allocated. Downloading image...");
  } else {
    imageBuffer = (uint8_t*)malloc(len);
    if (imageBuffer) Serial.println("Internal RAM memory allocated. Downloading image...");
  }

  if (imageBuffer == nullptr) {
    sendTelegramMessage(chatId, "Failed to allocate memory for image.");
    http.end();
    return;
  }
  
  // Download the image directly into RAM buffer
  WiFiClient* stream = http.getStreamPtr();
  int bytesRead = 0;
  unsigned long timeout = millis();

  while (http.connected() && (len > 0)) {
    size_t size = stream->available();
    if (size) {
      // Prevent buffer overflow by reading only what is left
      int bytesToRead = min(size, (size_t)len);
      int c = stream->read(imageBuffer + bytesRead, bytesToRead);
      if (c > 0) {
        bytesRead += c;
        len -= c;
        timeout = millis();
      }
    } else {
      delay(1);
    }
    // 10 seconds timeout
    if (millis() - timeout > 10000) {
      Serial.println("Download timeout!");
      break;
    }
  }
  
  // Explicitly close the camera HTTP connection before sending to Telegram
  // to save RAM and avoid network stack overload.
  http.end();
  
  if (bytesRead > 0) {
    Serial.printf("Downloaded %d bytes. Sending to Telegram...\n", bytesRead);
    sendPhotoToTelegram(chatId, imageBuffer, bytesRead);
  } else {
    sendTelegramMessage(chatId, "Failed to read image data.");
  }
  
  // Free the PSRAM memory when done
  free(imageBuffer);
  Serial.println("PSRAM memory freed.");
}

// Function to upload a photo to Telegram using multipart/form-data POST
void sendPhotoToTelegram(String chatId, uint8_t* imageBuffer, int imageSize) {
  WiFiClientSecure client;
  client.setInsecure(); // Disable certificate verification
  
  const char* host = "api.telegram.org";
  const int port = 443;
  
  if (!client.connect(host, port)) {
    Serial.println("Connection to Telegram failed for sending photo");
    sendTelegramMessage(chatId, "Failed to connect to Telegram API for photo upload.");
    return;
  }

  String boundary = "----ESP32Boundary" + String(millis());
  
  // Create the header part of the multipart form data
  String head = "--" + boundary + "\r\n"
              + "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
              + chatId + "\r\n"
              + "--" + boundary + "\r\n"
              + "Content-Disposition: form-data; name=\"photo\"; filename=\"image.jpg\"\r\n"
              + "Content-Type: image/jpeg\r\n\r\n";
              
  // Create the tail part
  String tail = "\r\n--" + boundary + "--\r\n";
  
  // Calculate total payload length
  uint32_t contentLength = head.length() + imageSize + tail.length();
  
  // Send HTTP headers
  client.println("POST /bot" + botToken + "/sendPhoto HTTP/1.1");
  client.println("Host: " + String(host));
  client.println("Content-Length: " + String(contentLength));
  client.println("Content-Type: multipart/form-data; boundary=" + boundary);
  client.println();
  
  // Send the multipart payload head
  client.print(head);
  
  // Send image in chunks. We allocate a small chunk buffer in INTERNAL RAM.
  // Some ESP32 mbedtls / DMA routines crash when reading directly from PSRAM
  // during SSL write operations. Copying to internal RAM avoids `StoreProhibited` panics.
  int chunkSize = 2048; // Send 2KB at a time
  uint8_t* chunkBuffer = (uint8_t*)malloc(chunkSize);
  
  if (chunkBuffer != nullptr) {
    for (int i = 0; i < imageSize; i += chunkSize) {
      int currentChunkSize = min(chunkSize, imageSize - i);
      // Copy from PSRAM to internal RAM
      memcpy(chunkBuffer, imageBuffer + i, currentChunkSize);
      // Write from internal RAM
      client.write(chunkBuffer, currentChunkSize);
    }
    free(chunkBuffer);
  } else {
    Serial.println("Failed to allocate chunk buffer in internal RAM! Sending from PSRAM as fallback.");
    // Fallback (might crash depending on ESP-IDF version, but better than doing nothing)
    for (int i = 0; i < imageSize; i += chunkSize) {
      int currentChunkSize = min(chunkSize, imageSize - i);
      client.write(imageBuffer + i, currentChunkSize);
    }
  }
  
  // Send the multipart payload tail
  client.print(tail);
  
  // Wait for the response
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") {
      break;
    }
  }
  
  // Read and print response payload
  String response = client.readString();
  Serial.println("Telegram Response: " + response);
  
  client.stop();
  
  if (response.indexOf("\"ok\":true") > 0) {
    Serial.println("Photo sent successfully!");
  } else {
    Serial.println("Error sending photo to Telegram.");
  }
}