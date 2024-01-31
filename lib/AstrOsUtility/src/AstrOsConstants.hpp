#ifndef ASTROSCONSTANTS_H
#define ASTROSCONSTANTS_H

namespace AstrOsConstants
{

    /***********************************
     *  Module
     ***********************************/
    constexpr const char *Version = "v0.9.0";

    constexpr const char *ModuleName = "AstrOs-esp32";
    constexpr const char *SerialNumber = "0001";
    constexpr const char *ApSsid = "AstrOs-";
    constexpr const char *Password = "password";

    /***********************************
     *  Kangaroo X2 commands
     ***********************************/

    constexpr const char *Start = "start";
    constexpr const char *Home = "home";
    constexpr const char *Position = "p";
    constexpr const char *Speed = "s";
    constexpr const char *PositionIncremental = "pi";
    constexpr const char *SpeedIncremental = "si";
    constexpr const char *PowerDown = "powerdown";
    constexpr const char *GetPosition = "getp";
    constexpr const char *GetSpeed = "gets";
    constexpr const char *GetPositionIncremental = "getpi";
    constexpr const char *GetSpeedIncremental = "getpsi";
    constexpr const char *GetPositionMax = "getmax";
    constexpr const char *GetPositionMin = "getmin";
    constexpr const char *Nothing = "";

    /************************************
     * Log messages
     ************************************/

    constexpr const char *OVERFLOW_ERROR = "Overflow";
    constexpr const char *MESSAGE_INTERUPTED = "MsgInteupt";
    constexpr const char *HEARTBEAT = "Heatbeat";
    constexpr const char *COMMAND_RECEIVED = "CommandReceived";
    constexpr const char *COMMAND_QUEUED = "CommandQueued";
}
#endif
