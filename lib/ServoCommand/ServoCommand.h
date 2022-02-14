#ifndef SERVOCOMMAND_H
#define SERVOCOMMAND_H

enum Servo {Nothing = 0, Lifter = 1, Spinner = 2};
enum Command {Invalid,
                Start,
                Stow,
                Home,
                Position, 
                Speed, 
                PostionIncremental, 
                SpeedIncremental,
                PowerDown,
                GetPostion,
                GetSpeed,
                GetPostionIncremental,
                GetSpeedIncremental,
                GetPostionMax,
                GetPostionMin
                };

class ServoCommand
{
private:
    Servo servo;
    Command command;
    int position;
    int speed;
    int duration;
    char* getHomeCommand();
    char* getSingleCommand(const char* cmd, int val);
    char* getDualCommand(bool isIncremental);
public:
    ServoCommand(Servo servo, Command Command, int position, int speed, int duration);
    ServoCommand();
    ~ServoCommand();
    void setValues(Servo servo, Command Command, int position, int speed, int duration);
    char* toCommand();
    int getDuration();
};


#endif