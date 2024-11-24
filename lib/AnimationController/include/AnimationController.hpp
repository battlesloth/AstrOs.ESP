#ifndef ANIMATIONCONTROLLER_HPP
#define ANIMATIONCONTROLLER_HPP

#include <AnimationCommand.hpp>

#include <string>

// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#define QUEUE_CAPACITY 30

typedef struct
{
    int domeLimit;
    QueueHandle_t animation_cmd;
    QueueHandle_t kangaroo_cmd;
    QueueHandle_t kangaroo_rsp;
} ac_configuration_t;

class AnimationController
{
private:
    // script queue
    bool queueing;
    int queueFront;
    int queueRear;
    int queueSize;
    int queueCapacity = QUEUE_CAPACITY;
    std::string scriptQueue[QUEUE_CAPACITY];

    // script
    bool scriptLoaded;
    int delayTillNextEvent;
    std::vector<AnimationCommand> scriptEvents;

    // functions
    bool queueIsEmpty();
    bool queueIsFull();
    void handleLastServoEvent();
    void loadNextScript();
    void parseScript(std::string script);

public:
    AnimationController();
    ~AnimationController();
    void panicStop();
    bool queueScript(std::string script);
    bool queueCommand(std::string command);
    bool scriptIsLoaded();
    CommandTemplate *getNextCommandPtr();
    int msTillNextServoCommand();
};

extern AnimationController AnimationCtrl;

#endif