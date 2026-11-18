// =============================================================
// OUTDOOR LOGGER (ESP32)  -  replaces compressor_logger.ino
//   - STM32 current (RMS + peak) over UART
//   - 3x DS18B20 (liquid, suction, ambient)
//   - MPU6050 compressor vibration
//   - Supply/return air temp + RH received from the indoor node (ESP-NOW)
//   - 0.91" SSD1306 OLED, CSV logging to SD card (SPI)
//
// Libraries: Adafruit GFX, Adafruit SSD1306, OneWire, DallasTemperature
// =============================================================
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>

// ---------------------------------------------------------
// Pins
// ---------------------------------------------------------
#define I2C_SDA_PIN   27      // OLED + MPU6050
#define I2C_SCL_PIN   26
#define RX_PIN        33      // from STM32 USART1 TX
#define TX_PIN        32
#define SD_SCK_PIN    18
#define SD_MISO_PIN   19
#define SD_MOSI_PIN   23
#define SD_CS_PIN     5
#define ONEWIRE_PIN   4       // DS18B20 data, 4.7k pull-up to 3V3

// ---------------------------------------------------------
// Settings
// ---------------------------------------------------------
#define LOG_INTERVAL_MS    1000
#define SD_FLUSH_MS        2000
#define PAGE_TIME_MS       3000
#define DISPLAY_REFRESH_MS 500
#define DS_PERIOD_MS       1000
#define DS_CONVERT_MS      800
#define VIB_WINDOW_MS      1000
#define MPU_ADDR           0x68
#define MPU_LSB_PER_G      8192.0   // +/-4 g

#define PACKET_LEN         8
#define PACKET_GAP_MS      10

#define ESPNOW_CHANNEL     1        // MUST match the indoor node
#define LINK_TIMEOUT_MS    10000    // no packet for this long => indoor link down

// Optional: fix each DS18B20 to a role by ROM address (see Serial at boot).
DeviceAddress dsAddr[3] = {
  {0, 0, 0, 0, 0, 0, 0, 0},   // liquid
  {0, 0, 0, 0, 0, 0, 0, 0},   // suction
  {0, 0, 0, 0, 0, 0, 0, 0}    // ambient
};

// ---------------------------------------------------------
// ESP-NOW packet (identical definition on the indoor node)
// ---------------------------------------------------------
#define PKT_MAGIC   0xA5
#define PKT_VERSION 1

typedef struct __attribute__((packed)) {
  uint8_t  magic;
  uint8_t  version;
  uint16_t seq;
  float    tSupply;
  float    hSupply;
  float    tReturn;
  float    hReturn;
  uint8_t  crc;
} IndoorPacket;

static uint8_t crc8(const uint8_t *d, size_t n) {
  uint8_t c = 0;
  while (n--) {
    c ^= *d++;
    for (int i = 0; i < 8; i++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
  }
  return c;
}

// ---------------------------------------------------------
// Objects
// ---------------------------------------------------------
Adafruit_SSD1306 display(128, 32, &Wire, -1);
OneWire oneWire(ONEWIRE_PIN);
DallasTemperature ds(&oneWire);
SPIClass sdSPI(VSPI);            // use FSPI on ESP32-S3/C3
File logFile;

// ---------------------------------------------------------
// State
// ---------------------------------------------------------
bool sdOk = false, mpuOk = false;

uint32_t seq = 0;
uint32_t rms_mA = 0, peak_mA = 0;
uint64_t rmsSum = 0; uint32_t rmsCount = 0; uint32_t peakMax = 0;

float tLiquid = NAN, tSuction = NAN, tAmbient = NAN;
float tSupply = NAN, hSupply = NAN, tReturn = NAN, hReturn = NAN;
float vib_mg = NAN;

// ESP-NOW receive state (written by the WiFi task, read by loop)
static portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;
static IndoorPacket rxPkt;
static volatile bool     rxNew = false;
static volatile uint32_t rxMs = 0;       // millis() of last good packet
static volatile uint32_t rxBad = 0;
bool     linkOk = false;
uint32_t indoorAgeMs = 0;
bool     everReceived = false;

uint32_t lastLog = 0, lastFlush = 0, lastDisp = 0;

// ---------------------------------------------------------
// ESP-NOW receive
// ---------------------------------------------------------
static void handleRx(const uint8_t *data, int len) {
  if (len != (int)sizeof(IndoorPacket)) { rxBad++; return; }
  IndoorPacket p;
  memcpy(&p, data, sizeof(p));
  if (p.magic != PKT_MAGIC || p.version != PKT_VERSION ||
      crc8(data, sizeof(p) - 1) != p.crc) { rxBad++; return; }

  portENTER_CRITICAL(&rxMux);
  rxPkt = p;
  rxNew = true;
  rxMs = millis();
  portEXIT_CRITICAL(&rxMux);
}

// Callback signature changed in ESP32 Arduino core 3.x
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  handleRx(data, len);
}
#else
void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
  handleRx(data, len);
}
#endif

void initEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);                       // keep the radio listening
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  Serial.print("Outdoor logger MAC: ");
  Serial.println(WiFi.macAddress());                   // paste into indoor_node peerMac
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_recv_cb(onEspNowRecv);
}

void updateIndoor() {
  IndoorPacket p = {};
  bool fresh = false;
  uint32_t ms;

  portENTER_CRITICAL(&rxMux);
  if (rxNew) { p = rxPkt; rxNew = false; fresh = true; }
  ms = rxMs;
  portEXIT_CRITICAL(&rxMux);

  if (fresh) {
    tSupply = p.tSupply; hSupply = p.hSupply;
    tReturn = p.tReturn; hReturn = p.hReturn;
    everReceived = true;
  }
  uint32_t now = millis();
  indoorAgeMs = ms ? (now - ms) : 0;
  linkOk = (ms != 0) && (indoorAgeMs < LINK_TIMEOUT_MS);
  if (!linkOk) { tSupply = hSupply = tReturn = hReturn = NAN; }
}

// ---------------------------------------------------------
// SD
// ---------------------------------------------------------
bool openLogFile() {
  char name[24];
  for (int i = 0; i < 10000; i++) {
    snprintf(name, sizeof(name), "/log_%04d.csv", i);
    if (!SD.exists(name)) break;
  }
  logFile = SD.open(name, FILE_WRITE);
  if (!logFile) return false;
  logFile.println("ms,seq,rms_mA,peak_mA,vib_mg,liquid_C,suction_C,ambient_C,"
                  "supply_C,supply_RH,return_C,return_RH,indoor_age_s");
  logFile.flush();
  Serial.printf("Logging to %s\n", name);
  return true;
}

static void fmt(char *out, size_t n, float v, int dec) {
  if (isnan(v)) out[0] = 0; else snprintf(out, n, "%.*f", dec, v);
}

void logRow() {
  uint32_t r = rmsCount ? (uint32_t)(rmsSum / rmsCount) : rms_mA;
  uint32_t p = rmsCount ? peakMax : peak_mA;
  rmsSum = 0; rmsCount = 0; peakMax = 0;
  if (!sdOk) return;

  char vib[12], l[12], s[12], a[12], ts[12], hs[12], tr[12], hr[12], age[12];
  fmt(vib, 12, vib_mg, 1);
  fmt(l, 12, tLiquid, 2);  fmt(s, 12, tSuction, 2);  fmt(a, 12, tAmbient, 2);
  fmt(ts, 12, tSupply, 1); fmt(hs, 12, hSupply, 1);
  fmt(tr, 12, tReturn, 1); fmt(hr, 12, hReturn, 1);
  fmt(age, 12, everReceived ? indoorAgeMs / 1000.0f : NAN, 1);

  char row[220];
  snprintf(row, sizeof(row), "%lu,%lu,%lu,%lu,%s,%s,%s,%s,%s,%s,%s,%s,%s",
           (unsigned long)millis(), (unsigned long)seq,
           (unsigned long)r, (unsigned long)p,
           vib, l, s, a, ts, hs, tr, hr, age);
  logFile.println(row);

  if (millis() - lastFlush >= SD_FLUSH_MS) {
    lastFlush = millis();
    logFile.flush();
  }
}

// ---------------------------------------------------------
// STM32 UART: 8 raw bytes (rms LE, peak LE), resync on idle
// ---------------------------------------------------------
void processSerialData() {
  static int lastN = 0;
  static uint32_t lastChange = 0;
  uint8_t buf[PACKET_LEN];

  while (Serial1.available() >= PACKET_LEN) {
    Serial1.readBytes(buf, PACKET_LEN);
    uint32_t r, p;
    memcpy(&r, &buf[0], 4);
    memcpy(&p, &buf[4], 4);
    if (p < r) { Serial.println("Bad packet (peak < rms), dropped"); continue; }
    rms_mA = r; peak_mA = p;
    rmsSum += r; rmsCount++;
    if (p > peakMax) peakMax = p;
    seq++;
  }

  uint32_t now = millis();
  int n = Serial1.available();
  if (n != lastN) { lastN = n; lastChange = now; }
  else if (n > 0 && now - lastChange > PACKET_GAP_MS) {
    while (Serial1.available()) Serial1.read();
    lastN = 0;
  }
}

// ---------------------------------------------------------
// DS18B20 (non-blocking)
// ---------------------------------------------------------
static float cleanDs(float t) {
  return (t == DEVICE_DISCONNECTED_C || t < -55.0f || t > 125.0f) ? NAN : t;
}

void initDS18B20() {
  ds.begin();
  ds.setResolution(12);
  ds.setWaitForConversion(false);
  int found = ds.getDeviceCount();
  Serial.printf("DS18B20 found: %d (need 3)\n", found);
  for (int i = 0; i < found; i++) {
    DeviceAddress a;
    if (!ds.getAddress(a, i)) continue;
    Serial.printf("  #%d: {", i);
    for (int k = 0; k < 8; k++) Serial.printf("0x%02X%s", a[k], k < 7 ? ", " : "");
    Serial.println("}");
  }
  for (int i = 0; i < 3; i++) {
    if (dsAddr[i][0] == 0 && i < found) ds.getAddress(dsAddr[i], i);
  }
}

void updateDS18B20() {
  static bool pending = false;
  static uint32_t reqMs = 0, lastDs = 0;
  uint32_t now = millis();

  if (!pending && now - lastDs >= DS_PERIOD_MS) {
    ds.requestTemperatures();
    pending = true; reqMs = now;
  } else if (pending && now - reqMs >= DS_CONVERT_MS) {
    pending = false; lastDs = reqMs;
    tLiquid  = dsAddr[0][0] ? cleanDs(ds.getTempC(dsAddr[0])) : NAN;
    tSuction = dsAddr[1][0] ? cleanDs(ds.getTempC(dsAddr[1])) : NAN;
    tAmbient = dsAddr[2][0] ? cleanDs(ds.getTempC(dsAddr[2])) : NAN;
  }
}

// ---------------------------------------------------------
// MPU6050 vibration (RMS of 3-axis variance over 1 s, in mg)
// ---------------------------------------------------------
bool mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg); Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool initMPU() {
  if (!mpuWrite(0x6B, 0x00)) return false;
  mpuWrite(0x1A, 0x01);   // DLPF ~184 Hz
  mpuWrite(0x19, 0x01);   // 500 Hz sample rate
  mpuWrite(0x1C, 0x08);   // +/-4 g
  return true;
}

bool readAccel(int16_t &x, int16_t &y, int16_t &z) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)6) != 6) return false;
  x = (Wire.read() << 8) | Wire.read();
  y = (Wire.read() << 8) | Wire.read();
  z = (Wire.read() << 8) | Wire.read();
  return true;
}

void updateVibration() {
  if (!mpuOk) return;
  static uint32_t lastSample = 0, windowStart = 0;
  static double sx, sy, sz, sxx, syy, szz;
  static int16_t rx, ry, rz;
  static uint32_t n = 0;
  static uint8_t fails = 0;

  uint32_t us = micros();
  if (us - lastSample < 2000) return;
  lastSample = us;

  int16_t x, y, z;
  if (!readAccel(x, y, z)) {
    if (++fails > 50) { vib_mg = NAN; fails = 0; }
    return;
  }
  fails = 0;

  if (n == 0) { rx = x; ry = y; rz = z; windowStart = millis(); }
  double dx = x - rx, dy = y - ry, dz = z - rz;
  sx += dx; sy += dy; sz += dz;
  sxx += dx * dx; syy += dy * dy; szz += dz * dz;
  n++;

  if (millis() - windowStart >= VIB_WINDOW_MS && n > 10) {
    double vx = sxx / n - (sx / n) * (sx / n);
    double vy = syy / n - (sy / n) * (sy / n);
    double vz = szz / n - (sz / n) * (sz / n);
    double v = vx + vy + vz;
    vib_mg = (float)(sqrt(v < 0 ? 0 : v) / MPU_LSB_PER_G * 1000.0);
    sx = sy = sz = sxx = syy = szz = 0; n = 0;
  }
}

// ---------------------------------------------------------
// OLED: 3 pages x 4 lines
// ---------------------------------------------------------
static void pv(float v, int dec) {
  if (isnan(v)) display.print("--"); else display.print(v, dec);
}

void updateDisplay() {
  uint32_t now = millis();
  if (now - lastDisp < DISPLAY_REFRESH_MS) return;
  lastDisp = now;

  uint8_t page = (now / PAGE_TIME_MS) % 3;
  display.clearDisplay();
  display.setCursor(0, 0);

  if (page == 0) {
    display.print("RMS:  "); display.print(rms_mA);  display.print(" mA");
    display.setCursor(0, 8);
    display.print("Peak: "); display.print(peak_mA); display.print(" mA");
    display.setCursor(0, 16);
    display.print("Vib:  "); pv(vib_mg, 0); display.print(" mg");
  } else if (page == 1) {
    display.print("Liq: "); pv(tLiquid, 1);  display.print(" C");
    display.setCursor(0, 8);
    display.print("Suc: "); pv(tSuction, 1); display.print(" C");
    display.setCursor(0, 16);
    display.print("Amb: "); pv(tAmbient, 1); display.print(" C");
  } else if (linkOk) {
    display.print("Sup:"); pv(tSupply, 1); display.print("C "); pv(hSupply, 0); display.print("%");
    display.setCursor(0, 8);
    display.print("Ret:"); pv(tReturn, 1); display.print("C "); pv(hReturn, 0); display.print("%");
    display.setCursor(0, 16);
    display.print("dT:  ");
    pv((isnan(tReturn) || isnan(tSupply)) ? NAN : tReturn - tSupply, 1);
    display.print(" C");
  } else {
    display.print("Indoor unit:");
    display.setCursor(0, 8);
    display.print("NO LINK");
    display.setCursor(0, 16);
    if (everReceived) { display.print("last "); display.print(indoorAgeMs / 1000); display.print(" s ago"); }
    else display.print("never received");
  }

  display.setCursor(0, 24);
  display.print(sdOk ? "SD:LOG #" : "SD:ERR #");
  display.print(seq);
  display.setCursor(110, 24);
  display.print(page + 1); display.print("/3");

  display.display();
}

// ---------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------
void setup() {
  btStop();
  Serial.begin(115200);
  Serial1.setRxBufferSize(1024);
  Serial1.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

  initEspNow();                    // WiFi must stay on for ESP-NOW

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;);
  }
  display.dim(true);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.clearDisplay();
  display.setCursor(0, 0);

  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  sdOk = SD.begin(SD_CS_PIN, sdSPI, 4000000) && openLogFile();
  display.println(sdOk ? "SD OK" : "SD FAIL (no log)");

  mpuOk = initMPU();
  display.println(mpuOk ? "MPU6050 OK" : "MPU6050 FAIL");

  initDS18B20();
  display.print("DS18B20: "); display.println(ds.getDeviceCount());
  display.println(WiFi.macAddress());   // MAC for the indoor node
  display.display();
  delay(2500);

  Serial.println("System Ready.");
}

void loop() {
  processSerialData();
  updateVibration();
  updateDS18B20();
  updateIndoor();
  processSerialData();

  if (millis() - lastLog >= LOG_INTERVAL_MS) {
    lastLog = millis();
    logRow();
  }
  updateDisplay();
}
