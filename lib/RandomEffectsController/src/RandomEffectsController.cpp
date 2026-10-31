#include "RandomEffectsController.hpp"

#include <AstrOsStructs.h>
#include <GpioModule.hpp>
#include <RandomCommands.hpp>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

static const char *TAG = "RandomEffects";

RandomEffectsController RandomEffectsCtrl;

namespace
{
    void logSkip(const char *reason, const char *raw)
    {
        ESP_LOGW(TAG, "Random span skipped (%s): %s", reason, raw);
    }
} // namespace

RandomEffectsController::RandomEffectsController() : table_(TABLE_CAPACITY, esp_random), dropLog_(TABLE_CAPACITY)
{
    changes_.reserve(TABLE_CAPACITY + 1);
}

bool RandomEffectsController::Init(const random_effects_config_t &config)
{
    if (config.servoQueue == nullptr || config.gpioQueue == nullptr || config.readMaestroChannel == nullptr)
    {
        ESP_LOGE(TAG, "Init: servoQueue, gpioQueue and readMaestroChannel are all required");
        return false;
    }
    config_ = config;

    queue_ = xQueueCreate(QUEUE_LENGTH, sizeof(QueuedSpan));
    tableMutex_ = xSemaphoreCreateMutex();
    if (queue_ == nullptr || tableMutex_ == nullptr)
    {
        ESP_LOGE(TAG, "Init: failed to create the random span queue / table mutex");
        return false;
    }
    return true;
}

void RandomEffectsController::TaskEntry(void *arg)
{
    (void)arg;
    RandomEffectsCtrl.run();
}

uint32_t RandomEffectsController::NowMs()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

uint32_t RandomEffectsController::MallocFailureCount() const
{
    return mallocFailures_.load(std::memory_order_relaxed);
}

void RandomEffectsController::Submit(MODULE_TYPE type, const std::string &command, uint32_t dispatchMs)
{
    QueuedSpan msg;
    msg.type = type;
    msg.dispatchTickMs = dispatchMs;
    msg.data = (char *)malloc(command.size() + 1);
    if (msg.data == NULL)
    {
        ESP_LOGE(TAG, "Malloc random span dispatch data fail");
        mallocFailures_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    memcpy(msg.data, command.c_str(), command.size());
    msg.data[command.size()] = '\0';

    if (xQueueSend(queue_, &msg, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send random queue fail, span skipped: %s", command.c_str());
        free(msg.data);
    }
}

// The fence and the queue drain share Clear()'s hold, so neither a queued span
// nor one the task is already holding can start after this returns.
bool RandomEffectsController::StopAll(const char *reason, TickType_t wait)
{
    if (xSemaphoreTake(tableMutex_, wait) != pdTRUE)
    {
        return false;
    }

    size_t active = table_.ActiveCount();
    table_.Clear();
    clearFence_.Mark(NowMs());

    QueuedSpan pending;
    int dropped = 0;
    while (xQueueReceive(queue_, &pending, 0) == pdTRUE)
    {
        free(pending.data);
        dropped++;
    }

    xSemaphoreGive(tableMutex_);

    ESP_LOGI(TAG, "%s: stopped %u random span(s), dropped %d queued", reason, static_cast<unsigned>(active), dropped);
    return true;
}

// The span checks that need no table lock: parse, read the channel, build the
// SpanStart. Logs and returns false on any reject. Never called with
// tableMutex_ held: the Maestro read can wait ~1 s on maestroModulesMutex.
bool RandomEffectsController::prepareSpan(const QueuedSpan &msg, SpanStart *out)
{
    char reason[160];
    SpanBuildResult built{};
    int module = 0;
    int channel = 0;
    int spanMs = 0;
    MaestroChannelSnapshot ch{};

    if (msg.type == MODULE_TYPE::MAESTRO_RANDOM)
    {
        auto parsed = ParseRandomMaestroCommand(msg.data);
        if (parsed.error != RandomParseError::NONE)
        {
            snprintf(reason, sizeof(reason), "parse %s", RandomParseErrorName(parsed.error));
            logSkip(reason, msg.data);
            return false;
        }
        module = parsed.cmd.module;
        channel = parsed.cmd.channel;
        spanMs = parsed.cmd.spanMs;

        switch (config_.readMaestroChannel(module, channel, &ch))
        {
        case MaestroChannelRead::OK:
            break;
        case MaestroChannelRead::MODULES_BUSY:
            logSkip("maestroModulesMutex timeout", msg.data);
            return false;
        case MaestroChannelRead::NO_MODULE:
            snprintf(reason, sizeof(reason), "unknown Maestro module %d", module);
            logSkip(reason, msg.data);
            return false;
        case MaestroChannelRead::NO_CHANNEL:
            snprintf(reason, sizeof(reason), "Maestro %d ch %d not read: out of range, or the module is busy", module,
                     channel);
            logSkip(reason, msg.data);
            return false;
        }

        built = BuildMaestroSpan(parsed.cmd, ch, msg.dispatchTickMs);
        if (built.servoFieldsOnDigital)
        {
            ESP_LOGW(TAG,
                     "Random span config mismatch: Maestro %d ch %d is digital but the span has servo fields (the "
                     "server thinks it is a servo); running it as a toggle, re-sync the controller config: %s",
                     module, channel, msg.data);
        }
    }
    else if (msg.type == MODULE_TYPE::GPIO_RANDOM)
    {
        auto parsed = ParseRandomGpioCommand(msg.data);
        if (parsed.error != RandomParseError::NONE)
        {
            snprintf(reason, sizeof(reason), "parse %s", RandomParseErrorName(parsed.error));
            logSkip(reason, msg.data);
            return false;
        }
        channel = parsed.cmd.channel;
        spanMs = parsed.cmd.spanMs;

        bool level = false;
        if (!GpioMod.GetLevel(channel, &level))
        {
            snprintf(reason, sizeof(reason), "unknown GPIO channel %d", channel);
            logSkip(reason, msg.data);
            return false;
        }

        built = BuildGpioSpan(parsed.cmd, level, msg.dispatchTickMs);
    }
    else
    {
        snprintf(reason, sizeof(reason), "unexpected command type %d", static_cast<int>(msg.type));
        logSkip(reason, msg.data);
        return false;
    }

    switch (built.reject)
    {
    case SpanReject::NONE:
        *out = built.start;
        return true;
    case SpanReject::DISABLED_CHANNEL:
        snprintf(reason, sizeof(reason), "Maestro %d ch %d is disabled", module, channel);
        break;
    case SpanReject::NO_USABLE_RANGE:
        snprintf(reason, sizeof(reason),
                 "Maestro %d ch %d has no usable range (min %d us >= max %d us), fix the channel config", module,
                 channel, ch.minPos, ch.maxPos);
        break;
    case SpanReject::DIGITAL_SPAN_ON_SERVO:
        snprintf(reason, sizeof(reason),
                 "config mismatch: the server sent a digital span for servo channel Maestro %d ch %d; re-sync the "
                 "controller config",
                 module, channel);
        break;
    case SpanReject::INVALID_SERVO_FIELDS:
        snprintf(reason, sizeof(reason), "invalid servo fields for Maestro %d ch %d", module, channel);
        break;
    case SpanReject::WITHIN_END_GUARD:
        snprintf(reason, sizeof(reason), "%d ms is within the %d ms end guard, no change can run", spanMs,
                 RandomSpanTable::END_GUARD_MS);
        break;
    }
    logSkip(reason, msg.data);
    return false;
}

// Under tableMutex_, with the hold's one `now`. Collect may append to changes_
// (when it has to free a slot); postChanges sends them.
void RandomEffectsController::startSpan(const QueuedSpan &msg, const SpanStart &span, uint32_t now)
{
    char reason[96];

    if (clearFence_.Fences(msg.dispatchTickMs))
    {
        logSkip("dispatched before a panic or config reload stopped the spans", msg.data);
        return;
    }
    if (SpanArrivedTooLate(msg.dispatchTickMs, span.spanMs, now))
    {
        snprintf(reason, sizeof(reason), "arrived %" PRIu32 " ms after dispatch, past the %d ms span's last change",
                 now - msg.dispatchTickMs, span.spanMs);
        logSkip(reason, msg.data);
        return;
    }

    bool started = table_.Start(span);
    if (!started && table_.ActiveCount() == TABLE_CAPACITY)
    {
        // A full slot may hold a span already inside its end guard; Collect drops it.
        table_.Collect(now, changes_);
        started = table_.Start(span);
    }
    if (!started)
    {
        if (table_.ActiveCount() == TABLE_CAPACITY)
        {
            snprintf(reason, sizeof(reason), "table full (%u/%u active)", static_cast<unsigned>(table_.ActiveCount()),
                     static_cast<unsigned>(TABLE_CAPACITY));
        }
        else
        {
            // The parser accepted these gaps, so the table's copy of the rules
            // disagrees with it (T-008 pins them together).
            snprintf(reason, sizeof(reason), "gap rules rejected");
        }
        logSkip(reason, msg.data);
        return;
    }

    dropLog_.Reset(span.key);

    if (span.key.type == MODULE_TYPE::GPIO_RANDOM)
    {
        ESP_LOGI(TAG, "Random span started: GPIO ch %d, %d ms, gaps %d-%d ms", span.key.channel, span.spanMs,
                 span.minGapMs, span.maxGapMs);
    }
    else if (span.isServo)
    {
        ESP_LOGI(TAG, "Random span started: Maestro %d ch %d servo, %d ms, gaps %d-%d ms, speed %d, accel %d",
                 span.key.module, span.key.channel, span.spanMs, span.minGapMs, span.maxGapMs, span.speed,
                 span.acceleration);
    }
    else
    {
        ESP_LOGI(TAG, "Random span started: Maestro %d ch %d digital, %d ms, gaps %d-%d ms", span.key.module,
                 span.key.channel, span.spanMs, span.minGapMs, span.maxGapMs);
    }
}

// Under tableMutex_, so a StopAll that takes the mutex sees no post after its
// Clear(). Each change is re-checked against its span's end guard just before
// sending. Zero-wait sends, and random changes fill at most half of a change
// queue (RandomChangeHasRoom): the rest stays free for the script's own
// commands. A change that doesn't fit is dropped (payload freed here).
void RandomEffectsController::postChanges()
{
    for (const SpanChange &change : changes_)
    {
        // Collect judged these by the hold's `now`. Logging since then can have
        // blocked this task while the dispatch loop queued the span's end-time
        // event, which a late random change must not follow (§ C).
        if (SpanChangeExpired(change, NowMs()))
        {
            ESP_LOGD(TAG, "Random change for ch %d past its span's end guard, not sent", change.key.channel);
            continue;
        }

        char command[SPAN_CHANGE_MAX_LEN];
        size_t len = FormatSpanChange(change, command, sizeof(command));
        if (len == 0)
        {
            ESP_LOGE(TAG, "Random change for type %d not formatted, skipped", change.key.type);
            continue;
        }

        bool isGpio = change.key.type == MODULE_TYPE::GPIO_RANDOM;
        QueueHandle_t queue = isGpio ? config_.gpioQueue : config_.servoQueue;
        bool posted = false;

        if (RandomChangeHasRoom(uxQueueMessagesWaiting(queue), uxQueueSpacesAvailable(queue)))
        {
            queue_msg_t msg;
            msg.message_id = isGpio ? 0 : change.key.module;
            msg.dataSize = len;
            msg.data = (uint8_t *)malloc(len + 1);
            if (msg.data == NULL)
            {
                ESP_LOGE(TAG, "Malloc random change fail");
                mallocFailures_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            memcpy(msg.data, command, len + 1);

            posted = xQueueSend(queue, &msg, 0) == pdTRUE;
            if (!posted)
            {
                free(msg.data);
            }
        }

        if (!posted && dropLog_.ShouldLog(change.key))
        {
            ESP_LOGW(TAG,
                     "Random change dropped, %s half full (the rest is kept for the script): %s (first drop for this "
                     "span; its later changes continue from the dropped value)",
                     isGpio ? "gpioQueue" : "servoQueue", command);
        }
    }
}

// Takes span commands from Submit, runs the table, and posts each due change.
void RandomEffectsController::run()
{
    TickType_t waitTicks = WaitTicksCeil(IDLE_WAIT_MS, portTICK_PERIOD_MS);

    while (1)
    {
        auto highWaterMark = uxTaskGetStackHighWaterMark(NULL);
        if (highWaterMark < 500)
        {
            ESP_LOGW(TAG, "Random Effects Stack HWM: %d", highWaterMark);
        }

        QueuedSpan msg;
        bool haveMsg = xQueueReceive(queue_, &msg, waitTicks) == pdTRUE;

        SpanStart span{};
        bool haveSpan = haveMsg && prepareSpan(msg, &span);

        if (xSemaphoreTake(tableMutex_, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            ESP_LOGW(TAG, "random_effects_task: table mutex timeout, pass skipped");
            if (haveSpan)
            {
                logSkip("dropped, table mutex timeout", msg.data);
            }
            if (haveMsg)
            {
                free(msg.data);
            }
            waitTicks = 1;
            continue;
        }

        // One `now` for the whole hold, sampled after every wait above: with an
        // earlier one, Collect could emit a change past a span's end (§ C).
        uint32_t now = NowMs();
        clearFence_.Expire(now);
        changes_.clear();

        if (haveSpan)
        {
            startSpan(msg, span, now);
        }

        table_.Collect(now, changes_);
        postChanges();

        // Read under the lock: StopAll Clear()s from other tasks.
        uint32_t waitMs = std::min(table_.MsUntilNext(now), IDLE_WAIT_MS);
        waitTicks = WaitTicksCeil(waitMs, portTICK_PERIOD_MS);

        xSemaphoreGive(tableMutex_);

        if (haveMsg)
        {
            free(msg.data);
        }
    }
}
