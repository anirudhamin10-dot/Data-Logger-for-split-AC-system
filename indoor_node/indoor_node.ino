// =============================================================
//   INDOOR NODE (ESP32)
//   Reads 2x DHT22 (supply air, return air) and sends them to the
//   outdoor logger over ESP-NOW every 2.5 s.
//
// Libraries: DHT sensor library (Adafruit) + Adafruit Unified Sensor
// =============================================================
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <DHT.h>
#include <math.h>

// ---------------------------------------------------------
// Pins
// ---------------------------------------------------------
#define DHT_SUPPLY_PIN 16
#define DHT_RETURN_PIN 17
#define LED_PIN        2        // onboard LED, blinks on every send

// ---------------------------------------------------------
// Settings
// ---------------------------------------------------------
#define SEND_PERIOD_MS 2500     // DHT22 needs >= 2 s between reads
#define ESPNOW_CHANNEL 1        // MUST match the outdoor logger

// MAC address of the OUTDOOR ESP32 
uint8_t peerMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---------------------------------------------------------
// Packet (identical definition on the outdoor logger)
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
  uint8_t  crc;               // CRC-8 (poly 0x07) over all previous bytes
} IndoorPacket;

static uint8_t crc8(const uint8_t *d, size_t n) {
  uint8_t c = 0;
  while (n--) {
    c ^= *d++;
    for (int i = 0; i < 8; i++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
  }
  return c;
}

DHT dhtSupply(DHT_SUPPLY_PIN, DHT22);
DHT dhtReturn(DHT_RETURN_PIN, DHT22);
uint16_t seq = 0;

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  btStop();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  Serial.print("Indoor node MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed, restarting");
    delay(1000);
    ESP.restart();
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, peerMac, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Failed to add peer");
  }

  dhtSupply.begin();
  dhtReturn.begin();
  delay(2000);                    // let the DHT22s settle
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last < SEND_PERIOD_MS) return;
  last = millis();

  IndoorPacket p;
  p.magic   = PKT_MAGIC;
  p.version = PKT_VERSION;
  p.seq     = seq++;
  p.tSupply = dhtSupply.readTemperature();
  p.hSupply = dhtSupply.readHumidity();
  p.tReturn = dhtReturn.readTemperature();
  p.hReturn = dhtReturn.readHumidity();   // failed reads stay NaN
  p.crc     = crc8((const uint8_t *)&p, sizeof(p) - 1);

  esp_err_t r = esp_now_send(peerMac, (const uint8_t *)&p, sizeof(p));

  digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  Serial.printf("#%u  Sup %.1fC %.0f%%  Ret %.1fC %.0f%%  send=%s\n",
                p.seq, p.tSupply, p.hSupply, p.tReturn, p.hReturn,
                r == ESP_OK ? "ok" : "ERR");
}
