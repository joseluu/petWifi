//----------------------------------------------------------------------------------------------
// BoardBoard Library : esp32 by Espressif Systems 3.2.0
// Board Select       : XIAO_ESP32S3
//----------------------------------------------------------------------------------------------
// [NOTE] When upload, BOOT+RESET:ON, RESET:OFF, then BOOT:OFF


// Transmit-only protocol (no station handshake):
// 1. scan Wi-Fi passive on 2.4 GHz channels
// 2. transmit CatPacket (UID, packetNumber, Vbatt, rssi, snr, interval, apCount, apList[5])
// 3. sleep until next CAT_TX_INTERVAL (60 s)

// 2025/04/04

#include <RadioLib.h>           // RadioLib by Jan Gromes 7.1.2
#include <Arduino.h>
#include "esp_idf_version.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_sleep.h"

#ifndef ESP_IDF_VERSION_VAL
#define ESP_IDF_VERSION_VAL(major, minor, patch)  ((major)*1000 + (minor)*100 + (patch))
#endif
#ifndef ESP_IDF_VERSION
#define ESP_IDF_VERSION  ESP_IDF_VERSION_VAL(ESP_IDF_VERSION_MAJOR, \
                                             ESP_IDF_VERSION_MINOR, \
                                             ESP_IDF_VERSION_PATCH)
#endif
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
    // You are on ESP-IDF v5.5 or newer
    #define USING_ESP_IDF_V5_5_OR_LATER
#endif
                                        
static uint16_t ap_count = 0;
#define MAX_AP_RECORDS 10
wifi_ap_record_t ap_records[MAX_AP_RECORDS];


#include "../Lora_station_251001-113904-seeed_xiao_esp32s3/include/packet.h"

#define CHECK_INTERVAL     10000   // legacy communication check interval [mS] (unused in transmit-only mode)
#define CAT_TX_INTERVAL    60000   // cat transmits every 60 s
#define TX_TIMEOUT         15000   // upper bound on CatPacket airtime at SF12/BW62.5/CR4/8 (~10 s)
#define RF_SW           D5      // RF Switch

#define ESP32_S3_MOSI_PIN 9
#define ESP32_S3_MISO_PIN 8
#define ESP32_S3_SCK_PIN 7
#define ESP32_S3_NSS_PIN 41
#define ESP32_S3_RST_PIN 42
#define ESP32_S3_BUSY_PIN 40
#define ESP32_S3_ANTENA_SW_PIN 38

// ✅ Updated Pin Configuration (B2B Connector)
#define LORA_NSS 41    // ✅ SPI Chip Select (GPIO41)
#define LORA_SCK 7     // ✅ SPI Clock (GPIO7)
#define LORA_MOSI 9    // ✅ SPI MOSI (GPIO9)
#define LORA_MISO 8    // ✅ SPI MISO (GPIO8)
#define LORA_RST 42    // ✅ LoRa Reset (GPIO42)
#define LORA_BUSY 40   // ✅ LoRa BUSY (GPIO40)
#define LORA_DIO1 39   // ✅ LoRa IRQ (DIO1 - GPIO39)
#define LORA_ANT_SW 38 // ✅ Antenna Switch (GPIO38)

// ✅ LoRa Configuration
#define LORA_FREQUENCY 869.52
#define LORA_BANDWIDTH 62.5
#define LORA_SPREADING_FACTOR 12  
#define LORA_CODING_RATE 8
#define LORA_TX_POWER 22    
#define LORA_PREAMBLE_LEN 48
// bit time = (2^spreadingFactor)/bandwidth 4096/62.5 = 65.6 [ms]   
// message duration = (preamble + payload + 4.25 )*8*FEC factor*bit time
// for SF12 BW62.5 CR 8, 11 bytes payload => about 11 544 [ms]
// idem for SF12 BW250 CR 8 => about 2 914 [ms]

// Radio lib Modele default setting : SPI:2MHz, MSBFIRST, MODE0
SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);

#define dataNum   11                // transmit data 11 bytes (SlaveUID 4, randomNumber 4, Vbatt 2, (previous)veryfiResult 1)
                                    // receive data 11 bytes  (MasterUID 4, verifyNumber 4, rssi 1, snr 1, interval 1)
union unionBuff {                   // buffer for data type conversion
  uint32_t  Buff_u32[dataNum/4];    // stationUID, catUID, randomNumber, verifyNumber
  uint16_t  Buff_u16[dataNum/2];    // Vbatt
  int8_t    Buff_i8[dataNum];       // rssi, snr
  uint8_t   Buff_u8[dataNum];       // verifyResult, interval
};
union unionBuff ub;

int8_t rssi;                        // signal RSSI [dBm]
int8_t snr;                         // signal SN ratio [dB]
uint32_t senderuid;                 // sender device UID
uint16_t Vbatt;                     // battery voltage data [mV]
char printBuff[4];                  // for sprintf()
String  txdata = "";                // transmission data packet string
String  rxdata = "";                // received data packet string
bool operationDone = false;         // receive or transmit operation done
uint16_t receivedInterval;           // Station received interval [sec]
uint32_t randomNumber;              // random number to verify that data was sent correctly
uint32_t verifyNumber;              // verify number from Station
uint32_t verifyResult = 0;          // verify result 1:GOOD or 0:ERROR
int state;                          // state of radio module
uint32_t timeoutCheck;              // for timeout check

uint32_t receivedTime;              // packet received time [mS]
uint32_t timestamp;                 // previouse received time [mS]

// *******************************************************************************************************
void setup() {
  Serial.begin(115200);

  // Station UID
  Serial.print("Station UID "); Serial.println(STATION_UID, HEX);

  // Pin initialization
  pinMode(RF_SW, OUTPUT);               // RF Switch
  pinMode(LED_BUILTIN, OUTPUT);         // built-in LED
  digitalWrite(RF_SW, HIGH);            // internal DIO2 controls Tx/Rx
  digitalWrite(LED_BUILTIN, HIGH);      // LED off  


  // Radio initialization, depend on the erea rules
  // int16_t begin(float freq = 434.0, float bw = 125.0, uint8_t sf = 9, uint8_t cr = 7, \
  // uint8_t syncWord = RADIOLIB_SX126X_SYNC_WORD_PRIVATE, int8_t power = 10, \
  // uint16_t preambleLength = 8, float tcxoVoltage = 1.6, bool useRegulatorLDO = false);
  // [NOTE] to be set up in compliance with area rules
  state = radio.begin(LORA_FREQUENCY, LORA_BANDWIDTH, LORA_SPREADING_FACTOR, LORA_CODING_RATE, 
                          0x12, LORA_TX_POWER, LORA_PREAMBLE_LEN, 1.6, false);
  radio.setCRC(true);                // Enable CRC
  radio.explicitHeader();            // Explicit mode
  radio.forceLDRO(true);             // For SF12

  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("radio.begin() success!");
  }
  else {
    Serial.print("failed, code ");
    Serial.println(state);
    errorBlink(4);                // error [[ 4 ]]
  }

  // callback function when receive or transmit operation done
  radio.setDio1Action(setFlag);

    // Initial Wi-Fi setup
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());

}
// ***********************************************************************************************************
// Channel scan logic
#define CHANNEL_LIST_SIZE 11
// most common channels first
static uint8_t channel_list[CHANNEL_LIST_SIZE] = {1, 6, 11, 4, 5, 2, 7, 8, 9, 10, 3};


#ifdef USING_ESP_IDF_V5_5_OR_LATER
// only available in esp-idl 5.5, does not work
// usage: array_2_channel_bitmap(channel_list, CHANNEL_LIST_SIZE, &config);
static void array_2_channel_bitmap(const uint8_t channel_list[], const uint8_t channel_list_size, wifi_scan_config_t *scan_config) {

    for(uint8_t i = 0; i < channel_list_size; i++) {
        uint8_t channel = channel_list[i];
        scan_config->channel_bitmap.ghz_2_channels |= (1 << channel);
    }
}


void start_passive_scan_multi_channel() {
  wifi_scan_config_t config = {};
  bool blocking = true;
  config.ssid = nullptr;
  config.bssid = nullptr;
  config.channel = 0;           // 0 = all channels or channel_bitmap
  config.show_hidden = true;    // also detect hidden SSIDs
  config.scan_type = WIFI_SCAN_TYPE_PASSIVE;
  array_2_channel_bitmap(channel_list, CHANNEL_LIST_SIZE, &config);
  Serial.printf("Starting passive scan multi channel");

  ESP_ERROR_CHECK(esp_wifi_scan_start(&config, blocking));

  // Wait for scan to complete
  if (blocking) {
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    if (ap_count > 0) {
      ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_records));
    }
  }
}
#endif

static void start_passive_scan_single_channel(int channel) {
  wifi_scan_config_t config = {};
  bool blocking = true;
  config.ssid = nullptr;
  config.bssid = nullptr;
  config.channel = channel;           // 0 = all channels or channel_bitmap
  config.show_hidden = true;    // also detect hidden SSIDs
  config.scan_type = WIFI_SCAN_TYPE_PASSIVE;

  ESP_ERROR_CHECK(esp_wifi_scan_start(&config, blocking));
}

void start_passive_scan_enumerate_channels() {
  ap_count = 0;
  Serial.printf("Starting passive scan on channel ");
  for (int ch = 1; ch <= CHANNEL_LIST_SIZE; ch++) {
    Serial.printf("%d ", channel_list[ch - 1]);
    start_passive_scan_single_channel(channel_list[ch - 1]);
    // TODO: collect APs after each channel scan
    if (ap_count < MAX_AP_RECORDS) {
      uint16_t found;
      esp_err_t ret = esp_wifi_scan_get_ap_num(&found);
      if (ret == ESP_OK && found > 0) {
        uint16_t to_copy = min(found, (uint16_t)(MAX_AP_RECORDS - ap_count));
        //TODO: sorted by descending order of RSSI, we should take only the first ones
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&to_copy, &ap_records[ap_count]));
        ap_count += to_copy;
      }
    }
    delay(100);  // small delay between channel scans
  }
  Serial.printf("\n");
}

void print_scan_results() {
  Serial.printf("\nScan complete! Found %d networks:\n", ap_count);
  Serial.println("BSSID               RSSI  Ch  Encryption      SSID");
  Serial.println("-------------------------------------------------------------");

  for (uint16_t i = 0; i < ap_count; i++) {
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             ap_records[i].bssid[0], ap_records[i].bssid[1], ap_records[i].bssid[2],
             ap_records[i].bssid[3], ap_records[i].bssid[4], ap_records[i].bssid[5]);

    const char* auth = "OPEN";
    switch (ap_records[i].authmode) {
      case WIFI_AUTH_OPEN:            auth = "OPEN";      break;
      case WIFI_AUTH_WEP:             auth = "WEP";       break;
      case WIFI_AUTH_WPA_PSK:         auth = "WPA";       break;
      case WIFI_AUTH_WPA2_PSK:        auth = "WPA2";      break;
      case WIFI_AUTH_WPA_WPA2_PSK:    auth = "WPA/WPA2";  break;
      case WIFI_AUTH_WPA3_PSK:        auth = "WPA3";      break;
      case WIFI_AUTH_WPA2_WPA3_PSK:   auth = "WPA2+WPA3"; break;
      default:                        auth = "UNKNOWN";   break;
    }

    char ssid[33] = {0};
    strncpy(ssid, (char*)ap_records[i].ssid, 32);

    Serial.printf("%s  %3d  %2d   %-12s  %s\n",
                  mac, ap_records[i].rssi, ap_records[i].primary, auth, ssid);
  }
  Serial.println();
}

// Sort scan results in-place by RSSI descending (strongest first)
void sort_scan_results_by_rssi(wifi_ap_record_t records[], uint16_t count) {
  uint16_t n = min(count, (uint16_t)MAX_AP_RECORDS);
  if (n < 2) return;

  // Simple selection sort (small arrays -> fine and predictable)
  for (uint16_t i = 0; i < n - 1; ++i) {
    uint16_t best = i;
    for (uint16_t j = i + 1; j < n; ++j) {
      // higher rssi is better (e.g. -30 > -80)
      if (records[j].rssi > records[best].rssi) {
        best = j;
      }
    }
    if (best != i) {
      wifi_ap_record_t tmp = records[i];
      records[i] = records[best];
      records[best] = tmp;
    }
  }
}

// ***********************************************************************************************************
// Transmit-only protocol: scan Wi-Fi, transmit CatPacket, sleep until next CAT_TX_INTERVAL.
// No handshake with station.
CatPacket catPacket;
void loop()
{
  static uint8_t catPacketNumber = 0;
  uint32_t cycleStart = millis();
  uint32_t txtime;
  uint32_t txDuration;

  Serial.println("***cat** Scanning Wi-Fi ********");

  /******** Scanning channels ********/
#ifdef USING_ESP_IDF_V5_5_OR_LATER
  start_passive_scan_multi_channel();
#else
  start_passive_scan_enumerate_channels();
#endif
  sort_scan_results_by_rssi(ap_records, ap_count);
  print_scan_results();

  Serial.println("******** Transmitting cat packet ********");

  Vbatt = 3999;  // sample [mV] — no battery divider wired yet
  catPacketNumber++;

  catPacket.fields.UID = CAT_UID;
  catPacket.fields.packetNumber = catPacketNumber;
  catPacket.fields.vbatt = Vbatt;
  catPacket.fields.rssi = 0;                          // no incoming packet to measure
  catPacket.fields.snr = 0;
  catPacket.fields.interval = CAT_TX_INTERVAL / 1000; // seconds
  catPacket.fields.apCount = std::min((int)ap_count, MAX_APS_IN_PACKET);
  for (uint8_t i = 0; i < ap_count && i < MAX_APS_IN_PACKET; i++) {
    catPacket.fields.apList[i] = AccessPoint(ap_records[i].bssid, ap_records[i].rssi, ap_records[i].primary);
  }

  digitalWrite(LED_BUILTIN, LOW);
  txtime = millis();
  operationDone = false;
  state = radio.startTransmit(catPacket.bytes, sizeof(catPacket.bytes));

  timeoutCheck = millis();
  while(!operationDone && ((millis() - timeoutCheck) < TX_TIMEOUT)) {
    delay(1);
  }
  txDuration = millis() - txtime;
  digitalWrite(LED_BUILTIN, HIGH);

  Serial.printf("Cat UID:\t%08x\n", catPacket.fields.UID);
  Serial.print("Packet Number:\t"); Serial.println(catPacket.fields.packetNumber);
  Serial.print("AP Count:\t"); Serial.println(catPacket.fields.apCount);
  Serial.print("Transmission time [ms]: "); Serial.println(txDuration);

  txdata = "";
  for(int i = 0; i < sizeof(catPacket.bytes); i++) {
    sprintf(printBuff, "%02x", catPacket.bytes[i]);
    txdata += printBuff;
  }
  txdata.toUpperCase();
  Serial.println(txdata);

  if (state == RADIOLIB_ERR_NONE && operationDone) {
    Serial.println("transmission done!");
  } else {
    Serial.print("transmit failed or timed out, code ");
    Serial.println(state);
  }
  Serial.println();

  // light sleep the remainder of the 60 s cycle
  uint32_t elapsed = millis() - cycleStart;
  if (elapsed < CAT_TX_INTERVAL) {
    uint32_t remaining = CAT_TX_INTERVAL - elapsed;
    Serial.printf("light sleep %lu ms until next cycle\n", (unsigned long)remaining);
    Serial.flush();
    esp_sleep_enable_timer_wakeup((uint64_t)remaining * 1000ULL);
    esp_light_sleep_start();
  }
}

//****************************************************************************************
// this function is called when a complete packet is received or transmitted
void setFlag(void) {
  operationDone = true;
}

//****************************************************************************************
// Blink specified number of times on error
void errorBlink(uint8_t err)
{
  while(true) {
    for(int i = 0; i < err; i++) {
      digitalWrite(LED_BUILTIN, LOW);
      delay(200);
      digitalWrite(LED_BUILTIN, HIGH);
      delay(200);
    }
    delay(500);
  }
}

void errorBlink_1(uint8_t err)
{
  for(int i = 0; i < err; i++) {
    digitalWrite(LED_BUILTIN, LOW);
    delay(200);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(200);
  }
  delay(500);
}