#ifndef ANIMATIONCONTROLLER_H
#define ANIMATIONCONTROLLER_H

#include <ServoCommand.h>

// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>


#define QUEUE_CAPACITY 30
#define COMMAND_NAME_SIZE 30
#define SCRIPT_CAPACITY 30

typedef struct {
    int domeLimit;
    QueueHandle_t animation_cmd;
    QueueHandle_t kangaroo_cmd;
    QueueHandle_t kangaroo_rsp;
} ac_configuration_t;

class AnimationController
{
private:
    
    // servo states
    int lowerLimit;
    int spinnerP;
    bool spinnerMoving;
    int lifterP;
    bool lifterMoving;
    
    // script queue
    int queueFront;
    int queueRear;
    int queueSize;
    int queueCapacity = QUEUE_CAPACITY;
    int commandSize = COMMAND_NAME_SIZE + 1;
    char scriptQueue[QUEUE_CAPACITY][COMMAND_NAME_SIZE + 1];

    // servo script
    bool servoScriptLoaded;
    int servoEvents;
    int servoEventsFired;
    int delayTillNextServoEvent;
    ServoCommand servoScript[SCRIPT_CAPACITY];
  
    // functions
    bool queueIsEmpty();
    bool queueIsFull();
    void handleLastServoEvent();
    void loadNextScript();
public:
    AnimationController(int lowerLimit);
    ~AnimationController();    
    bool queueScript(char scriptName[]);
    bool servoScriptIsLoaded();
    char* getNextServoCommand();
    int msTillNextServoCommand();
    void setLifterP(int p, bool isMoving);
    void setSpinnerP(int s, bool isMoving);
 
};

extern AnimationController AnimationCtrl;

#endif