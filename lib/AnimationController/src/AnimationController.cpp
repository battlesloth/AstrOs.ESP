#include <algorithm>
#include <esp_log.h>
#include <esp_system.h>
#include <string.h>

#include <AnimationCommand.hpp>
#include <AnimationCommon.hpp>
#include <AnimationController.hpp>
#include <AstrOsStorageManager.hpp>

static const char *TAG = "AnimationController";

AnimationController AnimationCtrl;

AnimationController::AnimationController()
{
    for (auto &script : this->scriptQueue)
    {
        script = "";
    }
    this->queueFront = 0;
    this->queueRear = -1;
    this->queueSize = 0;

    this->animationMutex = xSemaphoreCreateMutex();
    if (this->animationMutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create animationMutex — controller will be non-functional");
    }
    this->queueing = false;
    this->scriptLoaded = false;
    this->delayTillNextEvent = 0;
}

AnimationController::~AnimationController()
{
    // Guard against a failed xSemaphoreCreateMutex() — vSemaphoreDelete(NULL)
    // traps/asserts on most FreeRTOS configs.
    if (this->animationMutex != NULL)
    {
        vSemaphoreDelete(this->animationMutex);
        this->animationMutex = NULL;
    }
}

void AnimationController::panicStop()
{
    ESP_LOGI(TAG, "Panicing!");

    // CRITICAL SAFETY: signal stop immediately via atomic so the animation
    // timer callback halts command dispatch on its next tick, regardless of
    // whether the mutex below succeeds. This is the guarantee panicStop
    // contractually provides — the droid stops moving even if queue cleanup
    // can't complete.
    this->scriptLoaded = false;

    // Acquire the mutex for the full queue cleanup. On timeout the droid is
    // still halted by the atomic above; stale queue state is harmless and
    // will be cleared on the next successful queueScript.
    if (xSemaphoreTake(this->animationMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "panicStop: mutex timeout — scriptLoaded is false so motion is halted, "
                      "but script queue was not cleared (will be cleared on next queueScript)");
        return;
    }

    for (auto &script : this->scriptQueue)
    {
        script = "";
    }
    this->queueFront = 0;
    this->queueRear = -1;
    this->queueSize = 0;

    this->scriptEvents.clear();
    this->scriptLoaded = false;
    xSemaphoreGive(this->animationMutex);
}

bool AnimationController::queueScript(std::string scriptId)
{
    this->queueing = true;

    ESP_LOGI(TAG, "Queueing %s", scriptId.c_str());

    if (queueIsFull())
    {
        ESP_LOGI(TAG, "Queue is full");
        this->queueing = false;
        return false;
    }

    if (xSemaphoreTake(this->animationMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "queueScript: failed to acquire animationMutex within 5s");
        this->queueing = false;
        return false;
    }

    this->queueRear = (queueRear + 1) % queueCapacity;
    this->scriptQueue[queueRear] = scriptId;
    this->queueSize++;
    this->queueing = false;
    xSemaphoreGive(this->animationMutex);

    return true;
}

bool AnimationController::queueCommand(std::string command)
{
    if (xSemaphoreTake(this->animationMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "queueCommand: failed to acquire animationMutex within 5s");
        return false;
    }

    AnimationCommand cmd = AnimationCommand(command);
    this->scriptEvents.push_back(cmd);
    xSemaphoreGive(this->animationMutex);
    return true;
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

    if (xSemaphoreTake(this->animationMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "loadNextScript: failed to acquire animationMutex within 5s");
        this->scriptLoaded = false;
        return;
    }

    ESP_LOGI(TAG, "Loading script %s", this->scriptQueue[this->queueFront].c_str());

    std::string path = "scripts/" + this->scriptQueue[this->queueFront];

    script = AstrOs_Storage.readFile(path);

    this->queueFront = (this->queueFront + 1) % this->queueCapacity;
    this->queueSize--;
    xSemaphoreGive(this->animationMutex);

    if (script == "error")
    {
        ESP_LOGI(TAG, "Script not loaded");
        this->scriptLoaded = false;
    }
    else
    {
        // parseScript owns the scriptLoaded flag: it sets true on successful
        // population (inside its mutex block) and false on mutex timeout.
        // Do not override here.
        AnimationController::parseScript(script);
    }
}

bool AnimationController::scriptIsLoaded()
{

    if (!scriptLoaded && !queueIsEmpty())
    {
        loadNextScript();
    }

    return scriptLoaded;
}

std::unique_ptr<CommandTemplate> AnimationController::getNextCommandPtr()
{
    if (xSemaphoreTake(this->animationMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
    {
        // SAFETY: halt the animation sequence on mutex timeout. If we just returned
        // nullptr, the next timer tick would call us again and — if the mutex had
        // cleared — dispatch the NEXT command, silently skipping the one we missed.
        // For sequenced operations (e.g. "open panel then extend greeblie") that
        // can damage the droid. Setting scriptLoaded=false stops the callback from
        // retrieving any further commands; a new queueScript is required to resume.
        ESP_LOGE(TAG, "getNextCommandPtr: mutex timeout — halting script to prevent partial sequence execution");
        this->scriptLoaded = false;
        return nullptr;
    }

    std::unique_ptr<CommandTemplate> cmd;

    if (this->scriptEvents.empty())
    {
        this->scriptLoaded = false;
        cmd = std::make_unique<CommandTemplate>(MODULE_TYPE::NONE, 0, "");
    }
    else if (this->scriptEvents.size() == 1)
    {
        auto lastCmd = scriptEvents.back().GetCommandTemplatePtr();
        this->scriptEvents.pop_back();

        this->scriptLoaded = false;
        cmd = std::move(lastCmd);
    }
    else
    {
        this->delayTillNextEvent = scriptEvents.back().duration;

        // 10 milliseconds minimum between events
        if (this->delayTillNextEvent < 10)
        {
            this->delayTillNextEvent = 10;
        }

        cmd = scriptEvents.back().GetCommandTemplatePtr();
        this->scriptEvents.pop_back();
    }

    xSemaphoreGive(this->animationMutex);
    return cmd;
}

int AnimationController::msTillNextServoCommand()
{
    return this->delayTillNextEvent;
}

void AnimationController::parseScript(std::string script)
{
    if (xSemaphoreTake(this->animationMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
    {
        // SAFETY: on mutex timeout scriptEvents is stale/empty. Force
        // scriptLoaded=false so the caller (loadNextScript) and subsequent
        // timer callbacks do not attempt to dispatch from invalid state.
        ESP_LOGE(TAG, "parseScript: mutex timeout — script not loaded, scriptLoaded=false");
        this->scriptLoaded = false;
        return;
    }

    this->scriptEvents.clear();

    auto parts = AstrOsStringUtils::splitString(script, ';');

    for (auto part : parts)
    {
        if (part.empty())
        {
            continue;
        }
        AnimationCommand cmd = AnimationCommand(part);
        this->scriptEvents.push_back(cmd);
    }

    std::reverse(this->scriptEvents.begin(), this->scriptEvents.end());

    ESP_LOGI(TAG, "Loaded: %s", script.c_str());
    ESP_LOGI(TAG, "Events loaded: %d", this->scriptEvents.size());

    // parseScript owns the scriptLoaded flag: only mark the script as loaded
    // when events are actually populated and the mutex was held throughout.
    // loadNextScript no longer sets scriptLoaded after calling us.
    this->scriptLoaded = true;
    xSemaphoreGive(this->animationMutex);
}
