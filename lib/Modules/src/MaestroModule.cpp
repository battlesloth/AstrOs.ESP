#include "MaestroModule.hpp"

#include <AnimationCommands.hpp>
#include <AstrOsStorageManager.hpp>
#include <AstrOsUtility.h>

#include <AstrOsServoUtils.hpp>
#include <AstrOsUtility.h>
#include <AstrOsUtility_ESP.h>
#include <SerialModule.hpp>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_system.h>
#include <string.h>

static const char *TAG = "MaestroModule";
static const int RX_BUF_SIZE = 1024;

// T-003 lock/queue timing. Command paths (task context) may block on these;
// CheckServos (esp_timer task) never does -- every take there is zero-wait.
static const int SEND_MUTEX_WAIT_MS = 100;
static const int SEND_MUTEX_ATTEMPTS = 20; // ~2.2 s worst case, then ESP_LOGE and the operation returns
static const int STATE_MUTEX_WAIT_MS = 50;
static const int SEND_QUEUE_WAIT_MS = 500;

MaestroModule::MaestroModule(QueueHandle_t serialQueue, int idx, int baudRate)
{

    this->mutex = xSemaphoreCreateMutex();
    if (this->mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    this->stateMutex = xSemaphoreCreateMutex();
    if (this->stateMutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return;
    }

    this->idx = idx;
    this->baudRate = baudRate;
    this->serialQueue = serialQueue;
    this->loading = false;
}

MaestroModule::~MaestroModule() {}

void MaestroModule::UpdateConfig(QueueHandle_t serialQueue, int baudRate)
{
    bool configChanged = false;

    while (!configChanged)
    {
        // don't update config while we are sending commands to servos
        if (xSemaphoreTake(this->mutex, 100 / portTICK_PERIOD_MS))
        {

            ESP_LOGI(TAG, "Updating Maestro module %d config, old buad: %d, new baud %d", this->idx, this->baudRate,
                     baudRate);

            this->serialQueue = serialQueue;
            this->baudRate = baudRate;

            configChanged = true;
            xSemaphoreGive(this->mutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
}

void MaestroModule::LoadConfig()
{
    this->loading = true;

    ESP_LOGI(TAG, "Loading Maestro servos for module %d", this->idx);

    // loadMaestroServos copies straight into channels[], so the file read is
    // under stateMutex as well (a local copy would cost ~1 KB of main-task
    // stack). Boot / RELOAD_CONFIG path only. Meanwhile: CheckServos skips
    // ticks (zero-wait take); slider moves are dropped silently by the
    // `loading` guard in SetServoPosition; a script command waits up to
    // STATE_MUTEX_WAIT_MS and then applies or drops with a WARN.
    if (xSemaphoreTake(this->stateMutex, pdMS_TO_TICKS(STATE_MUTEX_WAIT_MS)) == pdTRUE)
    {
        AstrOs_Storage.loadMaestroServos(this->idx, channels, 24);
        xSemaphoreGive(this->stateMutex);
    }
    else
    {
        ESP_LOGE(TAG, "LoadConfig: state mutex timeout on module %d, config not loaded, homing with previous config",
                 this->idx);
    }

    this->HomeServos();

    this->loading = false;
}

void MaestroModule::QueueCommand(uint8_t *cmd)
{
    ESP_LOGI(TAG, "Queueing servo command => %s", cmd);
    MaestroCommand servoCmd = MaestroCommand(std::string(reinterpret_cast<char *>(cmd)));

    if (servoCmd.channel > 23 || servoCmd.channel < 0)
    {
        ESP_LOGE(TAG, "Invalid channel %d", servoCmd.channel);
        return;
    }

    int ch = servoCmd.channel;

    // One send-mutex hold across the state update and every frame, so a
    // concurrent CheckServos or Panic cannot slip an off between our frames.
    // (Panic's state write is not serialized against us until T-004; the
    // failure direction is a redundant off later, never a stuck servo.)
    if (!this->takeSendMutex())
    {
        ESP_LOGE(TAG, "QueueCommand: dropping command for channel %d on module %d", ch, this->idx);
        return;
    }

    if (xSemaphoreTake(this->stateMutex, pdMS_TO_TICKS(STATE_MUTEX_WAIT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "QueueCommand: state mutex timeout, dropping command for channel %d on module %d", ch, this->idx);
        xSemaphoreGive(this->mutex);
        return;
    }

    // set channel requested position to percentage of max - min taking into account inverted
    int requestPos = servoCmd.position;

    // if it's not a servo it's an on/off GPIO
    if (!channels[ch].isServo)
    {
        channels[ch].requestedPos = requestPos >= 1500 ? 2500 : 500;
    }
    else if (requestPos < 0)
    {
        channels[ch].requestedPos = channels[ch].home;
    }
    else
    {
        if (channels[ch].inverted)
        {
            requestPos = 100 - requestPos;
        }
        channels[ch].requestedPos = GetRelativeRequestedPosition(channels[ch].minPos, channels[ch].maxPos, requestPos);
    }

    channels[ch].currentPos = 0;
    channels[ch].speed = servoCmd.speed;
    channels[ch].acceleration = servoCmd.acceleration;
    channels[ch].on = true;

    // Snapshot what the log and the frames need, then release the state lock
    // before any enqueue (never hold stateMutex across a blocking call).
    int minPos = channels[ch].minPos;
    int maxPos = channels[ch].maxPos;
    int target = channels[ch].requestedPos;
    int lastPos = channels[ch].lastPos;
    bool inverted = channels[ch].inverted;

    xSemaphoreGive(this->stateMutex);

    ESP_LOGI(TAG, "Setting servo %d on module %d (min: %d, max: %d) to %d, cmd: %d. speed: %d. accel: %d. inverted: %d",
             ch, this->idx, minPos, maxPos, target, servoCmd.position, servoCmd.speed, servoCmd.acceleration, inverted);

    if (!this->setServoPosition(ch, target, lastPos, servoCmd.speed, servoCmd.acceleration))
    {
        ESP_LOGE(TAG, "QueueCommand: serial queue full, move for channel %d on module %d incomplete (still tracked on)",
                 ch, this->idx);
    }

    xSemaphoreGive(this->mutex);
}

void MaestroModule::SetServoPosition(int channel, int ms)
{
    if (this->loading)
    {
        return;
    }

    // Validate as int: the caller parses the channel from text, and narrowing
    // to uint8_t before this check would wrap 256 to 0 and -1 to 255.
    if (channel < 0 || channel > 23)
    {
        ESP_LOGE(TAG, "Invalid channel %d", channel);
        return;
    }

    if (!this->takeSendMutex())
    {
        ESP_LOGE(TAG, "SetServoPosition: dropping move for channel %d on module %d", channel, this->idx);
        return;
    }

    if (xSemaphoreTake(this->stateMutex, pdMS_TO_TICKS(STATE_MUTEX_WAIT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "SetServoPosition: state mutex timeout, dropping move for channel %d on module %d", channel,
                 this->idx);
        xSemaphoreGive(this->mutex);
        return;
    }

    // Direct (slider) moves arrive as a stream at full speed. Arm release
    // tracking on every one, exactly as QueueCommand does: each re-arm just
    // resets the clock, so CheckServos turns the servo off 20 s (the floor)
    // after the *last* message and never mid-drag. Deliberately no INFO log
    // here -- it would spam the monitor at drag rate.
    channels[channel].currentPos = 0;
    channels[channel].speed = 0;
    channels[channel].acceleration = 0;
    channels[channel].on = true;

    xSemaphoreGive(this->stateMutex);

    if (!this->setServoPosition(static_cast<uint8_t>(channel), ms, -1, 0, 0))
    {
        // WARN, not ERROR: a saturated slider drag can hit this at message rate,
        // and the next message re-sends everything.
        ESP_LOGW(TAG, "SetServoPosition: serial queue full, move for channel %d on module %d dropped", channel,
                 this->idx);
    }

    xSemaphoreGive(this->mutex);
}

void MaestroModule::Panic()
{
    ESP_LOGI(TAG, "Panic");

    // No caller today; T-004 rewrites this as a proper command-path operation
    // (per-channel 0x84 offs, state cleared only after a successful enqueue).
    // T-003 only puts the state write under stateMutex.
    uint8_t cmd[74] = {};
    cmd[0] = SET_MULTIPLE_SERVOS_COMMAND;
    cmd[1] = 24;

    for (size_t i = 0; i < 24; i++)
    {
        cmd[2 + (i * 3)] = i;
    }

    if (xSemaphoreTake(this->stateMutex, pdMS_TO_TICKS(STATE_MUTEX_WAIT_MS)) == pdTRUE)
    {
        for (size_t i = 0; i < 24; i++)
        {
            channels[i].on = false;
        }
        xSemaphoreGive(this->stateMutex);
    }
    else
    {
        ESP_LOGW(TAG, "Panic: state mutex timeout on module %d", this->idx);
    }

    this->sendQueueMsg(cmd, 74);
}

void MaestroModule::HomeServos()
{
    ESP_LOGI(TAG, "Homing Servos");

    if (!this->takeSendMutex())
    {
        ESP_LOGE(TAG, "HomeServos: send mutex timeout on module %d, servos not homed", this->idx);
        return;
    }

    // Decide and update state for every enabled channel under stateMutex,
    // remembering what to send; enqueue afterwards with the state lock released.
    struct HomeMove
    {
        bool send;
        int ms;
    };
    HomeMove moves[24] = {};

    if (xSemaphoreTake(this->stateMutex, pdMS_TO_TICKS(STATE_MUTEX_WAIT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "HomeServos: state mutex timeout on module %d, servos not homed", this->idx);
        xSemaphoreGive(this->mutex);
        return;
    }

    for (size_t i = 0; i < 24; i++)
    {
        if (!channels[i].enabled)
        {
            continue;
        }

        moves[i].send = true;

        if (!channels[i].isServo)
        {
            moves[i].ms = channels[i].inverted ? 2500 : 500;
        }
        else
        {
            channels[i].requestedPos = channels[i].home;
            channels[i].currentPos = 0;
            channels[i].on = true;
            channels[i].speed = 0;
            channels[i].acceleration = 0;
            channels[i].lastPos = channels[i].home;
            moves[i].ms = channels[i].home;
        }
    }

    xSemaphoreGive(this->stateMutex);

    int failed = 0;
    for (size_t i = 0; i < 24; i++)
    {
        if (moves[i].send)
        {
            // lastPos 0 (not -1): homing sends an explicit target-0 frame first,
            // as it always has.
            if (!this->setServoPosition(i, moves[i].ms, 0, 0, 0))
            {
                failed++;
            }
        }
    }

    if (failed > 0)
    {
        ESP_LOGE(TAG, "HomeServos: serial queue full on module %d, %d channel(s) not homed (still tracked on)",
                 this->idx, failed);
    }

    xSemaphoreGive(this->mutex);
}

/// @brief Since we can't really know where the servo is actually at
/// and we don't want to keep the servo on all the time, we accumulate
/// elapsed time per channel and turn the servo off once it has had
/// enough time to cover its entire range (worst case, with slack).
/// While a move is active, currentPos serves as the elapsed-ms
/// accumulator; the deadline comes from ServoReleaseDeadlineMs
/// (lib_native/AstrOsUtility/AstrOsServoUtils.hpp): the physical
/// worst case for the channel's speed/accel, floored at 20 s.
///
/// Runs on the esp_timer task, so nothing here may block: stateMutex and
/// the send mutex are try-taken with zero wait and the off frame is
/// enqueued with zero timeout. Any failure leaves the channel on and it
/// is re-checked next tick. The decision and the enqueue happen under
/// stateMutex so a new command cannot land its target between them.
/// Outcomes are logged only after stateMutex is released: a console line
/// is a multi-ms blocking write, and every channel homed together comes
/// due on the same tick (they all hit the 20 s floor), so logging inside
/// the lock would hold it past the command paths' 50 ms wait.
/// @param msSinceLastCheck The time since the last check in milliseconds
void MaestroModule::CheckServos(int msSinceLastCheck)
{
    if (xSemaphoreTake(this->stateMutex, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "CheckServos: state busy on module %d, skipping tick", this->idx);
        return;
    }

    uint32_t turnedOff = 0;
    uint32_t sendBusy = 0;
    uint32_t queueFull = 0;

    for (size_t i = 0; i < 24; i++)
    {
        if (!channels[i].isServo || !channels[i].on)
        {
            continue;
        }

        int deadline = ServoReleaseDeadlineMs(channels[i].speed, channels[i].acceleration);

        // Stop accumulating once due, so a wedged send path cannot overflow it.
        if (channels[i].currentPos < deadline)
        {
            channels[i].currentPos += msSinceLastCheck;
        }
        if (channels[i].currentPos < deadline)
        {
            continue;
        }

        if (xSemaphoreTake(this->mutex, 0) != pdTRUE)
        {
            sendBusy |= 1u << i;
            continue;
        }

        bool queued = this->setServoOff(i, 0);
        xSemaphoreGive(this->mutex);

        if (queued)
        {
            channels[i].on = false;
            channels[i].currentPos = 0;
            turnedOff |= 1u << i;
        }
        else
        {
            queueFull |= 1u << i;
        }
    }

    xSemaphoreGive(this->stateMutex);

    for (size_t i = 0; i < 24; i++)
    {
        if (turnedOff & (1u << i))
        {
            ESP_LOGI(TAG, "Turning off servo %d on module %d", i, this->idx);
        }
    }
    if (sendBusy != 0)
    {
        ESP_LOGW(TAG, "CheckServos: send busy on module %d, channels 0x%06X retry next tick", this->idx,
                 (unsigned)sendBusy);
    }
    if (queueFull != 0)
    {
        ESP_LOGW(TAG, "CheckServos: serial queue full on module %d, channels 0x%06X retry next tick", this->idx,
                 (unsigned)queueFull);
    }
}

bool MaestroModule::setServoPosition(uint8_t channel, int ms, int lastpos, int speed, int acceleration)
{
    uint8_t cmd[4] = {};
    const TickType_t wait = pdMS_TO_TICKS(SEND_QUEUE_WAIT_MS);

    cmd[1] = channel;

    // Stop at the first dropped frame: a delivered target after a dropped
    // speed frame would move at the Maestro's previous speed while the
    // release deadline assumes the new one.

    // we need to send the last requested position
    // before we send speed/accel commands if the servo
    // was set to off as these commands will not work
    // if they happen before the servo is turned on
    if (lastpos != -1)
    {
        cmd[0] = SET_SERVO_COMMAND;
        cmd[2] = lastpos & 0x7F;
        cmd[3] = (lastpos >> 7) & 0x7F;

        if (!this->enqueueFrame(cmd, 4, wait))
        {
            return false;
        }
    }

    cmd[0] = SET_SERVO_SPEED_COMMAND;
    cmd[2] = speed & 0x7F;
    cmd[3] = (speed >> 7) & 0x7F;

    if (!this->enqueueFrame(cmd, 4, wait))
    {
        return false;
    }

    cmd[0] = SET_SERVO_ACCELERATION_COMMAND;
    cmd[2] = acceleration & 0x7F;
    cmd[3] = (acceleration >> 7) & 0x7F;

    if (!this->enqueueFrame(cmd, 4, wait))
    {
        return false;
    }

    // .25us resolution
    int target = ms * 4;

    cmd[0] = SET_SERVO_COMMAND;
    cmd[2] = target & 0x7F;
    cmd[3] = (target >> 7) & 0x7F;

    if (!this->enqueueFrame(cmd, 4, wait))
    {
        return false;
    }

    ESP_LOGD(TAG, "command sent to channel: %d", channel);
    return true;
}

bool MaestroModule::setServoOff(uint8_t channel, TickType_t wait)
{
    uint8_t cmd[4] = {};

    cmd[0] = SET_SERVO_COMMAND;
    cmd[1] = channel;
    cmd[2] = 0x00;
    cmd[3] = 0x00;

    return this->enqueueFrame(cmd, 4, wait);
}

void MaestroModule::getError()
{
    uint8_t cmd[1] = {};

    cmd[0] = GET_ERROR_COMMAND;

    this->sendQueueMsg(cmd, 1);
}

bool MaestroModule::takeSendMutex()
{
    for (int attempt = 0; attempt < SEND_MUTEX_ATTEMPTS; attempt++)
    {
        if (xSemaphoreTake(this->mutex, pdMS_TO_TICKS(SEND_MUTEX_WAIT_MS)) == pdTRUE)
        {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // wait a bit before retrying
    }

    ESP_LOGE(TAG, "Send mutex timeout on module %d after %d attempts", this->idx, SEND_MUTEX_ATTEMPTS);
    return false;
}

bool MaestroModule::enqueueFrame(const uint8_t *cmd, size_t size, TickType_t wait)
{
    queue_serial_msg_t msg;

    msg.message_id = 1;
    msg.baudrate = this->baudRate;
    msg.data = (uint8_t *)malloc(size);
    if (msg.data == NULL)
    {
        ESP_LOGE(TAG, "enqueueFrame: out of memory on module %d", this->idx);
        return false;
    }
    memcpy(msg.data, cmd, size);
    msg.dataSize = size;

    if (xQueueSend(this->serialQueue, &msg, wait) != pdTRUE)
    {
        // No log here: callers report the drop with channel/module identity
        // (and CheckServos does so outside its lock).
        free(msg.data);
        return false;
    }

    return true;
}

void MaestroModule::sendQueueMsg(uint8_t cmd[], size_t size)
{
    if (!this->takeSendMutex())
    {
        ESP_LOGE(TAG, "sendQueueMsg: frame dropped on module %d", this->idx);
        return;
    }

    if (!this->enqueueFrame(cmd, size, pdMS_TO_TICKS(SEND_QUEUE_WAIT_MS)))
    {
        ESP_LOGW(TAG, "sendQueueMsg: frame 0x%02X dropped on module %d, serial queue full", cmd[0], this->idx);
    }

    xSemaphoreGive(this->mutex);
}
