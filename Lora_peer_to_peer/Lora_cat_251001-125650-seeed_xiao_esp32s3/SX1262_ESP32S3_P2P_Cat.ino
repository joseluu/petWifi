//----------------------------------------------------------------------------------------------
// BoardBoard Library : esp32 by Espressif Systems 3.2.0
// Board Select       : XIAO_ESP32S3
//----------------------------------------------------------------------------------------------
// [NOTE] When upload, BOOT+RESET:ON, RESET:OFF, then BOOT:OFF


// 1. wait for a packet from station
// 2. receives station data 11 bytes(stationUID 4, randomNumber 4, Vbatt 2, (previous)veryfiResult 1)
// 3. transmits response data 11 bytes(catUID 4, verifyNumber 4, rssi 1, snr 1, interval 1)

// 2025/04/04

#include <RadioLib.h>           // RadioLib by Jan Gromes 7.1.2
#include <U8g2lib.h>            // U8g2 2.35.30

#define CHECK_INTERVAL  5000   // communication check interval [mS]
#define RF_SW           D5      // RF Switch

// Device Unique ID
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
#define LORA_BANDWIDTH 250 
#define LORA_SPREADING_FACTOR 12  
#define LORA_CODING_RATE 8
#define LORA_TX_POWER 22    
#define LORA_PREAMBLE_LEN 12
// bit time = (2^spreadingFactor)/bandwidth 4096/62.5 = 65.6 [ms]   
// message duration = (preamble + payload + 4.25 )*8*FEC factor*bit time
// for SF12 BW62.5 CR 8, 11 bytes payload => about 11 544 [ms]
// idem for SF12 BW250 CR 8 => about 2 914 [ms]

// SSD1306 software I2C library, SCL=D6, SDA=D7, 400kHz
U8G2_SSD1306_128X64_NONAME_F_SW_I2C display(U8G2_R0, /*clock=*/ D6, /*data=*/ D7, /*reset=*/ U8X8_PIN_NONE);
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
uint8_t receivedInterval;           // Station received interval [sec]
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

  // Display initialization
  display.begin();            			/* end of sequence */
  display.clearDisplay();
  display.setFont(u8g2_font_8x13B_tr);
  display.setCursor(0, 15);
  display.println("Wio-SX1262");
  display.setCursor(0, 31);
  display.println("P2P Slave");
  display.sendBuffer();
  delay(1000);
  display.clearDisplay();  

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

  // setup end
  display.setCursor(0, 63);
  display.print("Setup END");
  display.sendBuffer();
  delay(1000);
  display.clearDisplay();
}

// ***********************************************************************************************************
void loop() 
{
  Serial.println("***cat** Waiting for incoming station packet ********");

  // start listening for LoRa packets
  digitalWrite(LED_BUILTIN, LOW);
      operationDone = false;    
      state = radio.startReceive();
      // check if the flag is set
      // If not received, proceed to next communication cycle to atempt synchronization recovery
      timeoutCheck = millis();
      while(!operationDone & ((millis() - timeoutCheck) < CHECK_INTERVAL)) {
        delay(1);
      }   
  digitalWrite(LED_BUILTIN, HIGH);

  // received status check  
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("radio.startReceive() failed code ");
    Serial.println(state);
    errorBlink_1(2);                        // error [[ 2 ]]
  } else {

    // read a packet 
    state = radio.readData(ub.Buff_u8, dataNum);

    senderuid = ub.Buff_u32[0];    // Station device UID
    if (state == RADIOLIB_ERR_NONE && senderuid == STATION_UID) {   // Check Station ID
      receivedTime = millis();
      receivedInterval = (receivedTime - timestamp) / 1000;       // [sec]
      timestamp = receivedTime;

      rxdata = "";
      for(int i = 0; i < dataNum; i++) {
        sprintf(printBuff, "%02x", ub.Buff_u8[i]);
        rxdata += printBuff;
      }
      rxdata.toUpperCase();
      Serial.println(rxdata);

      // restoration of received data
      randomNumber = ub.Buff_u32[1];    // random number to verify
      Vbatt = ub.Buff_u16[4];           // battery voltage [mV]
      verifyResult = ub.Buff_u8[10];    // verify result 

      Serial.printf("Sender UID:\t%08x\n", senderuid);    
      Serial.print("Interval:\t"); Serial.print(receivedInterval); Serial.println(" sec");
      Serial.print("Vbatt:\t\t"); Serial.print(Vbatt/1000.0); Serial.println(" V");
      Serial.print("randomNumber:\t"); Serial.println(randomNumber, HEX);
      Serial.print("Verify:\t\t"); Serial.println(verifyResult ? "good" : "bad");

      // RSSI and SNR of the last received packet
      rssi = radio.getRSSI();
      Serial.print("RSSI:\t\t"); Serial.print(rssi); Serial.println(" dBm");
      snr = radio.getSNR();
      Serial.print("SNR:\t\t"); Serial.print(snr); Serial.println(" dB");

      display.clearDisplay();
      display.setCursor(0, 15); display.print("Received"); 
      display.setCursor(0, 31); display.print(randomNumber, HEX);
      display.setCursor(80, 31); display.print(verifyResult ? "good" : "bad");    
      display.setCursor(0, 47); display.print(rssi); display.print(" dBm");
      display.setCursor(80, 47); display.print(snr); display.print(" dB");
      display.setCursor(0, 63); display.print(receivedTime / 1000); display.print(" sec");
      display.setCursor(80, 63); display.print(Vbatt/1000.0); display.print(" V");   
      display.sendBuffer();
      
    } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
      // packet was received, but is malformed
      Serial.println("******** CRC error ********");
      goto loop_again;  
    } else {
      // some other error occurred when receiving
      Serial.print("radio.readData() failed, code ");
      Serial.print(state);
      Serial.print(" or bad sender UID: ");
      Serial.println(senderuid);
      errorBlink_1(3);                        // error [[ 3 ]]    
      goto loop_again;              
    }
    digitalWrite(LED_BUILTIN, HIGH);
    delay(1000);     
  
    Serial.println("******** Transmitting response packet ********");

    Vbatt = 3.999;  // sample

    // transmit data setting
    ub.Buff_u32[0] = CAT_UID;         // Cat device UID
    verifyNumber = randomNumber;
    ub.Buff_u32[1] = verifyNumber;      // to verify
    ub.Buff_i8[8] = rssi;               // Cat RSSI  [dBm]
    ub.Buff_i8[9] = snr;                // Cat SN ratio  [dB]
    ub.Buff_u8[10] = receivedInterval;  // packet reserved interval [sec]

    digitalWrite(LED_BUILTIN, LOW);
        operationDone = false;
        state = radio.startTransmit(ub.Buff_u8, dataNum);

        // wait for transmittion completion
        timeoutCheck = millis();
        while(!operationDone & ((millis() - timeoutCheck) < CHECK_INTERVAL)) {
          delay(1);
        }
    digitalWrite(LED_BUILTIN, HIGH);

    // print and display transmitted data
    txdata = "";
    for(int i = 0; i < dataNum; i++) {
      sprintf(printBuff, "%02x", ub.Buff_u8[i]);
      txdata += printBuff;
    }
    txdata.toUpperCase();
    Serial.println(txdata);

    Serial.printf("Cat UID:\t%08x\n", CAT_UID);
    Serial.print("RSSI\t\t"); Serial.print(rssi); Serial.println(" dBm");
    Serial.print("SNR\t\t"); Serial.print(snr); Serial.println(" dB");
    Serial.print("verifyNumber:\t"); Serial.println(verifyNumber, HEX);

    display.clearDisplay();
    display.setCursor(40, 15); display.print("Transmitted");  
    display.setCursor(0, 31); display.print(verifyNumber, HEX);
    display.setCursor(80, 31); display.print(verifyResult ? "good" : "bad");
    display.setCursor(0, 47); display.print(rssi); display.print(" dBm");
    display.setCursor(80, 47); display.print(snr); display.print(" dB");
    display.setCursor(0, 63); display.print(receivedInterval); display.print(" sec");
    display.setCursor(80, 63); display.print(Vbatt/1000.0); display.print(" V");
    display.sendBuffer();   

    // error status check
    if (state == RADIOLIB_ERR_NONE) {
      // packet was successfully sent
      Serial.println("transmission done!");
      Serial.println();
    } else {
      // some other error occurred
      Serial.print("failed, code ");
      Serial.println(state);
    }
    digitalWrite(LED_BUILTIN, HIGH);
    delay(1000);   
  }
  loop_again:
  delay(100);
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