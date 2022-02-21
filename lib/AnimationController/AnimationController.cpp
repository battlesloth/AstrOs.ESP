#include <string.h>
#include <esp_log.h>
#include <algorithm>

#include <AnimationController.h>
#include <AnimationCommand.h>
#include <StorageManager.h>

static const char *TAG = "AnimationController";

AnimationController AnimationCtrl;

AnimationController::AnimationController()
{
    queueRear = -1;
}

AnimationController::~AnimationController() {}

void AnimationController::panicStop()
{
    ESP_LOGI(TAG, "Panicing!");
    // TODO
}

bool AnimationController::queueScript(std::string scriptId)
{

    ESP_LOGI(TAG, "Queueing %s", scriptId.c_str());

    if (queueIsFull())
    {
        ESP_LOGI(TAG, "Queue is full");
        return false;
    }

    bool loadScript = !scriptLoaded;

    queueRear = (queueRear + 1) % queueCapacity;

    scriptQueue[queueRear] = scriptId;

    queueSize++;

    if (loadScript)
    {
        loadNextScript();
    }

    return true;
}

bool AnimationController::queueIsFull()
{
    return (queueSize == queueCapacity);
}

bool AnimationController::queueIsEmpty()
{
    return (queueSize == 0);
}

void AnimationController::loadNextScript()
{

    if (queueIsEmpty())
    {
        scriptLoaded = false;
        return;
    }

    ESP_LOGI(TAG, "Loading script %s", scriptQueue[queueFront].c_str());

    std::string path = "scripts/" + std::string(scriptQueue[queueFront]);

    std::string script = Storage.readFile(path);

    AnimationController::parseScript(script);

    ESP_LOGI(TAG, "Loaded: %s", script.c_str());
    // LoadFromMemory(scriptQueue[queueFront]);

    /*****************************************
     * Test script
     *****************************************/
    // if (strncmp(scriptQueue[queueFront], "#start", strlen("#home")) == 0){
    //     servoScript[0].setValues(Spinner, Start, 0, 0, 500);
    //     servoScript[1].setValues(Lifter, Start, 0, 0, 500);
    //     servoEvents = 2;
    // }
    // if (strncmp(scriptQueue[queueFront], "#home", strlen("#home")) == 0){
    //     servoScript[0].setValues(Spinner, Home, 0, 0, 2000);
    //     servoScript[1].setValues(Lifter, Home, 0, 0, 2000);
    //     servoEvents = 2;
    // }
    // if (strncmp(scriptQueue[queueFront], "#stow", strlen("#stow")) == 0){
    //     servoScript[0].setValues(Spinner, Home, 0, 0, 2000);
    //     servoScript[1].setValues(Lifter, Position, -15, 0, 2000);
    //     servoEvents = 2;
    // }
    // else if (strncmp(scriptQueue[queueFront], "#deploy", strlen("#deploy")) == 0) {
    //     servoScript[0].setValues(Lifter, Position, 780, 0, 1000);
    //     servoEvents = 1;
    // }
    // else if (strncmp(scriptQueue[queueFront], "#sneaky", strlen("#sneaky")) == 0) {
    //     servoScript[0].setValues(Lifter, Position, 780, 100, 12000);
    //     servoScript[1].setValues(Lifter, Position, 350, 100, 6000);
    //     servoScript[2].setValues(Lifter, Position, 690, 100, 4000);
    //     servoScript[3].setValues(Spinner, Position, -350, 700, 5000);
    //     servoScript[4].setValues(Spinner, Position, 700, 2000, 3000);
    //     servoScript[5].setValues(Lifter, Position, 790, 100, 2000);
    //     servoScript[6].setValues(Spinner, Position, 350, 400, 5000);
    //     servoScript[7].setValues(Spinner, Home, 0, 0, 1000);
    //     servoScript[8].setValues(Lifter, Position, -15, 200, 12000);
    //     servoEvents = 9;
    // }
    /*****************************************
     * End Test Script
     *****************************************/

    queueFront = (queueFront + 1) % queueCapacity;
    queueSize--;
    scriptLoaded = true;
}

bool AnimationController::scriptIsLoaded()
{

    if (!scriptLoaded && !queueIsEmpty())
    {
        loadNextScript();
    }

    return scriptLoaded;
}

BaseCommand* AnimationController::getNextCommandPtr()
{

    if (scriptEvents.empty())
    {
        return new BaseCommand;
    }
    else if (scriptEvents.size() == 1)
    {

        BaseCommand* lastCmd = scriptEvents.back().toCommandPtr();
        scriptEvents.pop_back();
        
        AnimationController::loadNextScript();

        return lastCmd;
    }
    else
    {
        delayTillNextEvent = scriptEvents.back().duration;

        // 10 milliseconds minimum between events?
        if (delayTillNextEvent < 10)
        {
            delayTillNextEvent = 10;
        }

        BaseCommand* cmd = scriptEvents.back().toCommandPtr();
        scriptEvents.pop_back();
        return cmd;
    }
}

int AnimationController::msTillNextServoCommand()
{
    return delayTillNextEvent;
}

void AnimationController::parseScript(std::string script){
  
    scriptEvents.clear();

    auto start = 0U;
    auto end = script.find(";");
    while (end != std::string::npos)
    {
        AnimationCommand cmd = AnimationCommand(script.substr(start, end - start));
        scriptEvents.push_back(cmd);
        start = end + 1;
        end = script.find(";", start);
    }

    std::reverse(scriptEvents.begin(), scriptEvents.end());
}

