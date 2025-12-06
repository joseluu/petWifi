//----------------------------------------------------------------------------------------------
// BoardBoard Library : esp32 by Espressif Systems 3.2.0
// Board Select       : XIAO_ESP32S3
//----------------------------------------------------------------------------------------------
// [NOTE] When upload, BOOT+RESET:ON, RESET:OFF, then BOOT:OFF

// 1. transmit data 11 bytes(stationUID 4, randomNumber 4, Vbatt 2, (previous)veryfiResult 1)
// 2. wait for response from cat
// 3. receive data 11 bytes(catUID 4, verifyNumber 4, rssi 1, snr 1, interval 1)
// 4. verify sent randomNumber with received verifyNumber



// 2025/04/04

#include <RadioLib.h>           // RadioLib by Jan Gromes 7.1.2  
#include <U8g2lib.h>            // U8g2 2.35.30
#include <SPI.h>
#include <SD.h>

#include "clib/u8g2.h"          // for charge pump setting: try to fix luminosity problem

#include "packet.h"

#define CHECK_INTERVAL 10000    // communication check interval [mS]
#define TX_INTERVAL     30000   // depend on the erea rules, total of 360 sec or less per hour

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
uint16_t Vbatt;                     // battery voltage data [V]
uint32_t randomNumber;              // random number to verify
uint32_t verifyNumber;              // verify number
uint8_t verifyResult;               // received verify result
char printBuff[4];                  // for sprintf()
String  txdata = "";                // transmission data packet string
String  rxdata = "";                // received data packet string
bool operationDone = false;         // receive or transmit operation done
String logFileName = "/LoRaLog.txt"; // Log file name, need '/'


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

//
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
StationPacket stationPacket;
CatPacket catPacket;
void loop() 
{
  uint32_t processTime;
  uint32_t delayTime;
  do {  
    Serial.println("******** Station transmitting query packet ********");
      
    Vbatt = 3999; // sample

    // transmit data setting
    stationPacket.fields.waitTime = TX_INTERVAL/1000 -22;  // forced inactivity time for cat transmitter before listening again [sec]


    // transmit a packet
    txtime = millis(); 
    uint16_t sec = txtime / 1000;                   // transmission time [sec] 

    stationPacket.incrementPacketNumber();
    digitalWrite(LED_BUILTIN, LOW);
        operationDone = false;
        state = radio.startTransmit(stationPacket.bytes, sizeof(stationPacket.bytes));

        // wait for transmittion completion
        timeoutCheck = millis();
        while(!operationDone & ((millis() - timeoutCheck) < CHECK_INTERVAL)) {
          delay(1);
        }
    digitalWrite(LED_BUILTIN, HIGH);
   
    // print and display transmitted data
    txdata = "";
    for(int i = 0; i < sizeof(stationPacket.bytes); i++) {
      sprintf(printBuff, "%02x", stationPacket.bytes[i]);
      txdata += printBuff;
    }
    txdata.toUpperCase();
    Serial.println(txdata);

    Serial.print("Transmit Station UID:\t"); Serial.println(stationPacket.fields.UID, HEX);  
    Serial.print("Transmit packet number:\t"); Serial.println(stationPacket.fields.packetNumber);  
    Serial.print("Forced inactivity Time:\t\t"); Serial.print(stationPacket.fields.waitTime); Serial.println(" sec");
    Serial.println();

    display.setCursor(0, 15); display.print(" Tr");    
    display.sendBuffer();    
  } while(state != RADIOLIB_ERR_NONE);  // if status error, transmit again

//-------------------------------------------------------------------------------------------------
  Serial.println("*station** Transmit done: Waiting for incoming cat response ******** ");

  // start listening for LoRa packets
  digitalWrite(LED_BUILTIN, LOW);
      operationDone = false;    
      state = radio.startReceive();

      // check the flag
      // If not received, proceed to next communication cycle to atempt synchronization recovery
      timeoutCheck = millis();
      while(!operationDone & ((millis() - timeoutCheck) < CHECK_INTERVAL)) {
        delay(1);
      }  
  digitalWrite(LED_BUILTIN, HIGH);

  // received status check  
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("failed code ");
    Serial.println(state);
    errorBlink_1(2);                        // error [[ 2 ]]
  }

  // read a received packet 
  state = radio.readData(catPacket.bytes, sizeof(catPacket.bytes));

  // read status and Station UID check
  senderuid = catPacket.fields.UID;
  if (state == RADIOLIB_ERR_NONE && senderuid == STATION_UID) {   // check status and Station UID
    rxdata = "";
    for(int i = 0; i < sizeof(catPacket.bytes); i++) {
      sprintf(printBuff, "%02x", catPacket.bytes[i]);
      rxdata += printBuff;
    }
    rxdata.toUpperCase();
    Serial.println(rxdata);

    // restoration of received data
    rssi = catPacket.fields.rssi;  // RSSI   min. -127[dBm]
    snr = catPacket.fields.snr;   // SNR
    Vbatt = catPacket.fields.vbatt; // battery voltage [mV]
    int bssidCount = catPacket.fields.apCount; // number of APs in packet

    // random number verification
    if(randomNumber == verifyNumber) {
      verifyResult = 1;    
    } else {
      verifyResult = 0;
    }

    Serial.print("local RSSI:\t\t"); Serial.print(radio.getRSSI()); Serial.println(" dBm");
    Serial.print("local SNR:\t\t"); Serial.print(radio.getSNR()); Serial.println(" dB");

    printCatPacket(catPacket);

    display.clearDisplay();
    display.setCursor(64, 15); display.print("Received");  
    display.setCursor(0, 31); display.print(radio.getRSSI()); display.print(" ");
    display.setCursor(80, 31); display.print(Vbatt/1000.0); display.print(" V");   
    display.setCursor(0, 47); display.print(verifyNumber, HEX);
    display.setCursor(80, 47); display.print(verifyResult ? "good" : "bad");
    display.setCursor(0, 63); display.print(rssi); display.print(" dBm");
    display.setCursor(80, 63); display.print(snr); display.print(" dB");

    display.sendBuffer();
  }
  else {
    // error occurred when receiving
    Serial.print("receive failed, code ");
    Serial.println(state);
    errorBlink_1(3);                        // error [[ 3 ]]
  }  
  digitalWrite(LED_BUILTIN, HIGH);
  processTime = millis() - txtime;
  Serial.print("Processing time [ms]: "); Serial.println(processTime);
  delayTime = TX_INTERVAL - (millis() - txtime);
  Serial.print("******** now waiting for next transmit in :\t"); Serial.print((delayTime)/1000); Serial.println(" sec");
  // wait for transmit interval
  delay(delayTime);
}



//*********************************************************************************
// this function is called when a complete packet is received or transmitted
void setFlag(void) {
  operationDone = true;
}


//*********************************************************************************

