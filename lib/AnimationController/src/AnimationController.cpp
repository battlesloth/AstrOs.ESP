#include <string.h>
#include <esp_log.h>
#include <algorithm>
#include <esp_system.h>

#include <AnimationController.hpp>
#include <AnimationCommon.hpp>
#include <AnimationCommand.hpp>
#include <AstrOsStorageManager.hpp>

static const char *TAG = "AnimationController";

AnimationController AnimationCtrl;

AnimationController::AnimationController()
{      
    for(auto &script : this->scriptQueue)
    {
        script = "";
    }
    this->queueFront = 0;
    this->queueRear = -1;
    this->queueSize = 0;
 
    this->animationMutex = xSemaphoreCreateMutex();
    this->queueing = false;
}

AnimationController::~AnimationController() {}

void AnimationController::panicStop()
{
    ESP_LOGI(TAG, "Panicing!");
    auto cleared = false;

    while (!cleared)
    {
        if (xSemaphoreTake(this->animationMutex, portMAX_DELAY) == pdTRUE)
        {
            for(auto &script : this->scriptQueue)
            {
                script = "";
            }
            this->queueFront = 0;
            this->queueRear = -1;
            this->queueSize = 0;
         
            this->scriptEvents.clear();
            this->scriptLoaded = false;
            cleared = true;
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    // TODO
}

bool AnimationController::queueScript(std::string scriptId)
{
    this->queueing = true;

    ESP_LOGI(TAG, "Queueing %s", scriptId.c_str());

    if (queueIsFull())
    {
        ESP_LOGI(TAG, "Queue is full");
        return false;
    }

    while (this->queueing)
    {
        if (xSemaphoreTake(this->animationMutex, portMAX_DELAY) == pdTRUE)
        {
            this->queueRear = (queueRear + 1) % queueCapacity;

            this->scriptQueue[queueRear] = scriptId;

            this->queueSize++;
            this->queueing = false;
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    return true;
}

bool AnimationController::queueCommand(std::string command)
{
    auto queued = false;
    while (!queued)
    {
        if (xSemaphoreTake(this->animationMutex, portMAX_DELAY) == pdTRUE)
        {
            AnimationCommand cmd = AnimationCommand(command);
            this->scriptEvents.push_back(cmd);
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return false;
}

bool AnimationController::queueIsFull()
{
    return (this->queueSize == this->queueCapacity);
}

bool AnimationController::queueIsEmpty()
{
    return (this->queueSize == 0);
}

void AnimationController::loadNextScript()
{

    if (queueIsEmpty() || this->queueing)
    {
        this->scriptLoaded = false;
        return;
    }

    std::string script = "error";
    auto retrieved = false;

    while (!retrieved)
    {
        if (xSemaphoreTake(this->animationMutex, portMAX_DELAY) == pdTRUE)
        {
            ESP_LOGI(TAG, "Loading script %s", this->scriptQueue[this->queueFront].c_str());

            std::string path = "scripts/" + this->scriptQueue[this->queueFront];

            script = AstrOs_Storage.readFile(path);

            this->queueFront = (this->queueFront + 1) % this->queueCapacity;
            this->queueSize--;

            retrieved = true;
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (script == "error")
    {
        ESP_LOGI(TAG, "Script not loaded");
        this->scriptLoaded = false;
    }
    else
    {
        AnimationController::parseScript(script);
        this->scriptLoaded = true;
    }
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
}

bool AnimationController::scriptIsLoaded()
{

    if (!scriptLoaded && !queueIsEmpty())
    {
        loadNextScript();
    }

    return scriptLoaded;
}

CommandTemplate *AnimationController::getNextCommandPtr()
{
    CommandTemplate *cmd = nullptr;
    auto retrieved = false;

    while (!retrieved)
    {
        if (xSemaphoreTake(this->animationMutex, portMAX_DELAY) == pdTRUE)
        {

            if (this->scriptEvents.empty())
            {
                this->scriptLoaded = false;
                return new CommandTemplate(MODULE_TYPE::NONE, 0, "");
            }
            else if (this->scriptEvents.size() == 1)
            {

                CommandTemplate *lastCmd = scriptEvents.back().GetCommandTemplatePtr();
                this->scriptEvents.pop_back();

                this->scriptLoaded = false;
                return lastCmd;
            }
            else
            {
                this->delayTillNextEvent = scriptEvents.back().duration;

                // 10 milliseconds minimum between events?
                if (this->delayTillNextEvent < 10)
                {
                    this->delayTillNextEvent = 10;
                }

                cmd = scriptEvents.back().GetCommandTemplatePtr();
                this->scriptEvents.pop_back();
            }
            retrieved = true;
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    return cmd;
}

int AnimationController::msTillNextServoCommand()
{
    return this->delayTillNextEvent;
}

void AnimationController::parseScript(std::string script)
{
    auto parsed = false;

    while (!parsed)
    {
        if (xSemaphoreTake(this->animationMutex, portMAX_DELAY) == pdTRUE)
        {
            this->scriptEvents.clear();

            auto start = 0U;
            auto end = script.find(";");
            while (end != std::string::npos)
            {
                AnimationCommand cmd = AnimationCommand(script.substr(start, end - start));
                this->scriptEvents.push_back(cmd);
                start = end + 1;
                end = script.find(";", start);
            }

            std::reverse(this->scriptEvents.begin(), this->scriptEvents.end());

            ESP_LOGI(TAG, "Loaded: %s", script.c_str());
            ESP_LOGI(TAG, "Events loaded: %d", this->scriptEvents.size());
            parsed = true;
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
