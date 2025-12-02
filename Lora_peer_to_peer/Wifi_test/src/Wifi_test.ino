#include "WiFi.h"
#include "esp_arduino_version.h"

void setup()
{
    Serial.begin(115200);
    delay(5000);
    Serial.println("scanner starting");
    Serial.printf("Arduino-ESP32 core   : %d.%d.%d\n",
            ESP_ARDUINO_VERSION_MAJOR,
            ESP_ARDUINO_VERSION_MINOR,
            ESP_ARDUINO_VERSION_PATCH);

  // This is the line you want:
  Serial.printf("Based on ESP-IDF     : %s\n", ESP.getSdkVersion());
    // Set WiFi to station mode and disconnect from an AP if it was previously connected
    //WiFi.mode(WIFI_STA);
    //WiFi.disconnect();
    delay(100);
}
/*
6 networks found
9c:9d:7e:5b:13:17       1       -38     freeboite2
58:b5:68:22:1e:2a       1       -79     FK7RGOI2y5GglqIcLW4S39Lp6JKtgU1R
40:31:3c:1d:2d:71       7       -82     freeboite2
18:62:2c:b5:c3:4b       6       -86     Livebox-C34B
a8:a2:37:5a:95:f6       11      -86     Livebox-95F6
2e:f4:32:17:dc:3e       4       -90     IHS STOVE_DC3E
*/

void loop()
{
    // WiFi.scanNetworks will return the number of networks found
    //MatchState ms;
    int n = WiFi.scanNetworks();
    unsigned char bssid[n][6];
    String ssid[n];
    String rssi[n];
    if (n == 0) {
        Serial.println("no networks found");
    } else {
        Serial.print(n);
        Serial.println(" networks found");
        for (int i = 0; i < n; ++i) {
            // Print SSID and RSSI for each network found
            for (int j=0; j<6; j++){
                bssid[i][j] = WiFi.BSSID(i)[j];
            }
            ssid[i] = WiFi.SSID(i);
            rssi[i] = WiFi.RSSI(i);
        }
    }

     for (int i = 0; i < n; ++i) {
            String ssid2 = ssid[i];
            String rssi2 = rssi[i];
            String channel2 = String(WiFi.channel(i));
            char bssid_c[30];
            snprintf(bssid_c, 20, "%2x:%2x:%2x:%2x:%2x:%2x\t", bssid[i][0],bssid[i][1],bssid[i][2],bssid[i][3],bssid[i][4],bssid[i][5]);
            Serial.print(bssid_c);
            Serial.print(channel2 + '\t');
            Serial.print(rssi2 + '\t');
            ssid2.replace("unifi", "u"); //I customize so that it the display format is in single line
            Serial.println(ssid2);
            delay(100);
        }
}