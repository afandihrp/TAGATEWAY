# ESP32-S3 Camera Gateway Controller

This project acts as a central gateway and user interface for managing multiple remote ESP32 camera modules. Built for the ESP32-S3, it leverages a touchscreen display, PSRAM, SD card storage, and Telegram bot integration to provide a complete control and monitoring solution.

## 🚀 Key Features

### 1. Hardware & Core System
* **ESP32-S3 Optimized:** Fully utilizes the ESP32-S3's capabilities, specifically routing memory-intensive tasks (like LVGL framebuffers and mbedtls SSL contexts) to **PSRAM** for stability.
* **Storage & Configuration:** Uses an SD card (over SPI3) to store historical data, archived images, web assets (`index.html`), and load sensitive credentials via a `config.json` file.
* **Network & Time:** Connects to WiFi, registers an mDNS responder (`gateway.local`), and synchronizes time via NTP for accurate image archiving and statistics.

### 2. Rich Touch User Interface (LVGL + LovyanGFX)
Drives an ILI9488 display (480x320) with XPT2046 touch using LovyanGFX and LVGL. The UI is split into several interactive screens:
* **Image Viewer:** Displays the most recently captured image from a targeted camera.
* **Configuration:** Provides sliders and switches to remotely adjust camera settings (Framesize, Quality, Brightness, Contrast, Saturation, LED Flash, AWB, AEC, AGC, Mirror, Flip).
* **Multi-Camera View:** Interfaces for selecting different camera IPs, capturing images, starting MJPEG streams, and viewing multi-camera data.
* **Statistics Dashboard:** Features an LVGL line chart displaying the number of camera triggers over the last 7 days and an SD card storage progress bar.
* **Device Registry:** Tabular view of all registered remote cameras showing MAC, IP, and Wi-Fi signal strength (RSSI).

### 3. Remote Camera Management
* **Auto-Registration:** Hosts an HTTP POST endpoint (`/register`) allowing up to 5 remote cameras to self-register their MAC, IP, and RSSI.
* **Image Capture:** Fetches single frames (`http://IP/capture`) directly into a 1MB PSRAM buffer, renders them on the screen, and saves them to the SD card.
* **Video Streaming:** Connects to the remote camera's stream port (Port 81), parsing and rendering the MJPEG stream continuously on the UI.
* **HTTP Control API:** Relays configuration adjustments to the target camera via HTTP GET requests (`/control?var=...`).

### 4. Telegram Bot Integration
Features a fully autonomous, background-polled Telegram bot (running on Core 0) capable of two-way communication:
* **Event Notifications:** Automatically uploads images triggered by the system to a configured Telegram chat.
* **Remote Commands:** Users can message the bot to control the system remotely:
  * `/devices` - List all registered cameras.
  * `/getstat` - Check today's capture statistics.
  * `/capture {id}` - Trigger a specific camera and receive the photo.
  * `/getimage {DD MM YY}` - Navigate and retrieve archived images saved on the SD card for a specific date.

### 5. Web Server & API
Runs a lightweight internal web server (Port 80):
* **Web UI:** Serves an `index.html` file from the SD card if available.
* **API Endpoints:** Includes `/control`, `/xclk`, `/capture`, `/image`, `/status`, and `/devices` for handling requests over the local network.

### 6. Servo Control
* Includes a UI slider that broadcasts UDP packets (Port 8888) to the active camera IP, allowing real-time angle adjustments (0°-180°) for a pan/tilt servo mechanism.
