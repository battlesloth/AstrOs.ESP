#ifndef MAESTROMODULE_HPP
#define MAESTROMODULE_HPP

#include <AstrOsStructs.h>
#include <esp_err.h>
#include <hal/uart_types.h>
#include <string>
// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#define BAUD_RATE_INDICATION 0xAA
#define SET_SERVO_COMMAND 0x84
#define SET_SERVO_SPEED_COMMAND 0x87
#define SET_SERVO_ACCELERATION_COMMAND 0x89
#define GET_SERVO_POSITION_COMMAND 0x90
#define ANY_SERVO_MOVING_STATE_COMMAND 0x93
#define SET_MULTIPLE_SERVOS_COMMAND 0x9F
#define GET_ERROR_COMMAND 0xA1
#define HOME_COMMAND 0xA2

// speed is (.25us/10ms) * n, where n is 0-255
// 0 is no speed limit
// An extended range servo is 500-2500us at 180 degrees
// therefore 11.111...us per degree
// ~(degree/second * .11111) / .25 = speed value
// Speed 1 is ~2.25 degrees/second
// Speed 255 is ~573 degree/second

// acceleration is (0.25us)/(10ms)/(80ms) * n, where n is 0-255
// 0 is no acceleration limit
// An extended range servo is 500-2500us at 180 degrees
// therefore 11.111...us per degree
// ~(degree/second^2 * 11.111) / 312.5 = acceleration value
// Acceleration 1 is ~28 degrees/second^2
// Acceleration 255 is ~7175 degree/second^2

class MaestroModule
{
private:
    // Result of enqueueFrame. It never logs -- CheckServos calls it under
    // stateMutex on the esp_timer task -- so the caller reports the reason.
    enum class EnqueueResult
    {
        Queued,
        QueueFull,
        NoMemory
    };
    // How far setServoPosition got. Frames go out in this order, and the
    // Maestro keeps whatever limit was in force for any frame that did not.
    enum class SendStage
    {
        None = 0,
        LastPos = 1,
        Speed = 2,
        Accel = 3,
        Target = 4
    };

    bool loading;
    int idx;
    int baudRate;

    // Per-instance channel state (config + release tracking). Zero-initialized
    // at construction. LoadConfig() copies the parsed file over entries
    // 0..maxId as whole structs (release tracking there is reset, then
    // HomeServos() re-arms it); entries above the file's highest id -- and all
    // entries if the file is missing or unparseable -- keep prior state on
    // reload. Both run on the boot / RELOAD_CONFIG path. Written by
    // QueueCommand and SetServoPosition on task context (Panic() also writes
    // but has no caller today -- T-004 wires it); read-modify-written by
    // CheckServos() on the esp_timer task.
    //
    // Locking (T-003): every access is under stateMutex. QueueCommand,
    // SetServoPosition and HomeServos each hold `mutex` (the send mutex) once
    // across their state update and all of their frames, taking stateMutex
    // briefly inside -- lock order send -> state, and stateMutex is never held
    // across a *blocking* enqueue. CheckServos runs on the esp_timer task and
    // uses only zero-wait takes and a zero-timeout enqueue (that one is under
    // stateMutex by design); the inverted order there cannot deadlock because
    // a try-take never waits. Panic still writes state and sends separately
    // (no caller; T-004 makes it a proper operation).
    servo_channel channels[24] = {};

    QueueHandle_t serialQueue;
    SemaphoreHandle_t mutex = nullptr;      // send mutex: one hold per operation (T-003)
    SemaphoreHandle_t stateMutex = nullptr; // guards channels[]
    void SendCommand(uint8_t *cmd);
    // Frame builders; the caller holds `mutex`. setServoPosition enqueues up
    // to four frames with a 500 ms wait each and stops at the first drop,
    // returning the last stage that went out (Target = complete); the caller
    // logs with identity and calls reconcileLimits. setServoOff enqueues one
    // frame with `wait` and returns the result -- CheckServos calls it with
    // stateMutex held and wait 0.
    SendStage setServoPosition(uint8_t channel, int ms, int lastPos, int speed, int acceleration);
    // After a partial send, make the tracked speed/accel match what the
    // Maestro is actually using so the release deadline models the real
    // move: a frame that did not go out leaves the previous value in force.
    // Caller holds `mutex`; takes stateMutex briefly.
    void reconcileLimits(int channel, SendStage stage, int oldSpeed, int oldAccel);
    EnqueueResult setServoOff(uint8_t channel, TickType_t wait);
    int getServoPosition(uint8_t channel);
    // Takes `mutex` itself via sendQueueMsg -- never call with `mutex` held.
    void getError();
    // Bounded take of `mutex`: a 100 ms take retried at most 20 times (~2.2 s),
    // then ESP_LOGE and false. The command paths return before touching any
    // channel state when this fails.
    bool takeSendMutex();
    // Takes no lock and never logs: mallocs a copy of cmd and xQueueSends it
    // with `wait`. Nothing is left allocated on failure.
    EnqueueResult enqueueFrame(const uint8_t *cmd, size_t size, TickType_t wait);
    // Single-frame convenience: takeSendMutex -> enqueueFrame(500 ms) -> give.
    void sendQueueMsg(uint8_t cmd[], size_t size);

public:
    MaestroModule(QueueHandle_t queue, int idx, int baud);
    ~MaestroModule();

    // One instance per physical Maestro. A copy would fork the per-instance
    // channel state (channels) and share the FreeRTOS mutex handles.
    MaestroModule(const MaestroModule &) = delete;
    MaestroModule &operator=(const MaestroModule &) = delete;

    // false if a mutex could not be created (heap exhaustion). The constructor
    // cannot fail in this codebase (no exceptions), so the creator must check
    // this before keeping the instance -- every method assumes both handles.
    bool IsValid() const;

    void UpdateConfig(QueueHandle_t queue, int baud);
    void LoadConfig();
    void HomeServos();
    void QueueCommand(uint8_t *cmd);
    void SetServoPosition(int channel, int ms);
    void Panic();
    // periodically check servos to turn them off
    void CheckServos(int msSinceLastCheck);
};

#endif