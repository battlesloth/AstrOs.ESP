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

    this->idx = idx;
    this->baudRate = baudRate;
    this->serialQueue = serialQueue;
    this->loading = false;

    // Either handle missing => IsValid() is false and the creator must drop
    // this instance (loadMaestroConfigs does). Nothing else guards the handles.
    this->mutex = xSemaphoreCreateMutex();
    if (this->mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create send mutex for module %d", idx);
        return;
    }

    this->stateMutex = xSemaphoreCreateMutex();
    if (this->stateMutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create state mutex for module %d", idx);
        vSemaphoreDelete(this->mutex);
        this->mutex = nullptr;
        return;
    }
}

MaestroModule::~MaestroModule()
{
    // Runs when the last shared_ptr drops, so no task can be blocked on
    // these handles (a blocked task would hold a shared_ptr).
    if (this->stateMutex != nullptr)
    {
        vSemaphoreDelete(this->stateMutex);
    }
    if (this->mutex != nullptr)
    {
        vSemaphoreDelete(this->mutex);
    }
}

bool MaestroModule::IsValid() const
{
    return this->mutex != nullptr && this->stateMutex != nullptr;
}

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
    // STATE_MUTEX_WAIT_MS and then applies or drops with a WARN; Panic waits
    // the same and then sends its offs from an unlocked read of `enabled`.
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

    // Limits the Maestro currently holds for this channel; restored by
    // reconcileLimits for any frame that fails to go out.
    int oldSpeed = channels[ch].speed;
    int oldAccel = channels[ch].acceleration;

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

    SendStage stage = this->setServoPosition(ch, target, lastPos, servoCmd.speed, servoCmd.acceleration);
    if (stage != SendStage::Target)
    {
        ESP_LOGE(TAG,
                 "QueueCommand: frame not queued (serial queue full or no memory) after stage %d of 4, move for "
                 "channel %d on module %d incomplete (still tracked on)",
                 static_cast<int>(stage), ch, this->idx);
        this->reconcileLimits(ch, stage, oldSpeed, oldAccel);
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
    int oldSpeed = channels[channel].speed;
    int oldAccel = channels[channel].acceleration;

    channels[channel].currentPos = 0;
    channels[channel].speed = 0;
    channels[channel].acceleration = 0;
    channels[channel].on = true;

    xSemaphoreGive(this->stateMutex);

    SendStage stage = this->setServoPosition(static_cast<uint8_t>(channel), ms, -1, 0, 0);
    if (stage != SendStage::Target)
    {
        // WARN, not ERROR: a saturated slider drag can hit this at message rate,
        // and the next message re-sends everything. No pre-position frame on
        // this path, so nothing new is moving; reconcile anyway for uniformity.
        ESP_LOGW(TAG,
                 "SetServoPosition: frame not queued (serial queue full or no memory) after stage %d of 3, move for "
                 "channel %d on module %d dropped",
                 static_cast<int>(stage), channel, this->idx);
        this->reconcileLimits(channel, stage, oldSpeed, oldAccel);
    }

    xSemaphoreGive(this->mutex);
}

void MaestroModule::Panic()
{
    // Operator kill-switch. Panic is a STOP, not a reset: every enabled servo
    // channel is de-energized (motion halts, holding torque drops); GPIO-type
    // channels are left exactly as they are, because driving one anywhere --
    // its rest state or target 0 -- is itself a state change that could move
    // something. A T-003 command-path operation -- one send-mutex hold across
    // the state read, every off frame, and the state clear -- except that
    // tracking is cleared only AFTER an off was actually queued: the one
    // failure direction that must never happen is a servo left energized
    // while tracking says off. Runs on interfaceResponseQueueTask.
    if (!this->takeSendMutex())
    {
        ESP_LOGE(TAG, "Panic: send mutex timeout on module %d - offs NOT sent", this->idx);
        return;
    }

    bool enabled[24] = {};
    bool haveStateLock = xSemaphoreTake(this->stateMutex, pdMS_TO_TICKS(STATE_MUTEX_WAIT_MS)) == pdTRUE;
    if (!haveStateLock)
    {
        // Reachable during a config reload: LoadConfig holds stateMutex across
        // its SD read on serviceQueueTask. Every other taker (QueueCommand,
        // SetServoPosition, HomeServos, reconcileLimits) runs under the send
        // mutex we already hold, and CheckServos holds it for microseconds.
        // `enabled` is config-only, written by LoadConfig alone and a single
        // byte, so an unlocked read cannot tear: send the offs anyway. The
        // clear below is still attempted after the sends.
        ESP_LOGW(TAG, "Panic: state mutex timeout on module %d, reading config unlocked and sending offs anyway",
                 this->idx);
    }
    for (size_t i = 0; i < 24; i++)
    {
        enabled[i] = channels[i].enabled && channels[i].isServo;
    }
    if (haveStateLock)
    {
        xSemaphoreGive(this->stateMutex);
    }

    bool queued[24] = {};
    int queuedCount = 0;
    int failedCount = 0;
    for (size_t i = 0; i < 24; i++)
    {
        if (!enabled[i])
        {
            continue;
        }
        EnqueueResult result = this->setServoOff(i, pdMS_TO_TICKS(SEND_QUEUE_WAIT_MS));
        if (result == EnqueueResult::Queued)
        {
            queued[i] = true;
            queuedCount++;
        }
        else
        {
            // Stays exactly as it was: the channel is still `on`, so
            // CheckServos retries within one deadline.
            failedCount++;
            ESP_LOGE(TAG,
                     "Panic: off NOT queued for servo %d on module %d (%s) - stays energized until the timer retry", i,
                     this->idx, result == EnqueueResult::NoMemory ? "no memory" : "serial queue full");
        }
    }

    // Clear tracking only for channels whose off went out. Attempted even if
    // the first take failed: the sends took time and nothing about the clear
    // depends on how `enabled` was read.
    if (xSemaphoreTake(this->stateMutex, pdMS_TO_TICKS(STATE_MUTEX_WAIT_MS)) == pdTRUE)
    {
        for (size_t i = 0; i < 24; i++)
        {
            if (queued[i])
            {
                channels[i].on = false;
                channels[i].currentPos = 0;
            }
        }
        xSemaphoreGive(this->stateMutex);
    }
    else
    {
        // Physically off, tracking says on. CheckServos sends one redundant
        // off per channel within a deadline and clears it.
        ESP_LOGW(TAG, "Panic: state mutex timeout on module %d after sending offs; tracking clears on the next release",
                 this->idx);
    }

    // Neutral wording: frames are queued, not yet on the wire, and failedCount
    // may be nonzero. The per-channel ERRORs above carry the failures.
    ESP_LOGI(TAG, "Panic: module %d complete, %d off(s) queued, %d failed", this->idx, queuedCount, failedCount);

    xSemaphoreGive(this->mutex);
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
        int oldSpeed;
        int oldAccel;
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
        moves[i].oldSpeed = channels[i].speed;
        moves[i].oldAccel = channels[i].acceleration;

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
            SendStage stage = this->setServoPosition(i, moves[i].ms, 0, 0, 0);
            if (stage != SendStage::Target)
            {
                failed++;
                this->reconcileLimits(i, stage, moves[i].oldSpeed, moves[i].oldAccel);
            }
        }
    }

    if (failed > 0)
    {
        ESP_LOGE(TAG, "HomeServos: frames not queued on module %d, %d channel(s) not homed (still tracked on)",
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
    uint32_t noMemory = 0;

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

        EnqueueResult result = this->setServoOff(i, 0);
        xSemaphoreGive(this->mutex);

        if (result == EnqueueResult::Queued)
        {
            channels[i].on = false;
            channels[i].currentPos = 0;
            turnedOff |= 1u << i;
        }
        else if (result == EnqueueResult::QueueFull)
        {
            queueFull |= 1u << i;
        }
        else
        {
            noMemory |= 1u << i;
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
    if (noMemory != 0)
    {
        ESP_LOGE(TAG, "CheckServos: out of memory on module %d, channels 0x%06X retry next tick", this->idx,
                 (unsigned)noMemory);
    }
}

MaestroModule::SendStage MaestroModule::setServoPosition(uint8_t channel, int ms, int lastpos, int speed,
                                                         int acceleration)
{
    uint8_t cmd[4] = {};
    const TickType_t wait = pdMS_TO_TICKS(SEND_QUEUE_WAIT_MS);
    SendStage stage = SendStage::None;

    cmd[1] = channel;

    // Stop at the first dropped frame and report how far we got: a delivered
    // target after a dropped speed frame would move at the Maestro's previous
    // speed while the release deadline assumes the new one, so the caller
    // reconciles the tracked limits to the stage reached.

    // we need to send the last requested position
    // before we send speed/accel commands if the servo
    // was set to off as these commands will not work
    // if they happen before the servo is turned on
    if (lastpos != -1)
    {
        cmd[0] = SET_SERVO_COMMAND;
        cmd[2] = lastpos & 0x7F;
        cmd[3] = (lastpos >> 7) & 0x7F;

        if (this->enqueueFrame(cmd, 4, wait) != EnqueueResult::Queued)
        {
            return stage;
        }
        stage = SendStage::LastPos;
    }

    cmd[0] = SET_SERVO_SPEED_COMMAND;
    cmd[2] = speed & 0x7F;
    cmd[3] = (speed >> 7) & 0x7F;

    if (this->enqueueFrame(cmd, 4, wait) != EnqueueResult::Queued)
    {
        return stage;
    }
    stage = SendStage::Speed;

    cmd[0] = SET_SERVO_ACCELERATION_COMMAND;
    cmd[2] = acceleration & 0x7F;
    cmd[3] = (acceleration >> 7) & 0x7F;

    if (this->enqueueFrame(cmd, 4, wait) != EnqueueResult::Queued)
    {
        return stage;
    }
    stage = SendStage::Accel;

    // .25us resolution
    int target = ms * 4;

    cmd[0] = SET_SERVO_COMMAND;
    cmd[2] = target & 0x7F;
    cmd[3] = (target >> 7) & 0x7F;

    if (this->enqueueFrame(cmd, 4, wait) != EnqueueResult::Queued)
    {
        return stage;
    }

    ESP_LOGD(TAG, "command sent to channel: %d", channel);
    return SendStage::Target;
}

void MaestroModule::reconcileLimits(int channel, SendStage stage, int oldSpeed, int oldAccel)
{
    // Speed and accel both went out => the tracked limits already match the
    // wire, whatever happened to the target frame.
    if (static_cast<int>(stage) >= static_cast<int>(SendStage::Accel))
    {
        return;
    }

    if (xSemaphoreTake(this->stateMutex, pdMS_TO_TICKS(STATE_MUTEX_WAIT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "reconcileLimits: state mutex timeout, deadline for channel %d on module %d may run short",
                 channel, this->idx);
        return;
    }

    if (static_cast<int>(stage) < static_cast<int>(SendStage::Speed))
    {
        channels[channel].speed = oldSpeed;
    }
    channels[channel].acceleration = oldAccel;

    xSemaphoreGive(this->stateMutex);
}

MaestroModule::EnqueueResult MaestroModule::setServoOff(uint8_t channel, TickType_t wait)
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

MaestroModule::EnqueueResult MaestroModule::enqueueFrame(const uint8_t *cmd, size_t size, TickType_t wait)
{
    // Never logs: CheckServos calls this under stateMutex on the esp_timer
    // task. Callers report the result with channel/module identity, outside
    // any lock.
    queue_serial_msg_t msg;

    msg.message_id = 1;
    msg.baudrate = this->baudRate;
    msg.data = (uint8_t *)malloc(size);
    if (msg.data == NULL)
    {
        return EnqueueResult::NoMemory;
    }
    memcpy(msg.data, cmd, size);
    msg.dataSize = size;

    if (xQueueSend(this->serialQueue, &msg, wait) != pdTRUE)
    {
        free(msg.data);
        return EnqueueResult::QueueFull;
    }

    return EnqueueResult::Queued;
}

void MaestroModule::sendQueueMsg(uint8_t cmd[], size_t size)
{
    if (!this->takeSendMutex())
    {
        ESP_LOGE(TAG, "sendQueueMsg: frame dropped on module %d", this->idx);
        return;
    }

    EnqueueResult result = this->enqueueFrame(cmd, size, pdMS_TO_TICKS(SEND_QUEUE_WAIT_MS));
    if (result != EnqueueResult::Queued)
    {
        ESP_LOGW(TAG, "sendQueueMsg: frame 0x%02X dropped on module %d (%s)", cmd[0], this->idx,
                 result == EnqueueResult::NoMemory ? "no memory" : "serial queue full");
    }

    xSemaphoreGive(this->mutex);
}
