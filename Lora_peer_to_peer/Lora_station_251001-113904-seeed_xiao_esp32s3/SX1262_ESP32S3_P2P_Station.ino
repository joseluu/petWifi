//----------------------------------------------------------------------------------------------
// BoardBoard Library : esp32 by Espressif Systems 3.2.0
// Board Select       : XIAO_ESP32S3
//----------------------------------------------------------------------------------------------
// [NOTE] When upload, BOOT+RESET:ON, RESET:OFF, then BOOT:OFF

// Receive-only protocol (no station transmission):
// 1. continuously listen for CatPackets
// 2. on reception, verify UID == CAT_UID and upload to server if BSSID set changed



// 2025/04/04
#include <WiFi.h>
#include <HTTPClient.h>         // << added: HTTP client for upload
#include <RadioLib.h>           // RadioLib by Jan Gromes 7.1.2  
#include <U8g2lib.h>            // U8g2 2.35.30
#include <SPI.h>
#include <SD.h>
#include <esp_crc.h>            // for CRC32 calculation

#include "clib/u8g2.h"          // for charge pump setting: try to fix luminosity problem

#include "packet.h"
#include "network_creds.h"      // WiFi SSID and Password

#define RX_POLL_TIMEOUT 120000 // re-arm receiver if nothing heard for this long [mS]

#define RF_SW          D5       // RF Switch
#define sd_sck         D8       // Arduino SPI library uses VSPI circuit
#define sd_miso        D9
#define sd_mosi       D10
#define sd_ss          D0

// getDeviceUniqueId() to find the UID
#define STATION_UID   0xFE55C37C  // Slave device UID example
#define CAT_UID  0x54705810  // Master device UID example

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

// SSD1306 software I2C library, SCL=D6, SDA=D7, 400kHz
U8G2_SSD1306_128X64_NONAME_F_SW_I2C display(U8G2_R0, /*clock=*/ D6, /*data=*/ D7, /*reset=*/ U8X8_PIN_NONE);
// SD log file
File logFile;
// Radio lib Modele default setting : SPI:2MHz, MSBFIRST, MODE0
SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);

int8_t rssi;                        // signal RSSI [dBm]
int8_t snr;                         // signal SN ratio [dB]
uint32_t senderuid;                 // sender device UID
uint16_t Vbatt;                     // battery voltage station [V]
uint32_t randomNumber;              // random number to verify
uint32_t verifyNumber;              // verify number
uint8_t verifyResult;               // received verify result
char printBuff[500];                // for sprintf() holding URL and data
String  txdata = "";                // transmission data packet string
String  rxdata = "";                // received data packet string
bool operationDone = false;         // receive or transmit operation done
String logFileName = "/LoRaLog.txt"; // Log file name, need '/'
uint32_t previousCatBssidsCRC32;     // previous received Cat BSSIDs CRC32

uint8_t receivedInterval;           // Station received interval [sec]
int state;                          // state of radio module
uint32_t timeoutCheck;              // for timeout check
uint32_t txtime;                    // transmission time [mS]

// ***** utility functions *****
// Blinks specified number of times on error
void errorBlink(uint8_t err)
{  
  while(true) { // forever block
    for(int i = 0; i < err; i++) {
      digitalWrite(LED_BUILTIN, LOW);
      delay(200);
      digitalWrite(LED_BUILTIN, HIGH);
      delay(200);
    }
  delay(1000);
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
// Print contents of a received CatPacket to Serial
void printCatPacket(const CatPacket &packet) {
  Serial.printf("received UID:\t%08X\n", packet.fields.UID);
  Serial.printf("Packet Number:\t%u\n", packet.fields.packetNumber);
  Serial.printf("Vbatt:\t\t%u mV\n", packet.fields.vbatt);
  Serial.printf("RSSI:\t\t%d dBm\n", packet.fields.rssi);
  Serial.printf("SNR:\t\t%d dB\n", packet.fields.snr);
  Serial.printf("Interval:\t%u s\n", packet.fields.interval);
  Serial.printf("AP Count:\t%u\n", packet.fields.apCount);

  for (uint8_t i = 0; i < packet.fields.apCount && i < MAX_APS_IN_PACKET; ++i) {
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             packet.fields.apList[i].bssid[0], packet.fields.apList[i].bssid[1],
             packet.fields.apList[i].bssid[2], packet.fields.apList[i].bssid[3],
             packet.fields.apList[i].bssid[4], packet.fields.apList[i].bssid[5]);

    Serial.printf("AP %u: %s  RSSI: %d dBm  Ch: %u\n",
                  i, mac, packet.fields.apList[i].rssi, packet.fields.apList[i].channel);
  }
  Serial.println();
}

char * formatCatPacket(const CatPacket &packet, char *buffer, size_t bufferSize) {
  int offset = 0;
  offset += snprintf(buffer + offset, bufferSize - offset, "%s", uploadURL);
  offset += snprintf(buffer + offset, bufferSize - offset, "?scan_id=%u", packet.fields.packetNumber);
  offset += snprintf(buffer + offset, bufferSize - offset, "&cat_vbatt=%u", packet.fields.vbatt);
  offset += snprintf(buffer + offset, bufferSize - offset, "&cat_rssi=%d", packet.fields.rssi);
  offset += snprintf(buffer + offset, bufferSize - offset, "&cat_snr=%d", packet.fields.snr);
  offset += snprintf(buffer + offset, bufferSize - offset, "&sta_vbatt=%u", Vbatt);
  offset += snprintf(buffer + offset, bufferSize - offset, "&sta_rssi=%d", rssi);
  offset += snprintf(buffer + offset, bufferSize - offset, "&sta_snr=%d", snr);

  for (uint8_t i = 0; i < packet.fields.apCount && i < MAX_APS_IN_PACKET; ++i) {
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             packet.fields.apList[i].bssid[0], packet.fields.apList[i].bssid[1],
             packet.fields.apList[i].bssid[2], packet.fields.apList[i].bssid[3],
             packet.fields.apList[i].bssid[4], packet.fields.apList[i].bssid[5]);

    offset += snprintf(buffer + offset, bufferSize - offset, "&bssid[]=%s&rssi[]=%d",
                       mac, packet.fields.apList[i].rssi);
  }
  return buffer;
}

uint32_t calculateCatBssidsCRC32(const CatPacket &packet) {
  uint32_t crc = 0;
    for (uint8_t i = 0; i < packet.fields.apCount && i < MAX_APS_IN_PACKET; ++i) {
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             packet.fields.apList[i].bssid[0], packet.fields.apList[i].bssid[1],
             packet.fields.apList[i].bssid[2], packet.fields.apList[i].bssid[3],
             packet.fields.apList[i].bssid[4], packet.fields.apList[i].bssid[5]);
    crc = esp_crc32_le(crc, &packet.fields.apList[i].bssid[0], 6);
  }
  return crc;
}

bool uploadDataToServer(const char *url) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("uploadDataToServer: WiFi not connected");
    return false;
  }

  HTTPClient http;
  if (!http.begin(url)) {
    Serial.println("uploadDataToServer: HTTP begin failed");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode <= 0) {
    Serial.printf("uploadDataToServer: GET failed, err=%d\n", httpCode);
    http.end();
    return false;
  }

  Serial.printf("uploadDataToServer: HTTP code %d\n", httpCode);
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    Serial.println("uploadDataToServer: OK response information:");
    Serial.println(payload);
    http.end();
    return true;
  } else {
    // non-OK response (e.g. 3xx/4xx/5xx)
    Serial.printf("uploadDataToServer: unexpected response %d\n", httpCode);
    http.end();
    return false;
  }
}


//
// *******************************************************************************************************
void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  snprintf(uploadURL, sizeof(uploadURL), "http://%s/api/track", uploadHost);
  // Station UID
  Serial.print("Station UID "); Serial.println(STATION_UID, HEX);

  // Pin initialization
  pinMode(RF_SW, OUTPUT);               // RF Switch
  pinMode(LED_BUILTIN, OUTPUT);         // built-in LED
  digitalWrite(RF_SW, HIGH);            // internal DIO2 controls Tx/Rx
  digitalWrite(LED_BUILTIN, HIGH);      // LED off

  // SSD1306 Display initialization
  display.begin();
  // static const uint8_t u8x8_d_ssd1306_128x64_noname_pump[] = {
  //   U8X8_START_TRANSFER(),             	/* enable chip, delay is part of the transfer start */
  //   U8X8_CA(0x08d, 0x014),		/* [2] charge pump setting (p62): 0x014 enable, 0x010 disable, SSD1306 only, should be removed for SH1106 */
  //   U8X8_END_TRANSFER(),             	/* disable chip */
  //   U8X8_END()             			/* end of sequence */
  // };
  // u8x8_cad_SendSequence(u8g2_GetU8x8(&display), u8x8_d_ssd1306_128x64_noname_pump);    
  display.clearDisplay();
  display.setFont(u8g2_font_8x13B_tr);
  display.setCursor(0, 15);
  display.println("Wio-SX1262");
  display.setCursor(0, 31);
  display.println("P2P Master");
  display.sendBuffer();
  delay(1000);
  display.clearDisplay();

  // int16_t begin(float freq = 434.0, float bw = 125.0, uint8_t sf = 9, uint8_t cr = 7, \
  // uint8_t syncWord = RADIOLIB_SX126X_SYNC_WORD_PRIVATE, int8_t power = 10, \
  // uint16_t preambleLength = 8, float tcxoVoltage = 1.6, bool useRegulatorLDO = false);
  // [NOTE} To be set up in compliance with area rules
  int state = radio.begin(LORA_FREQUENCY, LORA_BANDWIDTH, LORA_SPREADING_FACTOR, LORA_CODING_RATE, 
                          0x12, LORA_TX_POWER, LORA_PREAMBLE_LEN, 1.6, false);
  radio.setCRC(true);                // Enable CRC
  radio.explicitHeader();            // Explicit mode
  radio.forceLDRO(true);             // For SF12

  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("radio.begin() success!");
  } else {
    Serial.print("radio init failed, code ");
    Serial.println(state);
    errorBlink(4);                           // error [[ 4 ]]
  }

  // callback function when receive or transmit operation done
  radio.setDio1Action(setFlag);

#if SD

  // SD initialization and file open
  SPI.begin(sd_sck, sd_miso, sd_mosi, sd_ss); // Arduino SPI library uses VSPI circuit
  SPI.setFrequency(2000000UL);
  SPI.setDataMode(SPI_MODE0);
  SPI.setHwCs(true);
  
  if(!SD.begin(sd_ss)) { 
    Serial.println("SD begin failure!");
    display.clearDisplay();
    display.setCursor(0, 15);
    display.println("SD begin failure");
    display.sendBuffer();
    errorBlink(2);                           // error [[ 2 ]]
  } 

  logFile = SD.open(logFileName, FILE_APPEND);  // APPEND insted of WRITE
  if(!logFile) { 
    Serial.println(logFileName + " open failure");
    display.clearDisplay();
    display.setCursor(0, 15);
    display.println("file open failure");
    display.sendBuffer();
    errorBlink(3);                           // error [[ 3 ]]   
  }   
  Serial.println("SD initialization finished");
#endif
  // setup end
  display.setCursor(0, 15);
  display.print("-- Receiv0ng --");
  display.sendBuffer();
}



// ***********************************************************************************************************
// Receive-only loop: re-arm radio, wait for CatPacket, process, repeat.
CatPacket catPacket;
void loop()
{
  Serial.println("******** Station waiting for cat packet ********");
  display.clearDisplay();
  display.setCursor(0, 15); display.print("Listening...");
  display.sendBuffer();

  Vbatt = analogReadMilliVolts(A0);          // station's own battery (for logging/upload)

  // arm receiver
  digitalWrite(LED_BUILTIN, LOW);
  operationDone = false;
  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("startReceive failed, code "); Serial.println(state);
    errorBlink_1(2);
    delay(1000);
    return;
  }

  // wait up to RX_POLL_TIMEOUT; on timeout we simply re-arm on next loop iteration
  timeoutCheck = millis();
  while(!operationDone && ((millis() - timeoutCheck) < RX_POLL_TIMEOUT)) {
    delay(1);
  }
  digitalWrite(LED_BUILTIN, HIGH);

  if (!operationDone) {
    Serial.println("rx timeout, re-arming");
    return;
  }

  txtime = millis();
  state = radio.readData(catPacket.bytes, sizeof(catPacket.bytes));
  senderuid = catPacket.fields.UID;

  if (state == RADIOLIB_ERR_NONE && senderuid == CAT_UID) {
    rxdata = "";
    for(int i = 0; i < sizeof(catPacket.bytes); i++) {
      sprintf(printBuff, "%02x", catPacket.bytes[i]);
      rxdata += printBuff;
    }
    rxdata.toUpperCase();
    Serial.println(rxdata);

    rssi = radio.getRSSI();
    snr = radio.getSNR();
    Serial.print("local RSSI:\t\t"); Serial.print(rssi); Serial.println(" dBm");
    Serial.print("local SNR:\t\t"); Serial.print(snr); Serial.println(" dB");

    printCatPacket(catPacket);
    uint32_t currentCatBssidsCRC32 = calculateCatBssidsCRC32(catPacket);
    if (currentCatBssidsCRC32 == previousCatBssidsCRC32) {
      Serial.println("Same Cat BSSIDs, cat did not move.");
    } else {
      previousCatBssidsCRC32 = currentCatBssidsCRC32;
      Serial.println("New Cat BSSIDs received, cat moved.");
      char * formattedData = formatCatPacket(catPacket, printBuff, sizeof(printBuff));
      Serial.println("Formatted Data for Upload:");
      Serial.println(formattedData);
      uploadDataToServer(formattedData);
    }

    display.clearDisplay();
    display.setCursor(64, 15); display.print("Received");
    display.setCursor(0, 31); display.print(rssi); display.print(" dBm");
    display.setCursor(80, 31); display.print(catPacket.fields.vbatt/1000.0); display.print(" V");
    display.setCursor(0, 47); display.print("pkt ");
    display.setCursor(32, 47); display.print(catPacket.fields.packetNumber);
    display.setCursor(80, 47); display.print("ap ");
    display.setCursor(104, 47); display.print(catPacket.fields.apCount);
    display.setCursor(0, 63); display.print(catPacket.fields.rssi); display.print(" dBm");
    display.setCursor(80, 63); display.print(catPacket.fields.snr); display.print(" dB");
    display.sendBuffer();
  } else if (state == RADIOLIB_ERR_NONE) {
    Serial.print("received but wrong UID: 0x"); Serial.println(senderuid, HEX);
  } else {
    Serial.print("receive failed, code "); Serial.println(state);
    errorBlink_1(3);
  }

  Serial.print("Processing time [ms]: "); Serial.println(millis() - txtime);
}



//*********************************************************************************
// this function is called when a complete packet is received or transmitted
void setFlag(void) {
  operationDone = true;
}


//*********************************************************************************

