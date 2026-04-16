#ifndef ANIMATIONCONTROLLER_HPP
#define ANIMATIONCONTROLLER_HPP

#include <AnimationCommand.hpp>
#include <AstrOsAnimationEngine.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
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
    SemaphoreHandle_t animationMutex;

    ScriptQueue<QUEUE_CAPACITY> scriptQueue_;
    std::atomic<bool> queueing;

    std::atomic<bool> scriptLoaded;
    std::atomic<int> delayTillNextEvent;
    std::atomic<uint32_t> panicGeneration;
    std::vector<AnimationCommand> scriptEvents;

    void loadNextScript();

public:
    AnimationController();
    ~AnimationController();
    void panicStop();
    bool queueScript(std::string script);
    bool queueCommand(std::string command);
    bool scriptIsLoaded();
    std::unique_ptr<CommandTemplate> getNextCommandPtr();
    int msTillNextServoCommand();
};

extern AnimationController AnimationCtrl;

#endif
