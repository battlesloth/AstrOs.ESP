#ifndef PACKETTRACKER_H
#define PACKETTRACKER_H

#include <unordered_map>
#include <vector>
#include <string>

#define PACKET_EXPIRATION_TIME 1000

typedef struct
{
    int packetNumber;
    int totalPackets;
    std::string payload;
} PacketData;

typedef enum
{
    ERROR,
    SUCCESS,
    MESSAGE_COMPLETE,
    PACKET_EXISTS,
    MESSAGE_EXPIRED
} AddPacketResult;

class PacketTracker
{
private:
    std::unordered_map<std::string, std::vector<PacketData>> packetMap;
    std::unordered_map<std::string, int> packetExpirationMap;

    void expireMessages(int time);

public:
    PacketTracker(/* args */);
    ~PacketTracker();

    AddPacketResult addPacket(std::string msgId, PacketData data, int time);
    std::string getMessage(std::string msgId);
};

#endif