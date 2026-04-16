#include <esp_log.h>
#include <esp_system.h>
#include <string.h>

#include <AnimationCommand.hpp>
#include <AnimationController.hpp>
#include <AstrOsAnimationEngine.hpp>
#include <AstrOsStorageManager.hpp>

static const char *TAG = "AnimationController";

AnimationController AnimationCtrl;

AnimationController::AnimationController()
{
    this->animationMutex = xSemaphoreCreateMutex();
    if (this->animationMutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create animationMutex — controller will be non-functional");
    }
    this->queueing.store(false);
    this->scriptLoaded.store(false);
    this->delayTillNextEvent.store(0);
}

AnimationController::~AnimationController()
{
    if (this->animationMutex != NULL)
    {
        vSemaphoreDelete(this->animationMutex);
        this->animationMutex = NULL;
    }
}

void AnimationController::panicStop()
{
    ESP_LOGI(TAG, "Panicing!");

    this->scriptLoaded.store(false);

    if (xSemaphoreTake(this->animationMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "panicStop: mutex timeout — scriptLoaded is false so motion is halted, "
                      "but script queue was not cleared (will be cleared on next queueScript)");
        return;
    }

    this->scriptQueue_.clear();
    this->scriptEvents.clear();
    this->scriptLoaded.store(false);
    xSemaphoreGive(this->animationMutex);
}

bool AnimationController::queueScript(std::string scriptId)
{
    this->queueing.store(true);

    ESP_LOGI(TAG, "Queueing %s", scriptId.c_str());

    if (xSemaphoreTake(this->animationMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "queueScript: failed to acquire animationMutex within 5s");
        this->queueing.store(false);
        return false;
    }

    if (!this->scriptQueue_.push(scriptId))
    {
        ESP_LOGI(TAG, "Queue is full");
        xSemaphoreGive(this->animationMutex);
        this->queueing.store(false);
        return false;
    }

    this->queueing.store(false);
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
    this->scriptLoaded.store(true);
    this->delayTillNextEvent.store(0);
    xSemaphoreGive(this->animationMutex);
    return true;
}

void AnimationController::loadNextScript()
{
    if (this->queueing.load())
    {
        this->scriptLoaded.store(false);
        return;
    }

    if (xSemaphoreTake(this->animationMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "loadNextScript: failed to acquire animationMutex within 5s");
        this->scriptLoaded.store(false);
        return;
    }

    if (this->scriptQueue_.isEmpty())
    {
        this->scriptLoaded.store(false);
        xSemaphoreGive(this->animationMutex);
        return;
    }

    std::string scriptId = this->scriptQueue_.pop();
    xSemaphoreGive(this->animationMutex);

    ESP_LOGI(TAG, "Loading script %s", scriptId.c_str());

    std::string path = "scripts/" + scriptId;
    std::string script = AstrOs_Storage.readFile(path);

    if (script == "error")
    {
        ESP_LOGI(TAG, "Script not loaded");
        this->scriptLoaded.store(false);
        return;
    }

    if (xSemaphoreTake(this->animationMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "loadNextScript: mutex timeout after file read — script not loaded");
        this->scriptLoaded.store(false);
        return;
    }

    this->scriptEvents = AstrOsAnimationEngine::parseAnimationScript(script);

    ESP_LOGI(TAG, "Loaded: %s", script.c_str());
    ESP_LOGI(TAG, "Events loaded: %zu", this->scriptEvents.size());

    this->scriptLoaded.store(!this->scriptEvents.empty());
    xSemaphoreGive(this->animationMutex);
}

bool AnimationController::scriptIsLoaded()
{
    if (!scriptLoaded.load())
    {
        loadNextScript();
    }
    return scriptLoaded.load();
}

std::unique_ptr<CommandTemplate> AnimationController::getNextCommandPtr()
{
    if (xSemaphoreTake(this->animationMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "getNextCommandPtr: mutex timeout — halting script to prevent partial sequence execution");
        this->scriptLoaded.store(false);
        return nullptr;
    }

    auto result = AstrOsAnimationEngine::getNextCommand(this->scriptEvents);

    if (result.scriptDone)
    {
        this->scriptLoaded.store(false);
    }
    else
    {
        this->delayTillNextEvent.store(result.delayMs);
    }

    xSemaphoreGive(this->animationMutex);
    return std::move(result.command);
}

int AnimationController::msTillNextServoCommand()
{
    return this->delayTillNextEvent.load();
}
