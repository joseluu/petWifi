// ESP32 Low-Power Active WiFi Scan
// 360 ms active scan per channel, full scan every 30 seconds
// Wi-Fi fully deinitialized between scans

#include <Arduino.h>
#include "esp_idf_version.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"

#define ESP_IDF_VERSION_VAL(major, minor, patch)  ((major)*1000 + (minor)*100 + (patch))
#define ESP_IDF_VERSION  ESP_IDF_VERSION_VAL(ESP_IDF_VERSION_MAJOR, \
                                             ESP_IDF_VERSION_MINOR, \
                                             ESP_IDF_VERSION_PATCH)
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
    // You are on ESP-IDF v5.5 or newer
    #define USING_ESP_IDF_V5_5_OR_LATER
#endif
                                        
static uint16_t ap_count = 0;
#define MAX_AP_RECORDS 10
wifi_ap_record_t ap_records[MAX_AP_RECORDS];

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

void start_passive_scan(int channel) {
  wifi_scan_config_t config = {};
  bool blocking = true;
  config.ssid = nullptr;
  config.bssid = nullptr;
  config.channel = channel;           // 0 = all channels or channel_bitmap
  config.show_hidden = true;    // also detect hidden SSIDs
  config.scan_type = WIFI_SCAN_TYPE_PASSIVE;

  Serial.printf("Starting passive scan on channel %d...\n", channel);

  ESP_ERROR_CHECK(esp_wifi_scan_start(&config, blocking));
}

void start_passive_scan_all_channels() {
  ap_count = 0;
  for (int ch = 1; ch <= CHANNEL_LIST_SIZE; ch++) {
    start_passive_scan(channel_list[ch - 1]);
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
}

void setup() {
  Serial.begin(115200);
  delay(5000);
  Serial.println("\nESP32 Low-Power Active Scanner");
  Serial.println("Full scan every 30 seconds, Wi-Fi OFF in between\n");

  // Initial Wi-Fi setup
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());

  // Start first scan
  start_passive_scan_all_channels();
}

void loop() {
  static uint32_t last_scan_time = 0;
  static bool scanning = true;

  if (scanning) {

      print_scan_results();
      // Fully power down Wi-Fi
      esp_wifi_stop();
      esp_wifi_deinit();
      Serial.println("Wi-Fi completely powered OFF (deep sleep level savings)\n");
      scanning = false;
      last_scan_time = millis();
    }
    // else: still scanning or no APs → keep waiting
    else {
    // Wait until 30 seconds from last scan start
    if (millis() - last_scan_time >= 30000) {
      // Re-init Wi-Fi and start next scan
      wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
      ESP_ERROR_CHECK(esp_wifi_init(&cfg));
      ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
      ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
      ESP_ERROR_CHECK(esp_wifi_start());

#ifdef USING_ESP_IDF_V5_5_OR_LATER
      start_passive_scan_multi_channel();
#else
      start_passive_scan_all_channels();
#endif
      scanning = true;
    } else {
      delay(100);  // Small delay to avoid watchdog issues
    }
  }

  // Small delay to avoid watchdog issues
  delay(100);
}