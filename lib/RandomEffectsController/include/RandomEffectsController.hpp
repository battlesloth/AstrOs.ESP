#ifndef RANDOMEFFECTSCONTROLLER_HPP
#define RANDOMEFFECTSCONTROLLER_HPP

#include <AstrOsEnums.h>
#include <RandomEffects.hpp>
#include <RandomSpanAdmission.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// What main.cpp's Maestro lookup found. The controller logs the reason.
enum class MaestroChannelRead
{
    OK,
    MODULES_BUSY, // maestroModulesMutex timeout
    NO_MODULE,
    NO_CHANNEL // out of range, or the module's stateMutex is busy (logged by the module)
};

// Reads one Maestro channel's snapshot. Supplied by main.cpp, which owns the
// Maestro module list.
typedef MaestroChannelRead (*read_maestro_channel_fn)(int module, int channel, MaestroChannelSnapshot *out);

// What main.cpp owns and the controller needs.
typedef struct
{
    QueueHandle_t servoQueue;
    QueueHandle_t gpioQueue;
    read_maestro_channel_fn readMaestroChannel;
} random_effects_config_t;

// Random span events (.docs/protocol.md § C, T-009). One instance,
// RandomEffectsCtrl. Threading and queue ownership: see the README.
class RandomEffectsController
{
public:
    RandomEffectsController();

    // A copy would fork the span schedule and share the FreeRTOS handles.
    RandomEffectsController(const RandomEffectsController &) = delete;
    RandomEffectsController &operator=(const RandomEffectsController &) = delete;

    // Two-step construction: the global is built at static-init time, before
    // FreeRTOS objects exist. Creates the queue and the table mutex. False when
    // either can't be created or the config is incomplete; init() aborts.
    bool Init(const random_effects_config_t &config);

    // random_effects_task's entry point (arg unused). Never returns.
    static void TaskEntry(void *arg);

    // Called by the dispatch loop for command types 6 and 7. Copies command and
    // queues it, waiting at most 100 ms; on failure logs and frees the copy.
    // dispatchMs comes from NowMs(), taken before the command was fetched.
    void Submit(MODULE_TYPE type, const std::string &command, uint32_t dispatchMs);

    // Panic and config reload: stops every span, drains the queue and fences
    // spans dispatched before now, all in one hold of the table mutex. False on
    // a mutex timeout (nothing stopped); the caller logs it.
    bool StopAll(const char *reason, TickType_t wait);

    // The clock the dispatch stamp and the task share: esp_timer ms, wrapping.
    static uint32_t NowMs();

    // Failed mallocs (dispatch copies and change strings), for the maintenance
    // timer's error-counter line.
    uint32_t MallocFailureCount() const;

private:
    // Queued from Submit to the task. data is the malloc'd command string.
    struct QueuedSpan
    {
        MODULE_TYPE type;
        uint32_t dispatchTickMs;
        char *data;
    };

    static constexpr size_t TABLE_CAPACITY = 32;
    static constexpr UBaseType_t QUEUE_LENGTH = 10;
    static constexpr uint32_t IDLE_WAIT_MS = 250;

    void run();
    bool prepareSpan(const QueuedSpan &msg, SpanStart *out);
    void startSpan(const QueuedSpan &msg, const SpanStart &span, uint32_t now);
    void postChanges();

    random_effects_config_t config_{};
    QueueHandle_t queue_ = nullptr;
    SemaphoreHandle_t tableMutex_ = nullptr;

    // Under tableMutex_.
    RandomSpanTable table_;
    ClearFence clearFence_;

    // random_effects_task only. Collect emits at most one change per slot; when
    // a full table forces startSpan's extra Collect, that pass can emit for
    // every slot and the new span then adds its first change, hence the
    // reserve of TABLE_CAPACITY + 1. The loop never allocates.
    std::vector<SpanChange> changes_;
    DropLogThrottle dropLog_;

    std::atomic<uint32_t> mallocFailures_{0};
};

extern RandomEffectsController RandomEffectsCtrl;

#endif
