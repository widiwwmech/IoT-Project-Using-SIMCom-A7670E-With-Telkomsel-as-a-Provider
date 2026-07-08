/**
 * ================================================================
 * AUDIRA IoT Firmware v2.0 - STABLE HTTPS EDITION
 * ESP32 DevKit V4 + MFRC522 RFID + SIMCOM A7670C (4G LTE)
 * ================================================================
 */

#include <ArduinoJson.h>
#include <MFRC522.h>
#include <SPI.h>

// ============================================================
//  KONFIGURASI — SESUAIKAN DENGAN KEBUTUHAN
// ============================================================
String APN          = "internet";         // Telkomsel / By.U
String SERVER_HOST  = "audira.id";        // Domain server
String DEVICE_TOKEN = "AUDIRA-HW-001";    // Identitas unik alat

// ============================================================
//  PIN DEFINITIONS (ESP32 DevKit V4)
// ============================================================
#define RXD2    16    // SIMCOM A7670C TX → ESP32 RX2
#define TXD2    17    // SIMCOM A7670C RX → ESP32 TX2
#define SS_PIN  5     // MFRC522 SDA/SS
#define RST_PIN 22    // MFRC522 RST (Sesuai wiring kita sebelumnya)
#define LED_PIN 2     // Built-in LED

// ============================================================
//  TIMING & STATE
// ============================================================
#define HEARTBEAT_INTERVAL_MS 10000  // 10 detik
#define RFID_DEBOUNCE_MS      1000   // 1 detik antar tap kartu (turun dari 2s)

MFRC522 rfid(SS_PIN, RST_PIN);
unsigned long lastHeartbeat = 0;
String lastScannedUID = "";
unsigned long lastScanTime = 0;
bool isLTEConnected = false;
bool isConnectedToRoom = false;

// ============================================================
//  AT COMMAND UTILITIES (ANTI-GAGAL ENGINE)
// ============================================================
String sendDataWaitResponse(String command, String waitString, const int timeout) {
  String response = "";
  bool found = false;

  Serial.println("\n>> " + command);
  Serial2.println(command);

  unsigned long startTime = millis();
  unsigned long lastCharTime = millis();

  // Baca karakter per karakter dengan idle-timeout 50ms
  // Jauh lebih cepat dari readString() yang defaultnya 1 DETIK per panggilan
  while ((millis() - startTime) < (unsigned long)timeout) {
    if (Serial2.available()) {
      char c = Serial2.read();
      response += c;
      lastCharTime = millis();

      // Cek apakah respons sudah mengandung string yang ditunggu
      if (response.indexOf(waitString) >= 0) {
        // Tunggu sedikit untuk sisa data masuk, lalu keluar
        delay(20);
        while (Serial2.available()) response += (char)Serial2.read();
        found = true;
        break;
      }
    } else {
      // Jika sudah ada data tapi idle >50ms, anggap respons selesai
      if (response.length() > 0 && (millis() - lastCharTime) > 50) {
        break;
      }
    }
  }

  if (found) {
    Serial.print("<< " + response);
  } else if (response.length() == 0) {
    Serial.println("<< [TIMEOUT] Modul lambat merespon...");
  }
  return response;
}

// ============================================================
//  NETWORK INITIALIZATION
// ============================================================
void initLTE() {
  Serial.println("\n[LTE] Menginisialisasi modul 4G...");

  sendDataWaitResponse("AT", "OK", 2000);
  sendDataWaitResponse("AT+HTTPTERM", "OK", 2000); // Bersihkan sesi lama

  sendDataWaitResponse("AT+CGATT=1", "OK", 5000);
  sendDataWaitResponse("AT+CGDCONT=1,\"IP\",\"" + APN + "\"", "OK", 5000);
  sendDataWaitResponse("AT+CGACT=1,1", "OK", 10000);
  sendDataWaitResponse("AT+CGPADDR", "+CGPADDR:", 5000); // Pastikan dapat IP

  isLTEConnected = true;
  Serial.println("[LTE] Jaringan siap. Mesin HTTPS standby.");
}

// ============================================================
//  HTTP POST (JSON over HTTPS)
// ============================================================
int sendHTTPPost(String endpoint, String jsonPayload) {
  if (!isLTEConnected) return -1;

  // Modul otomatis pakai SSL karena awalan url adalah "https://"
  String url = "https://" + SERVER_HOST + endpoint;

  sendDataWaitResponse("AT+HTTPTERM", "OK", 1000);
  sendDataWaitResponse("AT+HTTPINIT", "OK", 3000);

  sendDataWaitResponse("AT+HTTPPARA=\"URL\",\"" + url + "\"", "OK", 3000);
  sendDataWaitResponse("AT+HTTPPARA=\"CONTENT\",\"application/json\"", "OK", 2000);

  // 1. Kirim deklarasi panjang data, tunggu sampai muncul kata "DOWNLOAD"
  String dataCommand = "AT+HTTPDATA=" + String(jsonPayload.length()) + ",10000";
  sendDataWaitResponse(dataCommand, "DOWNLOAD", 3000);

  // 2. Tembakkan Payload JSON nya
  Serial.println(">> Mengirim JSON: " + jsonPayload);
  sendDataWaitResponse(jsonPayload, "OK", 3000);

  // 3. Eksekusi POST (Action = 1) — SSL handshake terjadi di sini, butuh waktu
  String actionResp = sendDataWaitResponse("AT+HTTPACTION=1", "+HTTPACTION:", 15000);

  // 4. Parse HTTP Status Code
  int httpCode = 0;
  int idx = actionResp.indexOf("+HTTPACTION: 1,");
  if (idx >= 0) {
    int codeStart = idx + 15;
    httpCode = actionResp.substring(codeStart, codeStart + 3).toInt();
  }

  Serial.println("[HTTP] Status Code: " + String(httpCode));

  // 5. Baca respon dari server jika berhasil (Opsional)
  if (httpCode > 0) {
    sendDataWaitResponse("AT+HTTPREAD=0,500", "+HTTPREAD:", 3000);
  }

  sendDataWaitResponse("AT+HTTPTERM", "OK", 1000);
  return httpCode;
}

// ============================================================
//  API FUNCTIONS
// ============================================================
void sendHeartbeat() {
  Serial.println("\n[API] Mengirim Heartbeat...");
  JsonDocument doc;
  doc["device_token"] = DEVICE_TOKEN;

  String payload;
  serializeJson(doc, payload);

  int httpCode = sendHTTPPost("/api/iot/heartbeat", payload);

  if (httpCode == 200) {
    Serial.println("[HB] Sukses! Terhubung ke room.");
    isConnectedToRoom = true;
    digitalWrite(LED_PIN, HIGH); 
  } else if (httpCode == 404) {
    Serial.println("[HB] Belum terhubung ke room. Menunggu binding dari Web...");
    isConnectedToRoom = false;
    blinkLED(2, 500); 
  } else {
    Serial.println("[HB] Error HTTP " + String(httpCode));
    blinkLED(3, 200);
  }
}

void sendRFIDScan(String uid) {
  Serial.println("\n[API] Mengirim RFID Scan: " + uid);
  JsonDocument doc;
  doc["device_token"] = DEVICE_TOKEN;
  doc["rfid_uid"] = uid;

  String payload;
  serializeJson(doc, payload);

  // Gunakan versi cepat: skip baca respon body (hanya perlu status code)
  if (!isLTEConnected) return;
  String url = "https://" + SERVER_HOST + "/api/iot/rfid-scan";

  sendDataWaitResponse("AT+HTTPTERM", "OK", 500);
  sendDataWaitResponse("AT+HTTPINIT", "OK", 2000);
  sendDataWaitResponse("AT+HTTPPARA=\"URL\",\"" + url + "\"", "OK", 2000);
  sendDataWaitResponse("AT+HTTPPARA=\"CONTENT\",\"application/json\"", "OK", 1000);

  String dataCmd = "AT+HTTPDATA=" + String(payload.length()) + ",10000";
  sendDataWaitResponse(dataCmd, "DOWNLOAD", 2000);
  Serial.println(">> JSON: " + payload);
  sendDataWaitResponse(payload, "OK", 2000);

  String actionResp = sendDataWaitResponse("AT+HTTPACTION=1", "+HTTPACTION:", 15000);

  int httpCode = 0;
  int idx = actionResp.indexOf("+HTTPACTION: 1,");
  if (idx >= 0) {
    httpCode = actionResp.substring(idx + 15, idx + 18).toInt();
  }

  // SKIP AT+HTTPREAD — tidak perlu baca body untuk scan
  sendDataWaitResponse("AT+HTTPTERM", "OK", 500);

  Serial.println("[SCAN] Status: " + String(httpCode));
  if (httpCode == 200) {
    Serial.println("[SCAN] [+] Kartu berhasil dikirim!");
    blinkLED(2, 150);  // Blink lebih cepat
  } else {
    Serial.println("[SCAN] [-] Ditolak. Kode: " + String(httpCode));
    blinkLED(5, 100);
  }
}

// ============================================================
//  RFID UTILITIES
// ============================================================
String getUID() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10)
      uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

void blinkLED(int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(delayMs);
    digitalWrite(LED_PIN, LOW);
    delay(delayMs);
  }
  if (isConnectedToRoom) digitalWrite(LED_PIN, HIGH);
}

// ============================================================
//  SETUP & LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, RXD2, TXD2);
  Serial2.setTimeout(100); // Penting: kurangi default 1000ms → 100ms
  delay(500);

  Serial.println("\n====================================");
  Serial.println("  AUDIRA IoT — STABLE EDITION v2.0");
  Serial.println("====================================");

  pinMode(LED_PIN, OUTPUT);
  blinkLED(3, 200);

  SPI.begin();
  rfid.PCD_Init();
  Serial.println("[RFID] Sensor Aktif.");

  initLTE();

  // Heartbeat Perdana
  sendHeartbeat();
  lastHeartbeat = millis();
}

void loop() {
  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    sendHeartbeat();
    lastHeartbeat = millis();
  }

  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String uid = getUID();

    if (uid != lastScannedUID || millis() - lastScanTime > RFID_DEBOUNCE_MS) {
      lastScannedUID = uid;
      lastScanTime = millis();
      
      // Jika berhasil baca kartu, langsung kirim ke server
      sendRFIDScan(uid);
    }
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }
}