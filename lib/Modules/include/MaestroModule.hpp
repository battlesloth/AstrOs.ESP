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
    // Locking (T-003): every access is under stateMutex. Command-path
    // operations hold `mutex` (the send mutex) once across their state update
    // and all of their frames, taking stateMutex briefly inside -- lock order
    // send -> state, and stateMutex is never held across an enqueue.
    // CheckServos runs on the esp_timer task and uses only zero-wait takes and
    // a zero-timeout enqueue; the inverted order there cannot deadlock because
    // a try-take never waits.
    servo_channel channels[24] = {};

    QueueHandle_t serialQueue;
    SemaphoreHandle_t mutex;      // send mutex: one hold per operation (T-003)
    SemaphoreHandle_t stateMutex; // guards channels[]
    void SendCommand(uint8_t *cmd);
    // Frame builders. The caller holds `mutex`; frames are enqueued with the
    // given wait and stateMutex released.
    void setServoPosition(uint8_t channel, int ms, int lastPos, int speed, int acceleration);
    bool setServoOff(uint8_t channel, TickType_t wait);
    int getServoPosition(uint8_t channel);
    void getError();
    // Bounded take of `mutex` (retries a 100 ms take at most 20 times). false
    // after ESP_LOGE; callers abort before touching channel state.
    bool takeSendMutex();
    // Lock-free: mallocs a copy of cmd and xQueueSends it with `wait`. false
    // (payload freed) if the serial queue stays full.
    bool enqueueFrame(const uint8_t *cmd, size_t size, TickType_t wait);
    // Single-frame convenience: takeSendMutex -> enqueueFrame(500 ms) -> give.
    void sendQueueMsg(uint8_t cmd[], size_t size);

public:
    MaestroModule(QueueHandle_t queue, int idx, int baud);
    ~MaestroModule();

    // One instance per physical Maestro. A copy would fork the per-instance
    // channel state (channels) and share the FreeRTOS mutex handles.
    MaestroModule(const MaestroModule &) = delete;
    MaestroModule &operator=(const MaestroModule &) = delete;

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