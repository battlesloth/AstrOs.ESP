#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "AstrOsConstants.h"
#include "ServoCommand.h"

ServoCommand::ServoCommand(Servo t, Command c, int p, int s, int d) :
    servo(t), command(c), position(p), speed(s), duration(d) {}

ServoCommand::ServoCommand() :
    servo(Nothing), command(Invalid), position(0), speed(0), duration(0) {}

ServoCommand::~ServoCommand() { }

void ServoCommand::setValues(Servo s, Command cmd, int pos, int sp, int dur){
    servo = s;
    command = cmd;
    position = pos;
    speed = sp;
    duration = dur;
}

// Make sure to release the return value
char* ServoCommand::toCommand() {
    const char* commandText = AstrOsConstants::Nothing;

    switch (command)
    {
    case Command::Start:
        commandText = AstrOsConstants::Start;
        break;
    case Command::Home:
        return getHomeCommand();
    case Command::Position:
        if (speed > 0){
            return getDualCommand(false);
        } 
        else {
            return getSingleCommand(AstrOsConstants::Position, position);
        }
        break;   
    case Command::PostionIncremental:
        if (speed > 0){
            return getDualCommand(true);
        } 
        else {
            return getSingleCommand(AstrOsConstants::PositionIncremental, position);
        }    
    case Command::Speed:
        return getSingleCommand(AstrOsConstants::Speed, speed);
    case Command::SpeedIncremental:
        return getSingleCommand(AstrOsConstants::SpeedIncremental, speed);  
    case Command::PowerDown:
        commandText = AstrOsConstants::PowerDown;
        break;    
    case Command::GetPostion:
        commandText = AstrOsConstants::GetPosition;
        break;    
    case Command::GetSpeed:
        commandText = AstrOsConstants::GetSpeed;
        break;    
    case Command::GetPostionIncremental:
        commandText = AstrOsConstants::GetPositionIncremental;
        break;
    case Command::GetSpeedIncremental:
        commandText = AstrOsConstants::GetSpeedIncremental;
        break;
    case Command::GetPostionMax:
        commandText = AstrOsConstants::GetPositionMax;
        break;
    case Command::GetPostionMin:
        commandText = AstrOsConstants::GetPositionMin;
        break;
    default:
        break;
    }

    int size = 4 + strlen(commandText);
    char *s = (char*)malloc(size);
    snprintf(s, size, "%i,%s%c", servo, commandText, '\n');

    return s;
}

char* ServoCommand::getHomeCommand(){
    char *s = (char*)malloc(11);
    snprintf(s, 11, "%i,home%c", servo, '\n');
    return s;
}

char* ServoCommand::getSingleCommand(const char* cmd, int val){
    int size = 4 + strlen(cmd) + sizeof(val);
    char *s = (char*)malloc(size);
    snprintf(s, size, "%i,%s%d%c", servo, cmd, val, '\n');
    return s;
}

char* ServoCommand::getDualCommand(bool isIncremental){
    int size = 10 + sizeof(speed) + sizeof(speed);
    char *s = (char*)malloc(size);

    if (isIncremental){
        snprintf(s, size, "%i,pi%d s%d%c", servo, position, speed, '\n');
    } else {
        snprintf(s, size, "%i,p%d s%d%c", servo, position, speed, '\n');
    }

    return s;
}

int ServoCommand::getDuration(){
    return duration;
}