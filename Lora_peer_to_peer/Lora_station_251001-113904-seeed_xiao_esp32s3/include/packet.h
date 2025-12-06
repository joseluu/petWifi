#ifndef PACKET_H
#define PACKET_H

#define STATION_UID   0xFE55C37C  // random
#define CAT_UID  0x54705810  // random
#define PACKET_VERSION 1

class StationPacket {
public:
    union {
        struct {
            uint32_t UID;
            uint8_t packetVersion;
            uint8_t packetNumber;
            uint16_t waitTime;
        } fields;
        uint8_t bytes[8];
    };


    StationPacket() {
        fields.UID = STATION_UID;
        fields.packetVersion = PACKET_VERSION;
        fields.packetNumber = 0;
        fields.waitTime = 0;
    }
    ~StationPacket() {}

    void incrementPacketNumber() {
        fields.packetNumber++;
    }
} ;

class AccessPoint {
public:
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel; // 8 bytes total

    AccessPoint() {
        for (int i = 0; i < 6; i++) {
            bssid[i] = 0;
        }
        rssi = 0;
        channel = 0;
    }
    AccessPoint(uint8_t* addr, int8_t signal, uint8_t ch) {
        for (int i = 0; i < 6; i++) {
            bssid[i] = addr[i];
        }
        rssi = signal;
        channel = ch;
    }
    ~AccessPoint() {}
    void setAccessPoint(uint8_t* addr, int8_t signal, uint8_t ch) {
        for (int i = 0; i < 6; i++) {
            bssid[i] = addr[i];
        }
        rssi = signal;
        channel = ch;
    }
};

#define MAX_APS_IN_PACKET 5

class CatPacket {
public:
    union {
        struct {
            uint32_t UID;
            uint8_t packetNumber;
            uint16_t vbatt;
            int8_t rssi;
            int8_t snr;
            uint8_t interval;
            uint8_t apCount; // size 11
            AccessPoint apList[MAX_APS_IN_PACKET];
        } fields;
        uint8_t bytes[11 + sizeof(AccessPoint) * MAX_APS_IN_PACKET];
    };

    CatPacket() {
        fields.UID = 0;
        fields.rssi = 0;
        fields.snr = 0;
        fields.interval = 0;
        fields.apCount = 0;
        for (int i = 0; i < MAX_APS_IN_PACKET; i++) {
            fields.apList[i] = AccessPoint();
        }
    }
    ~CatPacket() {}
} ;

#endif // PACKET_H