#include "PacketTracker.hpp"

PacketTracker::PacketTracker()
{
    this->packetMap = std::unordered_map<std::string, std::vector<PacketData>>();
    this->packetExpirationMap = std::unordered_map<std::string, int>();
}

PacketTracker::~PacketTracker()
{
}

AddPacketResult PacketTracker::addPacket(std::string msgId, PacketData data, int time)
{
    if (this->packetMap.find(msgId) == this->packetMap.end())
    {
        this->packetMap[msgId] = std::vector<PacketData>();
        this->packetExpirationMap[msgId] = time;
        this->packetMap[msgId].push_back(data);

        this->expireMessages(time);

        return AddPacketResult::SUCCESS;
    }

    for (auto it = this->packetMap[msgId].begin(); it != this->packetMap[msgId].end(); it++)
    {
        if (it->packetNumber == data.packetNumber)
        {
            this->expireMessages(time);
            return AddPacketResult::PACKET_EXISTS;
        }
    }

    this->packetMap[msgId].push_back(data);
    this->packetExpirationMap[msgId] = time;

    this->expireMessages(time);

    if (this->packetMap[msgId].size() == data.totalPackets)
    {
        return AddPacketResult::MESSAGE_COMPLETE;
    }

    return AddPacketResult::SUCCESS;
}

std::string PacketTracker::getMessage(std::string msgId)
{
    std::string message = "";
    if (this->packetMap.find(msgId) != this->packetMap.end())
    {
        for (auto it = this->packetMap[msgId].begin(); it != this->packetMap[msgId].end(); it++)
        {
            message += it->payload;
        }
        this->packetMap.erase(msgId);
        this->packetExpirationMap.erase(msgId);
    }
    return message;
}

void PacketTracker::expireMessages(int time)
{
    for (auto it = this->packetExpirationMap.begin(); it != this->packetExpirationMap.end();)
    {
        // if the packet time is further in the future than time provided then we can assume timer overflow
        if (it->second + PACKET_EXPIRATION_TIME < time || it->second > time)
        {
            this->packetMap.erase(it->first);
            it = this->packetExpirationMap.erase(it);
        }
        else
        {
            it++;
        }
    }
}