/**
 * ============================================================
 *  SD Card Test untuk ESP32 (38-pin DevKitC)
 * ============================================================
 *
 * Wiring SD Card ke ESP32 DevKitC (Standard VSPI):
 * --------------------------------------------------
 *    SD_CS   → GPIO 5
 *    MOSI    → GPIO 23
 *    MISO    → GPIO 19
 *    SCK     → GPIO 18
 *    VCC     → 3.3V
 *    GND     → GND
 *
 * Library yang dibutuhkan:
 *  - Arduino SD library (built-in)
 *  - SPI library (built-in)
 * ============================================================
 */

#include <SPI.h>
#include <SD.h>

// ============================================================
//  Konfigurasi Pin — Standard ESP32 VSPI
// ============================================================
#define SD_CS_PIN   5
#define SPI_MOSI    23
#define SPI_MISO    19
#define SPI_SCK     18

// Nama file test yang akan dibuat dan dibaca
#define TEST_FILENAME   "/sdtest.txt"
#define TEST_CONTENT    "ESP32 DevKitC SD Card Test OK"

// ============================================================
//  Variabel global
// ============================================================
bool sdOK = false;

// ============================================================
//  Fungsi Deklarasi
// ============================================================
void printCardInfo();
void testWriteFile();
void testReadFile();
void testListDir(const char* dirname);
void testBinaryFile();
void testDeleteFile();
void testBenchmark();
void printTroubleshooting();

// ============================================================
//  Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println("========================================");
  Serial.println("  SD Card Test - ESP32 DevKitC (38-pin)");
  Serial.println("========================================");

  // Inisialisasi SPI dengan pin standard ESP32
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS_PIN);

  Serial.println("\n[1] Menginisialisasi SD card...");
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("  ❌ GAGAL: SD card tidak terdeteksi!");
    Serial.println("     Cek koneksi kabel, pin CS, atau kartu SD.");
    printTroubleshooting();
    return;
  }

  Serial.println("  ✅ SD card berhasil diinisialisasi.");
  sdOK = true;

  // Informasi SD card
  printCardInfo();

  // Tes 1: Tulis file
  testWriteFile();

  // Tes 2: Baca file
  testReadFile();

  // Tes 3: List direktori root
  testListDir("/");

  // Tes 4: Tulis & baca file biner (opsional)
  testBinaryFile();

  // Tes 5: Hapus file test
  testDeleteFile();

  // Tes 6: Benchmark kecepatan
  testBenchmark();

  Serial.println("\n========================================");
  Serial.println("  Semua tes selesai!");
  Serial.println("========================================");
}

// ============================================================
//  Loop
// ============================================================
void loop() {
  // Kosong — semua tes dilakukan di setup()
  delay(5000);
}

// ============================================================
//  Fungsi: Informasi Kartu SD
// ============================================================
void printCardInfo() {
  Serial.println("\n[INFO] Informasi SD Card:");

  uint8_t cardType = SD.cardType();
  Serial.print("  Tipe Kartu : ");
  switch (cardType) {
    case CARD_MMC:  Serial.println("MMC");  break;
    case CARD_SD:   Serial.println("SDSC"); break;
    case CARD_SDHC: Serial.println("SDHC/SDXC"); break;
    default:        Serial.println("UNKNOWN"); break;
  }

  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("  Ukuran     : %llu MB\n", cardSize);
  Serial.printf("  Total      : %llu MB\n", SD.totalBytes() / (1024 * 1024));
  Serial.printf("  Terpakai   : %llu MB\n", SD.usedBytes() / (1024 * 1024));
}

// ============================================================
//  Tes 1: Tulis File
// ============================================================
void testWriteFile() {
  Serial.println("\n[2] Tes TULIS file...");

  File file = SD.open(TEST_FILENAME, FILE_WRITE);
  if (!file) {
    Serial.println("  ❌ GAGAL membuka file untuk ditulis!");
    return;
  }

  size_t written = file.println(TEST_CONTENT);
  file.printf("Timestamp: %lu ms\n", millis());
  file.close();

  if (written > 0) {
    Serial.printf("  ✅ File '%s' berhasil ditulis (%d bytes).\n", TEST_FILENAME, (int)written);
  } else {
    Serial.println("  ❌ GAGAL menulis ke file!");
  }
}

// ============================================================
//  Tes 2: Baca File
// ============================================================
void testReadFile() {
  Serial.println("\n[3] Tes BACA file...");

  File file = SD.open(TEST_FILENAME, FILE_READ);
  if (!file) {
    Serial.println("  ❌ GAGAL membuka file untuk dibaca!");
    return;
  }

  Serial.println("  Isi file:");
  while (file.available()) {
    String line = file.readStringUntil('\n');
    Serial.printf("    > %s\n", line.c_str());
  }
  file.close();

  // Verifikasi konten
  file = SD.open(TEST_FILENAME, FILE_READ);
  String firstLine = file.readStringUntil('\n');
  file.close();

  firstLine.trim();
  if (firstLine == TEST_CONTENT) {
    Serial.println("  ✅ Verifikasi konten: COCOK!");
  } else {
    Serial.println("  ⚠️  Verifikasi konten: TIDAK COCOK (cek encoding).");
    Serial.printf("    Diharapkan : '%s'\n", TEST_CONTENT);
    Serial.printf("    Terbaca    : '%s'\n", firstLine.c_str());
  }
}

// ============================================================
//  Tes 3: List Direktori
// ============================================================
void testListDir(const char* dirname) {
  Serial.printf("\n[4] Tes LIST direktori '%s'...\n", dirname);

  File root = SD.open(dirname);
  if (!root) {
    Serial.println("  ❌ GAGAL membuka direktori!");
    return;
  }
  if (!root.isDirectory()) {
    Serial.println("  ❌ Bukan direktori!");
    return;
  }

  File entry = root.openNextFile();
  int fileCount = 0;
  while (entry) {
    if (entry.isDirectory()) {
      Serial.printf("  📁 DIR  : %s\n", entry.name());
    } else {
      Serial.printf("  📄 FILE : %-30s  %6d bytes\n", entry.name(), (int)entry.size());
      fileCount++;
    }
    entry = root.openNextFile();
  }
  root.close();

  Serial.printf("  ✅ Total file ditemukan: %d\n", fileCount);
}

// ============================================================
//  Tes 4: Tulis & Baca File Biner
// ============================================================
void testBinaryFile() {
  Serial.println("\n[5] Tes file BINER...");

  const char* binFile = "/bintest.bin";
  uint8_t writeData[16];
  for (int i = 0; i < 16; i++) writeData[i] = i * 10;

  // Tulis
  File f = SD.open(binFile, FILE_WRITE);
  if (!f) {
    Serial.println("  ❌ GAGAL membuat file biner!");
    return;
  }
  f.write(writeData, 16);
  f.close();

  // Baca dan verifikasi
  f = SD.open(binFile, FILE_READ);
  if (!f) {
    Serial.println("  ❌ GAGAL membaca file biner!");
    return;
  }
  uint8_t readData[16] = {0};
  f.read(readData, 16);
  f.close();

  bool match = true;
  for (int i = 0; i < 16; i++) {
    if (readData[i] != writeData[i]) { match = false; break; }
  }

  if (match) {
    Serial.println("  ✅ File biner: tulis & baca COCOK!");
  } else {
    Serial.println("  ❌ File biner: data TIDAK COCOK!");
  }

  SD.remove(binFile);
}

// ============================================================
//  Tes 5: Hapus File
// ============================================================
void testDeleteFile() {
  Serial.println("\n[6] Tes HAPUS file...");

  if (SD.exists(TEST_FILENAME)) {
    if (SD.remove(TEST_FILENAME)) {
      Serial.printf("  ✅ File '%s' berhasil dihapus.\n", TEST_FILENAME);
    } else {
      Serial.printf("  ❌ GAGAL menghapus file '%s'.\n", TEST_FILENAME);
    }
  } else {
    Serial.println("  ⚠️  File tidak ditemukan (mungkin sudah terhapus).");
  }
}

// ============================================================
//  Tes 6: Benchmark Kecepatan
// ============================================================
void testBenchmark() {
  Serial.println("\n[7] Tes BENCHMARK kecepatan...");

  const char* benchFile = "/bench.bin";
  const size_t BUF_SIZE  = 512;
  const int    ITERATION = 20;

  uint8_t buf[BUF_SIZE];
  for (int i = 0; i < BUF_SIZE; i++) buf[i] = (uint8_t)(i & 0xFF);

  // --- Tulis ---
  File f = SD.open(benchFile, FILE_WRITE);
  if (!f) {
    Serial.println("  ❌ GAGAL membuat file benchmark!");
    return;
  }
  unsigned long tStart = millis();
  for (int i = 0; i < ITERATION; i++) f.write(buf, BUF_SIZE);
  f.close();
  unsigned long tWrite = millis() - tStart;

  float writeKBps = (float)(BUF_SIZE * ITERATION) / tWrite;  // KB/s
  Serial.printf("  ✍️  Tulis : %lu ms → %.1f KB/s\n", tWrite, writeKBps);

  // --- Baca ---
  f = SD.open(benchFile, FILE_READ);
  if (!f) {
    Serial.println("  ❌ GAGAL membuka file benchmark untuk dibaca!");
    return;
  }
  tStart = millis();
  while (f.available()) f.read(buf, BUF_SIZE);
  f.close();
  unsigned long tRead = millis() - tStart;

  float readKBps = (float)(BUF_SIZE * ITERATION) / max(tRead, 1UL);
  Serial.printf("  📖 Baca  : %lu ms → %.1f KB/s\n", tRead, readKBps);

  SD.remove(benchFile);
  Serial.println("  ✅ Benchmark selesai.");
}

// ============================================================
//  Troubleshooting Tips
// ============================================================
void printTroubleshooting() {
  Serial.println("\n  Tips Troubleshooting:");
  Serial.println("  ─────────────────────────────────────────");
  Serial.println("  1. Pastikan pin CS (SD_CS_PIN) sudah benar (GPIO 5)");
  Serial.println("  2. Cek apakah kartu SD diformat FAT32");
  Serial.println("  3. Pastikan wiring: SCK=18, MISO=19, MOSI=23, CS=5");
  Serial.println("  4. Pastikan VCC SD = 3.3V");
  Serial.println("  ─────────────────────────────────────────");
}
