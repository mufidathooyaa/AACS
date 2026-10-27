// Fix: one selected sensor for OLED + MQTT + fuzzy, persistent node pairing.
// Target: ESP32 DevKit, Arduino-ESP32 3.x, ArduinoJson 7.
// Keep the ORIGINAL fuzzy_logic.h here. Battery uses an additional ESP-NOW packet.
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_SHT31.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRac.h>
#include <WiFi.h>
#include "esp_eap_client.h"
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_attr.h>
#include "button_hold.h"
#include "sensor_reference.h"
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include "fuzzy_logic.h"
#include <esp_now.h>
#define PIN_BTN_RESET 13 // Pushbutton: GPIO13 ke GND, aktif LOW.
#include "protocol.h"
// -----------------------------------------
// KONFIGURASI MEMORI & BLE
// -----------------------------------------
Preferences preferences;
constexpr const char* CONFIG_NAMESPACE = "ac_config";
SemaphoreHandle_t configMutex = nullptr;
std::atomic<bool> factoryResetInProgress{false};
std::atomic<bool> factoryResetRequested{false};
RTC_DATA_ATTR uint32_t resetButtonLatch = 0;

bool publishFactoryResetEvent();

// Serialize Preferences access between BLE, loop, and the reset task.
class ConfigGuard {
 public:
  ConfigGuard() { xSemaphoreTake(configMutex, portMAX_DELAY); }
  ~ConfigGuard() { xSemaphoreGive(configMutex); }
};

void factoryResetNow() {
  if (factoryResetInProgress.exchange(true)) return;
  publishFactoryResetEvent();
  Serial.println("FACTORY RESET: menghapus ac_config...");
  // Keep this mutex until restart, so BLE cannot write the credentials back.
  xSemaphoreTake(configMutex, portMAX_DELAY);
  Preferences resetPrefs;
  bool opened = resetPrefs.begin(CONFIG_NAMESPACE, false);
  bool cleared = opened && resetPrefs.clear();
  if (opened) resetPrefs.end();
  if (!cleared) {
    Serial.println("RESET GAGAL: NVS tidak bisa dihapus. Lepas tombol lalu coba lagi.");
    factoryResetInProgress.store(false);
    xSemaphoreGive(configMutex);
    return;
  }
  Serial.println("Konfigurasi terhapus. Restart menuju pairing BLE.");
  resetButtonLatch = 0xAAC50001; // Wait for release after a software restart.
  vTaskDelay(pdMS_TO_TICKS(100));
  ESP.restart();
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

void resetButtonTask(void*) {
  ButtonHold resetButton;
  if (resetButtonLatch == 0xAAC50001) {
    do {
      while (digitalRead(PIN_BTN_RESET) == LOW) vTaskDelay(pdMS_TO_TICKS(10));
      vTaskDelay(pdMS_TO_TICKS(50));
    } while (digitalRead(PIN_BTN_RESET) == LOW);
    resetButtonLatch = 0;
  }
  for (;;) {
    if (resetButton.update(digitalRead(PIN_BTN_RESET) == LOW, millis())) {
      factoryResetRequested.store(true);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

#define BLE_SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
bool isProvisioned = false;
String savedSSID = "";
String savedUsername = "";
String savedIdentity = "";
String savedEapMethod = "PEAP";
String savedPass = "";
String savedRoomId = "";
String savedBrand = "";
// Variabel MQTT dinamis
String topicCommand = "";
String topicOccupancy = "";
String topicSensor = "";
String topicNodeBattery = "";
String topicAcStatus = "";
String topicNotification = "";
// -----------------------------------------
// KONFIGURASI MQTT (Tetap Hardcoded)
// -----------------------------------------
const char* mqtt_server = "f62988abf955495196dcc5ef9ec94248.s1.eu.hivemq.cloud"; 
const int   mqtt_port = 8883;
const char* mqtt_user = "AACS";  
const char* mqtt_pass = "Capstone123";  
WiFiClientSecure espClient;
PubSubClient client(espClient);

bool publishFactoryResetEvent() {
  if (!isProvisioned || savedRoomId.isEmpty() || !client.connected()) {
    Serial.println("[FACTORY_RESET] Event tidak terkirim: MQTT sedang offline.");
    return false;
  }

  const String payload = String("{\"type\":\"DEVICE_FACTORY_RESET\",\"room_id\":\"") +
      savedRoomId +
      "\",\"message\":\"Remote AC di-reset dan dilepas otomatis\"}";
  const bool published = client.publish(topicNotification.c_str(), payload.c_str());
  if (published) {
    client.loop();
    delay(250);
    Serial.println("[FACTORY_RESET] Event reset dikirim ke aplikasi.");
  } else {
    Serial.println("[FACTORY_RESET] Publish event reset gagal.");
  }
  return published;
}
// -----------------------------------------
// KONFIGURASI PIN ESP32 DEVKIT
// -----------------------------------------
// I2C Bus 1 (Layar OLED)
#define I2C_OLED_SDA 21
#define I2C_OLED_SCL 22
// I2C Bus 2 (Sensor SHT31)
#define I2C_SHT_SDA 32
#define I2C_SHT_SCL 33
const uint16_t kIrLed = 4; 
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET    -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_SHT31 sht31 = Adafruit_SHT31(&Wire1);
#include <ir_Panasonic.h>
#include <ir_Gree.h>
#include <ir_Coolix.h>
#include <ir_LG.h>
IRPanasonicAc* acPanasonic = NULL;
IRGreeAC* acGree = NULL;
IRCoolixAC* acCoolix = NULL;
IRLgAc* acLg = NULL;
// -----------------------------------------
// VARIABEL STATUS AC
// -----------------------------------------
bool acPowerState = false;
bool modeFuzzy = false; // Mode kontrol cerdas
portMUX_TYPE sensorMux = portMUX_INITIALIZER_UNLOCKED;
aacs::SensorReferenceState sensorState;
bool sensorReferenceConfigured = false;
aacs::ActiveReading activeReading; // Loop-only snapshot used by all consumers.
aacs::NodeBatteryReading activeNodeBattery; // Loop-only, independent of selected T/RH.
bool telemetryDirty = true;
bool batteryDirty = true;
std::atomic<bool> flagNodeBatteryUpdate{false};
bool displayDirty = true;
bool candidatePublishPending = false;
bool shtReady = false;
constexpr uint16_t NODE_BATTERY_LOW_MV = 3300;
constexpr uint16_t NODE_BATTERY_RECOVER_MV = 3450;
// Node normally contacts the remote about every 5-6 seconds. Give roughly
// three missed cycles plus margin so radio delay or one lost cycle does not
// immediately force the source back to REMOTE.
constexpr uint32_t NODE_OFFLINE_TIMEOUT_MS = 20000;
bool nodePreviouslyConnected = false;
bool nodeOfflineEventPending = false;
bool nodeLowBatteryLatched = false;
bool nodeLowBatteryEventPending = false;
uint8_t targetSuhu = 24;
String targetMode = "COOL";
String targetFan = "AUTO";
String targetSwing = "AUTO";
bool flagUbahSwing = false;
float tempHist[3] = {0,0,0};
float humHist[3] = {0,0,0};
uint8_t histIdx = 0;
bool histFilled = false;
int jumlahOrang = 0;
bool occupancyKnown = false;
uint32_t lastOccupancyUpdate = 0;
bool emptyRoomTimerActive = false;
uint32_t emptyRoomSince = 0;
// Python mengirim heartbeat setiap 15 detik. Setelah empat heartbeat terlewat,
// nilai lama tidak boleh lagi mengendalikan Fuzzy atau timer ROOM_EMPTY.
constexpr uint32_t OCCUPANCY_STALE_MS = 60UL * 1000UL;
constexpr uint32_t EMPTY_ROOM_NOTIFICATION_MS = 5UL * 60UL * 1000UL;
unsigned long lastShtReadTime = 0;
unsigned long lastFuzzyControlTime = 0;
// Memaksa satu pengiriman IR setelah Fuzzy aktif/AC perlu dinyalakan. Target
// suhu dihitung lebih dahulu agar tidak ada tembakan IR dengan setelan lama.
bool fuzzyIrPending = false;
const unsigned long shtInterval = 5000; 
constexpr float POLY_A_D = -0.079333553f;
constexpr float POLY_B_D =  4.910880485f;
constexpr float POLY_C_D = -47.819913147f;
constexpr float POLY_LINEAR_START_RAW_D = 29.452823f;
float correctTemperatureD(float rawTemperature) {
  if (!isfinite(rawTemperature)) return NAN;

  const float polynomialAtRaw =
    POLY_A_D * rawTemperature * rawTemperature +
    POLY_B_D * rawTemperature + POLY_C_D;
  if (rawTemperature <= POLY_LINEAR_START_RAW_D) return polynomialAtRaw;

  // Sambung linear setelah hasil koreksi mencapai 28 C. Nilai tetap kontinu
  // tetapi tidak lagi mengikuti sisi turun kurva kuadrat.
  const float polynomialAtBoundary =
    POLY_A_D * POLY_LINEAR_START_RAW_D * POLY_LINEAR_START_RAW_D +
    POLY_B_D * POLY_LINEAR_START_RAW_D + POLY_C_D;
  return polynomialAtBoundary + (rawTemperature - POLY_LINEAR_START_RAW_D);
}
// Deklarasi Fungsi
void startBLEProvisioning();
void setupWiFiMQTT();
void reconnectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void kirimPerintahAC();
void updateDisplay();
void buildTopics();
void serviceNetwork();
void refreshActiveReading();
void publishActiveTelemetry();
void publishNodeBattery();
void publishAcStatus();
void serviceNodeSafety(uint32_t now);
void serviceOccupancyAutomation(uint32_t now);
void readRemoteSensor(uint32_t now);
void printNodeDiagnostics();
bool setSensorReference(bool node);
std::atomic<bool> shouldRestart{false};
unsigned long restartTime = 0;
// -----------------------------------------
// KELAS CALLBACK BLUETOOTH
// -----------------------------------------
class MyCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    if (shouldRestart.load() || factoryResetInProgress.load()) return;
    String rxValue = characteristic->getValue().c_str();
    if (rxValue.isEmpty()) return;
    // Never print the raw provisioning JSON: it contains a password.
    Serial.println("Konfigurasi BLE diterima.");
    JsonDocument doc;
    if (deserializeJson(doc, rxValue)) {
      characteristic->setValue("ERROR:INVALID_JSON");
      Serial.println("JSON BLE tidak valid.");
      return;
    }
    // Numeric-only passwords must still be sent as JSON strings.
    if (!doc["ssid"].is<const char*>() ||
        !doc["pass"].is<const char*>() ||
        !doc["room"].is<const char*>() ||
        (!doc["username"].isNull() && !doc["username"].is<const char*>()) ||
        (!doc["identity"].isNull() && !doc["identity"].is<const char*>()) ||
        (!doc["eap_method"].isNull() && !doc["eap_method"].is<const char*>())) {
      characteristic->setValue("ERROR:FIELDS_MUST_BE_STRINGS");
      Serial.println("SSID, username, pass, room harus string; pass angka tetap diapit tanda kutip.");
      return;
    }
    String newSSID = doc["ssid"].as<String>();
    String newUsername = doc["username"] | "";
    String newPass = doc["pass"].as<String>();
    String newRoom = doc["room"].as<String>();
    String newBrand = doc["brand"] | "Panasonic";
    String newIdentity = doc["identity"] | newUsername.c_str();
    String newEapMethod = doc["eap_method"] | "PEAP";
    newUsername.trim();
    newRoom.trim();
    newEapMethod.toUpperCase();
    // Do not trim the password or SSID; spaces can be intentional.
    if (newSSID.isEmpty() || newSSID.length() > 32 || newRoom.isEmpty() ||
        (newEapMethod != "PEAP" && newEapMethod != "TTLS") ||
        (!newUsername.isEmpty() && newPass.isEmpty()) ||
        (newSSID == "UNNES-ID" && newUsername.isEmpty())) {
      characteristic->setValue("ERROR:INVALID_CONFIG");
      Serial.println("Konfigurasi tidak lengkap; UNNES-ID memerlukan username.");
      return;
    }
    {
      ConfigGuard guard;
      if (!preferences.begin(CONFIG_NAMESPACE, false)) {
        characteristic->setValue("ERROR:NVS_OPEN");
        return;
      }
      // room is the provisioning marker; write it last, only after all fields succeed.
      preferences.remove("room");
      bool ok = preferences.putString("ssid", newSSID) == newSSID.length();
      ok &= preferences.putString("username", newUsername) == newUsername.length();
      ok &= preferences.putString("pass", newPass) == newPass.length();
      ok &= preferences.putString("brand", newBrand) == newBrand.length();
      ok &= preferences.putString("identity", newIdentity) == newIdentity.length();
      ok &= preferences.putString("eap_method", newEapMethod) == newEapMethod.length();
      if (ok) ok = preferences.putString("room", newRoom) == newRoom.length();
      preferences.end();
      if (!ok) {
        characteristic->setValue("ERROR:NVS_WRITE");
        Serial.println("Konfigurasi gagal disimpan; kirim ulang dari aplikasi.");
        return;
      }
    }
    // This is a saved-config response, NOT proof that Wi-Fi authentication succeeded.
    characteristic->setValue("CONFIG_SAVED");
    Serial.println("Konfigurasi disimpan. Restart dalam 2 detik.");
    restartTime = millis();
    shouldRestart.store(true);
  }
};
// ==========================================
// ESP-NOW: USER NODE PAIRING & TELEMETRY
// ==========================================
#include "remote_node_link.h"
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN_RESET, INPUT_PULLUP);
  configMutex = xSemaphoreCreateMutex();
  if (!configMutex || xTaskCreate(resetButtonTask, "reset_button", 4096,
                                 nullptr, 2, nullptr) != pdPASS) {
    Serial.println("GAGAL membuat task tombol reset.");
    for (;;) delay(1000);
  }
  Serial.println("Tahan pushbutton GPIO13 ke GND selama 3 detik untuk factory reset.");
  Wire.begin(I2C_OLED_SDA, I2C_OLED_SCL);
  Wire1.begin(I2C_SHT_SDA, I2C_SHT_SCL);
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("OLED Gagal!"));
    for (;;) delay(1000); // Button task remains available.
  }
  // Read the SAME namespace used by provisioning and factory reset.
  size_t freeEntries = 0;
  {
    ConfigGuard guard;
    if (preferences.begin(CONFIG_NAMESPACE, true)) {
      savedSSID = preferences.getString("ssid", "");
      savedUsername = preferences.getString("username", "");
      savedPass = preferences.getString("pass", "");
      savedIdentity = preferences.getString("identity", savedUsername);
      savedEapMethod = preferences.getString("eap_method", "PEAP");
      savedRoomId = preferences.getString("room", "");
      savedBrand = preferences.getString("brand", "Panasonic");
      sensorReferenceConfigured = preferences.isKey("use_node");
      sensorState.selectedNode = preferences.getBool("use_node", false);
      freeEntries = preferences.freeEntries();
      preferences.end();
    }
  }
  savedUsername.trim();
  savedEapMethod.toUpperCase();
  // DEBUG: Tampilkan isi memori yang terbaca
  Serial.println("=== DEBUG MEMORI ===");
  Serial.println("SSID: [" + savedSSID + "]");
  Serial.println("Room: [" + savedRoomId + "]");
  Serial.println("Brand: [" + savedBrand + "]");
  Serial.printf("NVS Free Entries: %u\n", static_cast<unsigned>(freeEntries));
  Serial.printf("Username tersimpan: %s | EAP: %s | Password: %u karakter\n",
                savedUsername.isEmpty() ? "TIDAK" : "YA", savedEapMethod.c_str(),
                static_cast<unsigned>(savedPass.length()));
  Serial.println("====================");
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 10);
  if (savedSSID == "" || savedRoomId == "" ||
      (savedSSID == "UNNES-ID" && savedUsername.isEmpty())) {
    // Mode Pairing Bluetooth
    isProvisioned = false;
    display.println("Mode Pairing BLE...");
    display.display();
    startBLEProvisioning();
  } else {
    // Mode Normal WiFi
    isProvisioned = true;
    Serial.println("WiFi: " + savedSSID + " | Room ID: " + savedRoomId);
    display.println("Connecting WiFi...");
    display.display();
    // Inisialisasi semua protokol AC agar bisa berganti merek secara instan tanpa restart
    acGree = new IRGreeAC(kIrLed);
    acGree->begin();
    acCoolix = new IRCoolixAC(kIrLed);
    acCoolix->begin();
    acLg = new IRLgAc(kIrLed);
    acLg->begin();
    acPanasonic = new IRPanasonicAc(kIrLed);
    acPanasonic->begin();
    acPanasonic->setModel(kPanasonicRkr);
    buildTopics();
    loadNodePair();
    Wire1.setTimeOut(50);
    shtReady = sht31.begin(0x44);
    if (!shtReady) Serial.println("SHT31 Gagal! Akan dicoba lagi tiap 5 detik.");
    if (shtReady) sht31.heater(false);
    readRemoteSensor(millis()); // No fake zero-valued startup telemetry.
    lastShtReadTime = millis();
    refreshActiveReading();
    setupWiFiMQTT();
    updateDisplay();
  }
}
void loop() {
  if (factoryResetRequested.exchange(false)) {
    factoryResetNow();
    return;
  }
  if (shouldRestart) {
    if (millis() - restartTime >= 2000) {
      Serial.println("Melakukan restart sekarang...");
      ESP.restart();
    }
    delay(10);
    return; // Task pushbutton tetap berjalan selama menunggu restart
  }
  if (!isProvisioned) {
    // Tunggu koneksi BLE, diam saja
    delay(100);
    return;
  }
  serviceNetwork();
  if (client.connected()) client.loop();
  if (factoryResetRequested.exchange(false)) {
    factoryResetNow();
    return;
  }
  unsigned long currentMillis = millis();
    if (flagNewCandidate.exchange(false)) candidatePublishPending = true;
    if (candidatePublishPending && client.connected()) {
        uint8_t candidate[6];
        portENTER_CRITICAL(&sensorMux);
        memcpy(candidate, candidateNoticeMac, 6);
        portEXIT_CRITICAL(&sensorMux);
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", candidate[0], candidate[1], candidate[2], candidate[3], candidate[4], candidate[5]);
        String payload = String("{\"mac\":\"") + macStr + "\"}";
        if (client.publish(("room/" + savedRoomId + "/pairing/candidate").c_str(), payload.c_str())) {
          candidatePublishPending = false;
          Serial.println("Kandidat Node dikirim ke Aplikasi: " + String(macStr));
        }
    }
    if (flagPairingSuccess.exchange(false)) {
        Serial.println("Pairing Selesai dan Sukses!");
        publishAcStatus();
    }
  bool sensorTick = false;
  if (currentMillis - lastShtReadTime >= shtInterval) {
    lastShtReadTime = currentMillis;
    readRemoteSensor(currentMillis);
    sensorTick = true;
  }
  refreshActiveReading(); // Includes source changes and node timeout, every loop.
  serviceNodeSafety(currentMillis);
  if (flagNodeBatteryUpdate.exchange(false)) batteryDirty = true;
  printNodeDiagnostics();
  if (sensorTick) telemetryDirty = true; // Keep the existing 5-second heartbeat.
  publishActiveTelemetry();
  publishNodeBattery(); // Independent of temperature/humidity availability.
  serviceOccupancyAutomation(currentMillis);
  if (modeFuzzy && occupancyKnown && jumlahOrang > 0 && activeReading.reading.valid &&
      (currentMillis - lastFuzzyControlTime >= 30000)) {
        lastFuzzyControlTime = currentMillis;
        const float suhuFuzzy = activeReading.reading.temperature;
        int target_baru = jalankan_inferensi_fuzzy(suhuFuzzy, jumlahOrang);
        Serial.printf("[FUZZY] SRC=%s T=%.1f Orang=%d target=%d sebelumnya=%d\n",
                      activeReading.source == aacs::Source::NODE ? "NODE" : "REMOTE",
                      suhuFuzzy, jumlahOrang, target_baru, targetSuhu);
        // Koreksi fan hasil sinkronisasi meskipun setpoint Fuzzy tetap sama.
        const bool fuzzyFanChanged = targetFan != "LOW";
        targetFan = "LOW";
        const bool shouldSendFuzzyIr = fuzzyIrPending || target_baru != targetSuhu || fuzzyFanChanged;
        targetSuhu = target_baru;
        if (shouldSendFuzzyIr) {
            if (fuzzyIrPending) acPowerState = true;
            fuzzyIrPending = false;
            kirimPerintahAC();
            publishAcStatus();
        }
  }
  if (sensorTick) {
    publishAcStatus();
  }
  updateDisplay();
  delay(10); // Yield CPU for radio worker; OLED is rate-limited below.
}

void readRemoteSensor(uint32_t now) {
  if (!shtReady) {
    shtReady = sht31.begin(0x44);
    if (shtReady) sht31.heater(false);
  }
  float t = NAN, h = NAN;
  bool valid = shtReady && sht31.readBoth(&t, &h) && aacs::validValues(t, h);
  const float corrected = correctTemperatureD(t);
  valid = valid && aacs::validValues(corrected, h);
  float meanT = NAN, meanH = NAN;
  if (valid) {
    tempHist[histIdx] = corrected; humHist[histIdx] = h;
    if (histIdx == 2) histFilled = true;
    const uint8_t count = histFilled ? 3 : histIdx+1;
    meanT = 0; meanH = 0;
    for (uint8_t i = 0; i < count; ++i) { meanT += tempHist[i]; meanH += humHist[i]; }
    meanT /= count; meanH /= count;
    histIdx = (histIdx+1)%3;
  } else {
    histIdx = 0; histFilled = false;
    shtReady = false;
    Serial.println("Sensor remote invalid; tidak mengirim suhu palsu.");
  }
  portENTER_CRITICAL(&sensorMux);
  sensorState.setRemote(meanT, meanH, valid, now);
  portEXIT_CRITICAL(&sensorMux);
}

void refreshActiveReading() {
  aacs::ActiveReading next;
  aacs::NodeBatteryReading nextBattery;
  portENTER_CRITICAL(&sensorMux);
  next = sensorState.select(millis());
  nextBattery = sensorState.selectBattery(millis());
  portEXIT_CRITICAL(&sensorMux);
  if (nextBattery.valid != activeNodeBattery.valid || nextBattery.updatedAt != activeNodeBattery.updatedAt ||
      nextBattery.millivolts != activeNodeBattery.millivolts || nextBattery.percent != activeNodeBattery.percent)
    batteryDirty = true;
  activeNodeBattery = nextBattery;
  const bool changed = next.source != activeReading.source || next.selectedNode != activeReading.selectedNode ||
      next.fallback != activeReading.fallback || next.issue != activeReading.issue ||
      next.reading.valid != activeReading.reading.valid ||
      (next.reading.valid && (next.reading.temperature != activeReading.reading.temperature ||
                            next.reading.humidity != activeReading.reading.humidity ||
                            next.reading.updatedAt != activeReading.reading.updatedAt));
  if (changed) {
    telemetryDirty = true; displayDirty = true;
    if (next.source != activeReading.source || next.selectedNode != activeReading.selectedNode ||
        next.issue != activeReading.issue) {
      Serial.printf("[SOURCE] dipilih=%s aktif=%s status=%s\n", next.selectedNode ? "NODE" : "REMOTE",
                    next.source == aacs::Source::NODE ? "NODE" : next.source == aacs::Source::REMOTE ? "REMOTE" : "NONE",
                    aacs::issueName(next.issue));
      if (next.reading.valid) lastFuzzyControlTime = millis()-30000UL;
    }
  }
  activeReading = next;
}

void printNodeDiagnostics() {
  static uint32_t reportedAt = 0;
  if (!activeReading.selectedNode || uint32_t(millis()-reportedAt) < 5000) return;
  reportedAt = millis();
  Serial.printf("[NODE_DIAG] RX=%lu bad=%lu queue_full=%lu no_pair=%lu wrong_mac=%lu accepted=%lu invalid=%lu duplicate=%lu | %s\n",
                static_cast<unsigned long>(nodeRxFrames.load()), static_cast<unsigned long>(nodeRxBadPacket.load()),
                static_cast<unsigned long>(nodeRxQueueFull.load()), static_cast<unsigned long>(nodeRxUnpaired.load()),
                static_cast<unsigned long>(nodeRxWrongMac.load()), static_cast<unsigned long>(nodeRxAccepted.load()),
                static_cast<unsigned long>(nodeRxInvalid.load()), static_cast<unsigned long>(nodeRxDuplicate.load()),
                aacs::issueName(activeReading.issue));
}

void publishActiveTelemetry() {
  if (!telemetryDirty || !client.connected()) return;
  // Preserve the existing temperature/humidity contract exactly.
  // Both invalid: no zero/NaN/null fabricated payload; pause telemetry and fuzzy.
  if (!activeReading.reading.valid) return;
  // T dan RH selalu satu pasangan dari snapshot aktif yang sama. Nilainya akan
  // berasal dari sensor node saat USER_NODE dipilih, atau sensor remote saat
  // REMOTE dipilih. Kedua sensor tetap dibaca/disimpan secara independen.
  const float T = activeReading.reading.temperature;
  const float RH = activeReading.reading.humidity;
  const String payload = String("{\"suhu\":") + String(T, 2) +
                         ",\"kelembaban\":" + String(RH, 2) + "}";
  if (client.publish(topicSensor.c_str(), payload.c_str())) telemetryDirty = false;
}

void publishNodeBattery() {
  if (!batteryDirty || !client.connected()) return;
  String payload = String("{\"version\":1,\"valid\":") +
                   (activeNodeBattery.valid ? "true" : "false");
  if (activeNodeBattery.valid) {
    payload += String(",\"millivolts\":") + String(activeNodeBattery.millivolts) +
               ",\"percent\":" + String(activeNodeBattery.percent);
  } else {
    payload += ",\"millivolts\":null,\"percent\":null";
  }
  payload += "}";
  if (client.publish(topicNodeBattery.c_str(), payload.c_str())) batteryDirty = false;
}

void publishAcStatus() {
  if (!client.connected()) return;
  bool nodePaired;
  bool nodeConnected;
  bool selectedNode;
  const uint32_t now = millis();
  portENTER_CRITICAL(&sensorMux);
  nodePaired = sensorState.nodePaired;
  selectedNode = sensorState.selectedNode;
  portEXIT_CRITICAL(&sensorMux);
  nodeConnected = nodePaired && pairedNodeContactSeen.load() &&
      uint32_t(now-lastPairedNodeContactAt.load()) < NODE_OFFLINE_TIMEOUT_MS;
  // Pertahankan field lama power+suhu dan tambahkan state lengkap agar backend
  // dapat mengembalikan mode FUZZY/MANUAL lewat endpoint latest-status.
  const String payload = String("{\"power\":") + (acPowerState ? "true" : "false") +
      ",\"suhu\":" + targetSuhu +
      ",\"mode\":\"" + (modeFuzzy ? "FUZZY" : "MANUAL") + "\"" +
      ",\"ac_mode\":\"" + targetMode + "\"" +
      ",\"fan_speed\":\"" + targetFan + "\"" +
      ",\"swing\":\"" + targetSwing + "\"" +
      ",\"control_source\":\"" + (selectedNode ? "USER_NODE" : "REMOTE") + "\"" +
      ",\"node_paired\":" + (nodePaired ? "true" : "false") +
      ",\"node_connected\":" + (nodeConnected ? "true" : "false") +
      ",\"node_battery_low\":" + (nodeLowBatteryLatched ? "true" : "false") + "}";
  client.publish(topicAcStatus.c_str(), payload.c_str());
}

bool publishNodeLifecycleEvent(const char* type, const String& message) {
  if (!client.connected()) return false;
  String payload = String("{\"type\":\"") + type +
      "\",\"room_id\":\"" + savedRoomId + "\",\"message\":\"" + message + "\"";
  if (activeNodeBattery.valid) {
    payload += String(",\"millivolts\":") + activeNodeBattery.millivolts +
        ",\"percent\":" + activeNodeBattery.percent;
  }
  payload += "}";
  return client.publish(topicNotification.c_str(), payload.c_str());
}

void serviceNodeSafety(uint32_t now) {
  bool nodePaired;
  bool nodeConnected;
  bool selectedNode;
  aacs::NodeBatteryReading battery;
  portENTER_CRITICAL(&sensorMux);
  nodePaired = sensorState.nodePaired;
  selectedNode = sensorState.selectedNode;
  battery = sensorState.selectBattery(now);
  portEXIT_CRITICAL(&sensorMux);
  nodeConnected = nodePaired && pairedNodeContactSeen.load() &&
      uint32_t(now-lastPairedNodeContactAt.load()) < NODE_OFFLINE_TIMEOUT_MS;

  // Unpair disengaja bukan kejadian NODE_OFFLINE.
  if (!nodePaired) {
    nodePreviouslyConnected = false;
    nodeOfflineEventPending = false;
    nodeLowBatteryLatched = false;
    nodeLowBatteryEventPending = false;
    return;
  }

  if (nodeConnected && !nodePreviouslyConnected) {
    nodePreviouslyConnected = true;
    publishAcStatus();
  } else if (!nodeConnected && nodePreviouslyConnected) {
    nodePreviouslyConnected = false;
    nodeOfflineEventPending = true;
    if (selectedNode && setSensorReference(false)) {
      Serial.println("[NODE_OFFLINE] Referensi otomatis dipindahkan ke REMOTE.");
    }
    publishAcStatus();
  }

  const bool batteryLow = battery.valid && battery.millivolts <= NODE_BATTERY_LOW_MV;
  if (batteryLow && !nodeLowBatteryLatched) {
    nodeLowBatteryLatched = true;
    nodeLowBatteryEventPending = true;
    if (selectedNode && setSensorReference(false)) {
      Serial.println("[NODE_BATTERY_LOW] Referensi otomatis dipindahkan ke REMOTE.");
    }
    publishAcStatus();
  } else if (battery.valid && battery.millivolts >= NODE_BATTERY_RECOVER_MV) {
    nodeLowBatteryLatched = false;
    nodeLowBatteryEventPending = false;
  }

  // Pending dipertahankan selama MQTT offline agar event dicoba kembali.
  if (nodeOfflineEventPending &&
      publishNodeLifecycleEvent("NODE_OFFLINE",
          "Node sensor terputus; sumber dipindahkan ke Remote")) {
    nodeOfflineEventPending = false;
  }
  if (nodeLowBatteryEventPending &&
      publishNodeLifecycleEvent("NODE_BATTERY_LOW",
          "Baterai node 3,3 V atau lebih rendah; isi daya node")) {
    nodeLowBatteryEventPending = false;
  }
}

void serviceOccupancyAutomation(uint32_t now) {
  if (occupancyKnown &&
      uint32_t(now-lastOccupancyUpdate) > OCCUPANCY_STALE_MS) {
    occupancyKnown = false;
    emptyRoomTimerActive = false;
    Serial.println("[OCCUPANCY] Data lebih dari 60 detik; otomasi dijeda.");
    displayDirty = true;
  }
  if (!occupancyKnown) return;

  // Ruangan kosong hanya relevan saat AC sedang menyala. Mematikan AC juga
  // membatalkan interval yang sedang berjalan agar tidak ada notifikasi sisa.
  if (!acPowerState) {
    emptyRoomTimerActive = false;
    return;
  }

  if (jumlahOrang > 0) {
    emptyRoomTimerActive = false;

    if (modeFuzzy && !acPowerState && !fuzzyIrPending) {
      targetMode = "COOL";
      targetFan = "LOW";
      targetSwing = "AUTO";
      fuzzyIrPending = true;
      lastFuzzyControlTime = now-30000UL;
      displayDirty = true;
      Serial.printf("[FUZZY] Pengguna terdeteksi (%d), menunggu hasil inferensi sebelum IR.\n", jumlahOrang);
    }
    return;
  }

  if (!emptyRoomTimerActive) {
    emptyRoomTimerActive = true;
    emptyRoomSince = now;
    Serial.println("[EMPTY_ROOM] Timer 5 menit dimulai.");
    return;
  }

  if (uint32_t(now-emptyRoomSince) < EMPTY_ROOM_NOTIFICATION_MS) return;

  const String payload =
      "{\"type\":\"ROOM_EMPTY\",\"people_count\":0,\"duration_seconds\":300,"
      "\"message\":\"Ruangan tidak mendeteksi pengguna selama 5 menit\"}";
  if (client.connected() && client.publish(topicNotification.c_str(), payload.c_str())) {
    // Mulai interval berikutnya. Selama ruangan tetap kosong event akan
    // dipublish lagi setiap lima menit, baik mode MANUAL maupun FUZZY.
    emptyRoomSince = now;
    Serial.println("[EMPTY_ROOM] Notifikasi periodik 5 menit dipublish ke MQTT.");
  }
}

bool setSensorReference(bool node) {
  bool previous;
  portENTER_CRITICAL(&sensorMux);
  previous = sensorState.selectedNode;
  portEXIT_CRITICAL(&sensorMux);
  if (previous != node) {
    ConfigGuard guard;
    if (factoryResetInProgress.load() || !preferences.begin(CONFIG_NAMESPACE, false)) return false;
    const bool ok = preferences.putBool("use_node", node) == 1;
    preferences.end();
    if (!ok) return false;
    portENTER_CRITICAL(&sensorMux);
    sensorState.selectedNode = node;
    portEXIT_CRITICAL(&sensorMux);
    sensorReferenceConfigured = true;
    // Re-evaluate fuzzy using the selected source at the next loop iteration.
    lastFuzzyControlTime = millis()-30000UL;
  }
  refreshActiveReading(); telemetryDirty = true;
  publishActiveTelemetry(); updateDisplay();
  return true;
}
void buildTopics() {
  topicCommand   = "room/" + savedRoomId + "/ac/command";
  topicOccupancy = "room/" + savedRoomId + "/occupancy";
  topicSensor    = "room/" + savedRoomId + "/telemetry";
  topicNodeBattery = "room/" + savedRoomId + "/node/battery";
  topicAcStatus  = "room/" + savedRoomId + "/ac/status";
  topicNotification = "room/" + savedRoomId + "/notification";
  Serial.println("Topik diset ke Ruangan ID: " + savedRoomId);
}
void startBLEProvisioning() {
  BLEDevice::init("SMART_AC_UNNES");
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(BLE_SERVICE_UUID);
  BLECharacteristic *pCharacteristic = pService->createCharacteristic(
                                         BLE_CHARACTERISTIC_UUID,
                                         BLECharacteristic::PROPERTY_READ |
                                         BLECharacteristic::PROPERTY_WRITE
                                       );
  pCharacteristic->setCallbacks(new MyCallbacks());
  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(BLE_SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("Bluetooth Aktif! Menunggu data dari HP...");
}
// PEAP is the default assumption. Set eap_method=TTLS only if campus settings say TTLS.
unsigned long lastWiFiAttempt = 0;
unsigned long lastMqttAttempt = 0;
bool wifiWasConnected = false;
bool mqttAttempted = false;

void beginSavedWiFi() {
  Serial.println("Menghubungkan WiFi: " + savedSSID);
  lastWiFiAttempt = millis();
  if (!savedUsername.isEmpty()) {
    Serial.printf("Mode Enterprise %s | password %u karakter\n", savedEapMethod.c_str(),
                  static_cast<unsigned>(savedPass.length()));
    if (savedEapMethod == "TTLS") {
      // TTLS phase-2 in this sketch is MSCHAPv2; confirm against campus settings.
      esp_err_t err = esp_eap_client_set_ttls_phase2_method(ESP_EAP_TTLS_PHASE2_MSCHAPV2);
      if (err != ESP_OK) Serial.printf("TTLS phase2 error: %d\n", err);
      WiFi.begin(savedSSID.c_str(), WPA2_AUTH_TTLS, savedIdentity.c_str(),
                 savedUsername.c_str(), savedPass.c_str());
    } else {
      WiFi.begin(savedSSID.c_str(), WPA2_AUTH_PEAP, savedIdentity.c_str(),
                 savedUsername.c_str(), savedPass.c_str());
    }
  } else {
    esp_wifi_sta_enterprise_disable();
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());
  }
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  // Wi-Fi callback runs in another task. Only log here.
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf("WiFi terputus: reason=%u\n", info.wifi_sta_disconnected.reason);
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.println("WiFi autentikasi berhasil dan IP diperoleh.");
  }
}

void setupWiFiMQTT() {
  WiFi.persistent(false);
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Remote stays awake to receive the battery node.
  WiFi.setAutoReconnect(true);
  nodeQueue = xQueueCreate(16, sizeof(NodeEvent));
  if (nodeQueue && esp_now_init() == ESP_OK &&
      esp_now_register_recv_cb(OnDataRecv) == ESP_OK &&
      xTaskCreate(nodeRadioTask, "node_radio", 6144, nullptr, 1, nullptr) == pdPASS) {
    radioReady.store(true);
    Serial.println("ESP-NOW worker aktif");
  } else {
    Serial.println("ESP-NOW Init Failed");
  }
  // Existing prototype TLS setting retained; no CA was provided in the source.
  espClient.setInsecure();
  espClient.setHandshakeTimeout(5);
  espClient.setTimeout(5000);
  client.setSocketTimeout(3);
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  beginSavedWiFi(); // No blocking connection wait and no restart loop.
}

void serviceNetwork() {
  const unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiWasConnected) {
      // Close old MQTT transport before reopening it on the new Wi-Fi connection.
      espClient.stop();
      wifiWasConnected = false;
      mqttAttempted = false;
    }
    if (now - lastWiFiAttempt >= 45000UL) {
      Serial.println("WiFi belum terhubung; coba ulang. Konfigurasi tetap disimpan.");
      WiFi.disconnect(false, false);
      beginSavedWiFi();
    }
    return;
  }
  if (!wifiWasConnected) {
    wifiWasConnected = true;
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    Serial.printf("WiFi channel: %d\n", WiFi.channel());
  }
  reconnectMQTT();
}

void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED || client.connected()) return;
  unsigned long now = millis();
  if (mqttAttempted && now - lastMqttAttempt < 5000UL) return;
  mqttAttempted = true;
  Serial.println("Mencoba MQTT...");
  String clientId = "ESP32-AC-" + WiFi.macAddress();
  // One attempt per call. TLS can still block briefly; resetButtonTask remains active.
  if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
    Serial.println("MQTT connected");
    client.subscribe(topicCommand.c_str());
    Serial.println("[MQTT_SUB] " + topicCommand);
    client.subscribe(topicOccupancy.c_str());
    telemetryDirty = true; // Send the current reference again after reconnect.
    batteryDirty = true;   // Battery has its own topic and validity lifecycle.
  } else {
    Serial.printf("MQTT gagal: rc=%d (WiFi dan MQTT berbeda tahap).\n", client.state());
  }
  lastMqttAttempt = millis();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  String incomingTopic = String(topic);
  if (incomingTopic == topicOccupancy) {
    message.trim();
    bool validOccupancy = message.length() > 0;
    for (unsigned int i = 0; i < message.length() && validOccupancy; i++) {
      if (!isDigit(message[i])) validOccupancy = false;
    }
    if (!validOccupancy) {
      Serial.println("[OCCUPANCY] Payload bukan bilangan non-negatif; diabaikan.");
      return;
    }
    const int reportedPeople = message.toInt();
    jumlahOrang = reportedPeople > 0 ? reportedPeople : 0;
    occupancyKnown = true;
    lastOccupancyUpdate = millis();
    if (jumlahOrang > 0) lastFuzzyControlTime = lastOccupancyUpdate-30000UL;
    serviceOccupancyAutomation(lastOccupancyUpdate);
    updateDisplay();
  }
  else if (incomingTopic == topicCommand) {
    // Parse JSON dari Laravel
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message);
    if (error) {
      Serial.println("MQTT Parsing JSON Gagal!");
      return;
    }
    String command = doc["command"] | "";
    String valueStr;
    if (!doc["value"].isNull()) {
      valueStr = doc["value"].as<String>();
    } else if (command == "SET_MODE" && !doc["mode"].isNull()) {
      // Kompatibilitas APK lama yang mengirim {command, mode}.
      valueStr = doc["mode"].as<String>();
      Serial.println("[MQTT_CMD] SET_MODE memakai key mode lama; diterima sebagai kompatibilitas.");
    } else if (command == "UNPAIR") {
      // Factory reset remote memang tidak membutuhkan value.
      valueStr = "";
    } else {
      Serial.printf("[MQTT_CMD] command=%s ditolak: key value tidak ada.\n", command.c_str());
      return;
    }
    bool adaPerubahan = false;
    if (command == "SET_POWER") {
      if (valueStr == "ON") { acPowerState = true; adaPerubahan = true; }
      else if (valueStr == "OFF") { acPowerState = false; adaPerubahan = true; }
    } 
    else if (command == "SET_TEMPERATURE") {
      int newTemp = valueStr.toInt();
      if (newTemp >= 16 && newTemp <= 30) {
        targetSuhu = newTemp;
        acPowerState = true; // Otomatis nyala kalau suhu diubah
        adaPerubahan = true;
      }
    }
    else if (command == "SET_MODE") {
        const bool wasFuzzy = modeFuzzy;
        if (valueStr == "FUZZY") {
          const bool fuzzyFanChanged = targetFan != "LOW";
          modeFuzzy = true;
          // AC baru otomatis menyala setelah okupansi > 0.
          targetMode = "COOL";
          targetFan = "LOW";
          targetSwing = "AUTO";
          // Simpan sampai input sensor + okupansi valid. Loop Fuzzy tetap
          // mensyaratkan jumlahOrang > 0, jadi flag ini aman saat ruang kosong.
          if (!wasFuzzy || fuzzyFanChanged) {
            fuzzyIrPending = true;
          }
        }
        else if (valueStr == "MANUAL") {
          modeFuzzy = false;
          fuzzyIrPending = false;
        }
        else {
          Serial.printf("[MQTT_CMD] SET_MODE value tidak valid: %s\n", valueStr.c_str());
          return;
        }
        // Jalankan inferensi pertama segera pada iterasi loop berikutnya.
        lastFuzzyControlTime = millis()-30000UL;
        displayDirty = true;
        updateDisplay();
        // SET_MODE hanya mengubah mode/ACK. IR baru dikirim oleh loop setelah
        // output Fuzzy selesai dihitung, sehingga tidak ada tembakan setelan lama.
        publishAcStatus();
        return;
      }
      else if (command == "START_PAIRING") {
        bool queued = false;
        if (valueStr == "OPEN") {
            queued = queueNodeCommand(NodeEventKind::OPEN);
        } else if (valueStr == "REPLACE") {
            queued = queueNodeCommand(NodeEventKind::REPLACE);
        } else if (valueStr == "APPROVE") {
            queued = queueNodeCommand(NodeEventKind::APPROVE);
        } else if (valueStr == "REJECT" || valueStr == "CLOSE") {
            queued = queueNodeCommand(NodeEventKind::REJECT);
        } else if (valueStr == "UNPAIR") {
            queued = queueNodeCommand(NodeEventKind::UNPAIR);
        }
        if (!queued) Serial.println("Perintah pairing ditolak: radio/queue/value tidak tersedia.");
        return;
      }
      else if (command == "SET_CONTROL_SOURCE" || command == "set_sensor_reference") {
        // Existing backend command is retained; lower-case alias is optional.
        String source = valueStr;
        if (command == "set_sensor_reference") source = doc["reference"] | valueStr.c_str();
        source.toUpperCase();
        Serial.printf("[SRC_CMD] value=%s\n", source.c_str());
        const bool node = source == "USER_NODE" || source == "NODE";
        const bool remote = source == "REMOTE" || source == "REMOTE_SENSOR" ||
                            source == "MAIN_REMOTE" || source == "MAIN_SENSOR" ||
                            source == "MAIN" || source == "INTERNAL";
        if (!node && !remote) {
          Serial.println("Referensi tidak valid; pilihan sebelumnya dipertahankan."); return;
        }
        if (!setSensorReference(node)) Serial.println("Pilihan referensi gagal disimpan.");
        return;
      }
      else if (command == "SET_AC_MODE") {
      targetMode = valueStr;
      acPowerState = true; 
      adaPerubahan = true;
    }
    else if (command == "SET_FAN_SPEED") {
      targetFan = valueStr;
      acPowerState = true;
      adaPerubahan = true;
    }
    else if (command == "SET_SWING") {
      targetSwing = valueStr;
      acPowerState = true;
      adaPerubahan = true;
      flagUbahSwing = true;
    }
    else if (command == "SET_BRAND") {
      savedBrand = valueStr;
      {
        ConfigGuard guard;
        if (preferences.begin(CONFIG_NAMESPACE, false)) {
          preferences.putString("brand", savedBrand);
          preferences.end();
        }
      }
      Serial.println("Brand AC diganti ke: " + savedBrand);
      // Update display dengan brand baru
      display.clearDisplay();
      display.setCursor(0, 10);
      display.println("Brand: " + savedBrand);
      display.display();
      delay(1500);
      updateDisplay();
      return;
    }
    else if (command == "SYNC_STATE") {
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, valueStr);
      if (!error) {
         String pwr = doc["power"];
         int temp = doc["setpoint"];
         String mde = doc["ac_mode"];
         String fn = doc["fan_speed"];
         String swg = doc["swing"];
         String controlMode = doc["mode"] | "MANUAL";
         acPowerState = (pwr == "ON");
         targetSuhu = temp;
         targetMode = mde;
         targetFan = fn;
         targetSwing = swg;
         modeFuzzy = (controlMode == "FUZZY");
         Serial.println("Sinkronisasi State: " + pwr + ", " + String(temp) + "C (Tanpa tembakan IR)");
         updateDisplay();
      }
      return; // Return langsung agar tidak memicu kirimPerintahAC()
    }
    // TAMBAHKAN UNPAIR JIKA DIHAPUS DARI HP
    else if (command == "UNPAIR") {
      factoryResetRequested.store(true);
      return;
    }
    if (adaPerubahan) {
      kirimPerintahAC();
      updateDisplay();
      // Laporkan balik status terbaru ke MQTT supaya HP langsung update
      publishAcStatus();
    }
  }
}
void kirimPerintahAC() {
  // Selama Fuzzy aktif, setiap perintah IR memakai fan paling rendah.
  if (modeFuzzy) targetFan = "LOW";
  Serial.printf("Mengirim sinyal IR... Brand: %s, Power: %d, Temp: %d, Mode: %s, Fan: %s\n", 
                savedBrand.c_str(), acPowerState, targetSuhu, targetMode.c_str(), targetFan.c_str());
  if (savedBrand == "Gree" && acGree != NULL) {
    if (acPowerState) {
      acGree->on(); 
      acGree->setTemp(targetSuhu);
      // Mode
      if (targetMode == "COOL") acGree->setMode(kGreeCool);
      else if (targetMode == "DRY") acGree->setMode(kGreeDry);
      else if (targetMode == "AUTO") acGree->setMode(kGreeAuto);
      else if (targetMode == "FAN") acGree->setMode(kGreeFan);
      else if (targetMode == "HEAT") acGree->setMode(kGreeHeat);
      // Fan
      if (targetFan == "LOW") acGree->setFan(kGreeFanMin);
      else if (targetFan == "MEDIUM") acGree->setFan(kGreeFanMed);
      else if (targetFan == "HIGH") acGree->setFan(kGreeFanMax);
      else if (targetFan == "AUTO") acGree->setFan(kGreeFanAuto);
      // Swing (Gree uses 0-11 for swing, or kGreeSwingAuto)
      if (targetSwing == "AUTO") acGree->setSwingVertical(true, kGreeSwingAuto);
      else if (targetSwing == "OFF") acGree->setSwingVertical(false, kGreeSwingAuto);
      else acGree->setSwingVertical(true, kGreeSwingUp); // Fallback manual swing
    } else {
      acGree->off();
    }
    acGree->send();
  } 
  else if (savedBrand == "Electrolux" && acCoolix != NULL) {
    if (acPowerState) {
      acCoolix->on(); 
      acCoolix->setTemp(targetSuhu);
      // Mode
      if (targetMode == "COOL") acCoolix->setMode(kCoolixCool);
      else if (targetMode == "DRY") acCoolix->setMode(kCoolixDry);
      else if (targetMode == "AUTO") acCoolix->setMode(kCoolixAuto);
      else if (targetMode == "FAN") acCoolix->setMode(kCoolixFan);
      else if (targetMode == "HEAT") acCoolix->setMode(kCoolixHeat);
      // Fan
      if (targetFan == "LOW") acCoolix->setFan(kCoolixFanMin);
      else if (targetFan == "MEDIUM") acCoolix->setFan(kCoolixFanMed);
      else if (targetFan == "HIGH") acCoolix->setFan(kCoolixFanMax);
      else if (targetFan == "AUTO") acCoolix->setFan(kCoolixFanAuto);
      // Swing Coolix
      // Tidak mengirim toggle otomatis karena akan mematikan/menyalakan swing tiap ganti suhu
      // Pengguna harus mengontrol swing secara terpisah jika menggunakan Coolix
      if (flagUbahSwing) {
        acCoolix->setSwing();
        flagUbahSwing = false;
      }
    } else {
      acCoolix->off();
    }
    acCoolix->send();
  } 
  else if (savedBrand == "LG" && acLg != NULL) {
    if (acPowerState) {
      acLg->on();
      acLg->setTemp(targetSuhu);
      // Mode
      if (targetMode == "COOL") acLg->setMode(kLgAcCool);
      else if (targetMode == "DRY") acLg->setMode(kLgAcDry);
      else if (targetMode == "AUTO") acLg->setMode(kLgAcAuto);
      else if (targetMode == "FAN") acLg->setMode(kLgAcFan);
      else if (targetMode == "HEAT") acLg->setMode(kLgAcHeat);
      // Fan
      if (targetFan == "LOW") acLg->setFan(modeFuzzy ? kLgAcFanLowest : kLgAcFanLow);
      else if (targetFan == "MEDIUM") acLg->setFan(kLgAcFanMedium);
      else if (targetFan == "HIGH") acLg->setFan(kLgAcFanMax);
      else if (targetFan == "AUTO") acLg->setFan(kLgAcFanAuto);
      // Swing
      if (targetSwing == "AUTO") acLg->setSwingV(kLgAcSwingVAuto);
      else if (targetSwing == "OFF") acLg->setSwingV(kLgAcSwingVOff);
      else if (targetSwing == "0") acLg->setSwingV(kLgAcSwingVHighest);
      else if (targetSwing == "1") acLg->setSwingV(kLgAcSwingVHigh);
      else if (targetSwing == "2") acLg->setSwingV(kLgAcSwingVMiddle);
      else if (targetSwing == "3") acLg->setSwingV(kLgAcSwingVLow);
      else if (targetSwing == "4") acLg->setSwingV(kLgAcSwingVLowest);
    } else {
      acLg->off();
    }
    acLg->send();
  }
  else if (acPanasonic != NULL) {
    if (acPowerState) {
      acPanasonic->on(); 
      acPanasonic->setTemp(targetSuhu);
      // Mode
      if (targetMode == "COOL") acPanasonic->setMode(kPanasonicAcCool);
      else if (targetMode == "DRY") acPanasonic->setMode(kPanasonicAcDry);
      else if (targetMode == "AUTO") acPanasonic->setMode(kPanasonicAcAuto);
      else if (targetMode == "FAN") acPanasonic->setMode(kPanasonicAcFan);
      else if (targetMode == "HEAT") acPanasonic->setMode(kPanasonicAcHeat);
      // Fan
      if (targetFan == "LOW") acPanasonic->setFan(kPanasonicAcFanMin);
      else if (targetFan == "MEDIUM") acPanasonic->setFan(kPanasonicAcFanMed);
      else if (targetFan == "HIGH") acPanasonic->setFan(kPanasonicAcFanMax);
      else if (targetFan == "AUTO") acPanasonic->setFan(kPanasonicAcFanAuto);
      // Swing
      if (targetSwing == "AUTO") acPanasonic->setSwingVertical(kPanasonicAcSwingVAuto);
      else if (targetSwing == "OFF") acPanasonic->setSwingVertical(kPanasonicAcSwingVHighest);
      else if (targetSwing == "0") acPanasonic->setSwingVertical(kPanasonicAcSwingVHighest);
      else if (targetSwing == "1") acPanasonic->setSwingVertical(kPanasonicAcSwingVHigh);
      else if (targetSwing == "2") acPanasonic->setSwingVertical(kPanasonicAcSwingVMiddle);
      else if (targetSwing == "3") acPanasonic->setSwingVertical(kPanasonicAcSwingVLow);
      else if (targetSwing == "4") acPanasonic->setSwingVertical(kPanasonicAcSwingVLowest);
    } else {
      acPanasonic->off();
    }
    acPanasonic->send();
  }
}
void updateDisplay() {
  static uint32_t displayedAt = 0;
  static bool displayed = false;
  if (displayed && !displayDirty && uint32_t(millis()-displayedAt) < 100) return;
  displayedAt = millis(); displayed = true;
  displayDirty = false;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  // Top-left source label. Temperature + RH move to the second row.
  if (activeReading.fallback) display.print("SRC:REMOTE*");
  else display.print(activeReading.selectedNode ? "SRC:NODE" : "SRC:REMOTE");
  display.setCursor(84, 0);
  if (acPowerState) { display.print("AC:"); display.print(targetSuhu); display.print("C"); }
  else display.print("AC:OFF");
  display.setCursor(0, 12);
  display.print("T:");
  if (activeReading.reading.valid) display.print(activeReading.reading.temperature, 1);
  else display.print("--");
  display.print("C");
  display.setCursor(76, 12);
  display.print("RH:");
  if (activeReading.reading.valid) display.print(activeReading.reading.humidity, 0);
  else display.print("--");
  display.print("%");
  display.setCursor(0, 24);
  display.print("Org: ");
  if (occupancyKnown) display.print(jumlahOrang);
  else display.print("--");
  display.setCursor(68, 24);
  if (activeReading.issue == aacs::Issue::PAIR_REQUIRED) display.print("PAIR NODE");
  else if (activeReading.issue == aacs::Issue::WAIT_NODE) display.print("WAIT NODE");
  else if (activeReading.issue == aacs::Issue::NODE_STALE) display.print("NODE OLD");
  else if (!activeReading.reading.valid) display.print("NO DATA");
  else display.print(modeFuzzy ? "FUZZY" : "MANUAL");
  display.display();
}
// ============================================================
// MESIN FUZZY LOGIC MAMDANI (TRANSLASI DARI PYTHON SCIKIT-FUZZY)
// ============================================================
