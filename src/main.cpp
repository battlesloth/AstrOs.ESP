#include <stdio.h>

#include <atomic>
#include <cinttypes>
#include <driver/rmt.h>
#include <driver/uart.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <map>
#include <memory>
#include <nvs_flash.h>
#include <string.h>
#include <vector>

#include <AnimationCommand.hpp>
#include <AnimationController.hpp>
#include <AstrOsDisplay.hpp>
#include <AstrOsSerialMsgHandler.hpp>
#include <AstrOsUtility.h>
#include <GpioModule.hpp>
#include <I2cMaster.hpp>
#include <I2cModule.hpp>
#include <OtaForwarder.hpp>
#include <OtaQueueMessage.h>
#include <OtaReceiver.hpp>
#include <OtaWriter.hpp>
#include <SerialModule.hpp>

#include <AstrOsEspNow.h>
#include <AstrOsInterfaceResponseMsg.hpp>
#include <AstrOsNames.h>
#include <AstrOsStorageManager.hpp>
#include <AstrOsUtility_ESP.h>
#include <MaestroModule.hpp>
#include <guid.h>

static const char *TAG = AstrOsConstants::ModuleName;

/**********************************
 * States
 **********************************/
static std::atomic<int> displayTimeout{0};
static std::atomic<int> defaultDisplayTimeout{10};
static std::atomic<bool> discoveryMode{false};
static std::atomic<bool> isMasterNode{false};
static uart_port_t ASTRO_PORT = UART_NUM_0;

static const char *currentRank()
{
    return isMasterNode.load() ? "Master" : "Padawan";
}

/**********************************
 * Time trackers
 **********************************/
// static uint64_t animation_loop_start = 0;
// static uint64_t animation_loop_end = 0;
// static uint64_t animation_loop_drift = 0;

static uint64_t lastHeartBeat = 0;

/**********************************
 * queues
 **********************************/

static QueueHandle_t animationQueue;
static QueueHandle_t interfaceResponseQueue;
static QueueHandle_t serviceQueue;
static QueueHandle_t serialCh1Queue;
static QueueHandle_t serialCh2Queue;
static QueueHandle_t servoQueue;
static QueueHandle_t i2cQueue;
static QueueHandle_t gpioQueue;
static QueueHandle_t espnowQueue;
static QueueHandle_t otaQueue;
static QueueHandle_t otaForwarderQueue;
static QueueHandle_t otaWriterQueue;

/**********************************
 * UART
 **********************************/

static const int RX_BUF_SIZE = 1024;

/**********************************
 * timers
 **********************************/

static esp_timer_handle_t pollingTimer;
static bool polling = false;

static esp_timer_handle_t maintenanceTimer;
static esp_timer_handle_t servoShutdownTimer;

// Silent-error counters — incremented from cross-task contexts (Wi-Fi driver task
// for espnowRecvCallback, astrosRxTask on core 0). Read from maintenanceTimerCallback
// (esp_timer task). std::atomic<uint32_t> is single-word on ESP32 and provides the
// memory visibility we need without a mutex on the hot paths.
static std::atomic<uint32_t> astrosRxOverflowCount{0};
static std::atomic<uint32_t> espnowMallocFailureCount{0};
static std::atomic<uint32_t> dispatchMallocFailureCount{0};

#define SERVO_MOVE_INTERVAL 500 // 500ms

/**********************************
 * animation
 **********************************/
#define QUEUE_LENGTH 5

/**********************************
 * Reset Button
 **********************************/

#define MEDIUM_PRESS_THRESHOLD_MS 3000 // 3 second
#define LONG_PRESS_THRESHOLD_MS 10000  // 10 seconds

/**********************************
 * Serial Settings
 **********************************/
#define ASTROS_INTEFACE_BAUD_RATE (115200)

static SerialModule SerialChannel1;
static SerialModule SerialChannel2;

static std::map<int, std::shared_ptr<MaestroModule>> maestroModules;
static SemaphoreHandle_t maestroModulesMutex = NULL;

// #define BAUD_RATE_1 (9600)

// #define MAESTRO_UART_PORT UART_NUM_2
// #define MAESTRO_BAUD_RATE (115200)

/**********************************
 * I2C Settings
 **********************************/

#define I2C_PORT I2C_NUM_0

/**********************************
 * Servo Settings
 **********************************/

#define SERVO_BOARD_0_ADDR 0x40
#define SERVO_BOARD_1_ADDR 0x41

/**********************************
 * Method definitions
 *********************************/
void init(void);

static void loadConfig();
static void loadMaestroConfigs();
static void loadGpioConfig();

// timers
static void initTimers(void);
static void pollingTimerCallback(void *arg);
static void maintenanceTimerCallback(void *arg);
static void servoShutdownTimerCallback(void *arg);

// espnow call backs
bool cachePeer(espnow_peer_t peer);
void updateSeviceConfig(std::string name, uint8_t *mac);
void displaySetDefault(std::string line1, std::string line2, std::string line3);
static void espnowSendCallback(const esp_now_send_info_t *tx_info, esp_now_send_status_t status);
static void espnowRecvCallback(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);

// tasks
void buttonListenerTask(void *arg);
void astrosRxTask(void *arg);
void serviceQueueTask(void *arg);
void interfaceResponseQueueTask(void *arg);
void animationQueueTask(void *arg);
void animationDispatchTask(void *arg);
void serialCh1QueueTask(void *arg);
void serialCh2QueueTask(void *arg);
void servoQueueTask(void *arg);
void i2cQueueTask(void *arg);
void gpioQueueTask(void *arg);
void espnowQueueTask(void *arg);
void otaReceiverTask(void *arg);
void otaForwarderTask(void *arg);
void otaWriterTask(void *arg);

// handlers
static AstrOsSerialMessageType getSerialMessageType(AstrOsInterfaceResponseType type);
static void handleRegistrationSync(astros_interface_response_t msg);
static void handleSetConfig(astros_interface_response_t msg);
static void handleSaveScript(astros_interface_response_t msg);
static void handleRunSctipt(astros_interface_response_t msg);
static void handleRunCommand(astros_interface_response_t msg);
static void handlePanicStop(astros_interface_response_t msg);
static void handleFormatSD(std::string id);
static void handleServoTest(astros_interface_response_t msg);

esp_err_t mountSdCard(void);

#pragma region Main()

extern "C"
{
    void app_main(void);
}

void app_main()
{
    init();

    // core 1
    xTaskCreatePinnedToCore(&buttonListenerTask, "button_listener_task", 4096, (void *)serviceQueue, 5, NULL, 1);
    xTaskCreatePinnedToCore(&serviceQueueTask, "service_queue_task", 4096, (void *)serviceQueue, 6, NULL, 1);
    xTaskCreatePinnedToCore(&animationQueueTask, "animation_queue_task", 4096, (void *)animationQueue, 7, NULL, 1);
    xTaskCreatePinnedToCore(&animationDispatchTask, "animation_dispatch_task", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(&interfaceResponseQueueTask, "interface_queue_task", 6144, (void *)interfaceResponseQueue,
                            10, NULL, 1);
    xTaskCreatePinnedToCore(&serialCh1QueueTask, "serial_ch1_queue_task", 4096, (void *)serialCh1Queue, 9, NULL, 1);
    xTaskCreatePinnedToCore(&serialCh2QueueTask, "serial_ch2_queue_task", 4096, (void *)serialCh2Queue, 9, NULL, 1);
    xTaskCreatePinnedToCore(&servoQueueTask, "servo_queue_task", 4096, (void *)servoQueue, 10, NULL, 1);
    xTaskCreatePinnedToCore(&i2cQueueTask, "i2c_queue_task", 3072, (void *)i2cQueue, 8, NULL, 1);
    xTaskCreatePinnedToCore(&gpioQueueTask, "gpio_queue_task", 3072, (void *)gpioQueue, 8, NULL, 1);
    if (xTaskCreatePinnedToCore(&otaReceiverTask, "ota_receiver_task", 4096, (void *)otaQueue, 6, NULL, 1) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create ota_receiver_task — aborting init");
        abort();
    }

    if (isMasterNode.load())
    {
        if (xTaskCreatePinnedToCore(&otaForwarderTask, "ota_forwarder_task", 4096, (void *)otaForwarderQueue, 6, NULL,
                                    1) != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to create otaForwarderTask — aborting init");
            abort();
        }
    }

    if (!isMasterNode.load())
    {
        // 8 KB stack (larger than the OTA receiver/forwarder family):
        // OtaWriter::handleEnd declares a 4 KB readback buffer plus the
        // SHA-256 ctx and call frames; that does not fit in 4 KB.
        if (xTaskCreatePinnedToCore(&otaWriterTask, "ota_writer_task", 8192, (void *)otaWriterQueue, 6, NULL, 1) !=
            pdPASS)
        {
            ESP_LOGE(TAG, "Failed to create otaWriterTask — aborting init");
            abort();
        }
    }

    // core 0
    xTaskCreatePinnedToCore(&astrosRxTask, "astros_rx_task", 4096, (void *)animationQueue, 9, NULL, 0);
    xTaskCreatePinnedToCore(&espnowQueueTask, "espnow_queue_task", 4096, (void *)espnowQueue, 10, NULL, 0);

    initTimers();

    loadMaestroConfigs();
    ESP_LOGI(TAG, "Maestro Modules initiated");
}

#pragma endregion
#pragma region Init()

void init(void)
{
    ESP_LOGI(TAG, "init called");
    ESP_LOGI(TAG, "AstrOs.ESP version %s (sha: %s)", AstrOsConstants::Version, AstrOsConstants::GitSha);

    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_POSEDGE; // Interrupt on rising edge
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = 1ULL << RESET_PIN;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    // Dispatch queues sized for fan-in from the animation dispatch path + producer bursts.
    // Control queues stay at QUEUE_LENGTH (5) since they carry low-rate coordination.
    animationQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_ani_cmd_t));
    serviceQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_svc_cmd_t));
    interfaceResponseQueue = xQueueCreate(QUEUE_LENGTH, sizeof(astros_interface_response_t));
    serialCh1Queue = xQueueCreate(10, sizeof(queue_serial_msg_t));
    serialCh2Queue = xQueueCreate(10, sizeof(queue_serial_msg_t));
    servoQueue = xQueueCreate(20, sizeof(queue_msg_t));
    i2cQueue = xQueueCreate(16, sizeof(queue_msg_t));
    gpioQueue = xQueueCreate(10, sizeof(queue_msg_t));
    espnowQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_espnow_msg_t));
    otaQueue = xQueueCreate(16, sizeof(queue_ota_msg_t));
    if (otaQueue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create otaQueue — aborting init");
        abort();
    }

    // otaForwarderQueue is created later, after loadConfig() resolves the
    // master/padawan role — only masters pay the 64-slot heap cost.

    maestroModulesMutex = xSemaphoreCreateMutex();
    if (maestroModulesMutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create maestroModulesMutex — aborting init");
        abort();
    }

    ESP_ERROR_CHECK(AstrOs_Storage.Init());

    loadConfig();

    // Master-only forwarder allocations: 64-slot queue + 3 esp_timer
    // handles + the OtaForwarder singleton's state. Padawans never spawn
    // the forwarder task and never receive FW_DEPLOY_BEGIN over serial,
    // so they don't need to allocate any of this.
    if (isMasterNode.load())
    {
        // Sized for streaming-phase peak load: 50 ms tick + ACK/NAK arrivals
        // from a chunk-streaming peer can overlap if the consumer is briefly
        // blocked on file I/O. 64 slots give enough headroom that
        // deadline-bearing sentinel posts don't get dropped under burst.
        otaForwarderQueue = xQueueCreate(64, sizeof(queue_ota_forwarder_msg_t));
        if (otaForwarderQueue == NULL)
        {
            ESP_LOGE(TAG, "Failed to create otaForwarderQueue — aborting init");
            abort();
        }
    }

    // otaWriterQueue is padawan-only: master never receives OTA_BEGIN /
    // OTA_DATA / OTA_END (those are master→padawan directional frames).
    // Keeping the queue padawan-only saves the queue's 16-slot * sizeof
    // allocation on master nodes.
    if (!isMasterNode.load())
    {
        otaWriterQueue = xQueueCreate(16, sizeof(queue_ota_writer_msg_t));
        if (otaWriterQueue == NULL)
        {
            ESP_LOGE(TAG, "Failed to create otaWriterQueue — aborting init");
            abort();
        }
    }

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, RX_BUF_SIZE * 2, 0, 0, NULL, 0));

    // otaForwarderQueue is nullptr on padawan; SerialMsgHandler's
    // handleFwDeployBeginInbound and AstrOsEspNow's routeOtaAckNakToForwarder
    // both null-guard before posting to it.
    AstrOs_SerialMsgHandler.Init(interfaceResponseQueue, serialCh1Queue, otaQueue, otaForwarderQueue);
    // The receiver's watchdog posts abort messages into the same queue
    // otaReceiverTask drains.
    AstrOs_OtaReceiver.Init(otaQueue);
    if (isMasterNode.load())
    {
        AstrOs_OtaForwarder.Init(otaForwarderQueue);
        AstrOs_EspNow.setOtaForwarderQueue(otaForwarderQueue);
    }
    if (!isMasterNode.load())
    {
        AstrOs_OtaWriter.Init(otaWriterQueue);
        AstrOs_EspNow.setOtaWriterQueue(otaWriterQueue);
    }
    ESP_LOGI(TAG, "AstrOs Interface initiated");

    ESP_ERROR_CHECK(i2cMaster.Init((gpio_num_t)SDA_PIN, (gpio_num_t)SCL_PIN));
    ESP_LOGI(TAG, "I2C Master initiated");

    serial_config_t serialConf1;

    if (isMasterNode.load())
    {
        serialConf1.isMaster = true;
        serialConf1.defaultBaudRate = ASTROS_INTEFACE_BAUD_RATE;
    }
    else
    {
        serialConf1.isMaster = false;
        serialConf1.defaultBaudRate = 9600;
    }

    serialConf1.port = UART_NUM_1;
    serialConf1.txPin = TX_PIN_1;
    serialConf1.rxPin = RX_PIN_1;

    ESP_ERROR_CHECK(SerialChannel1.Init(serialConf1));
    ESP_LOGI(TAG, "Serial Channel 1 initiated");

    serial_config_t serialConf2;
    serialConf2.defaultBaudRate = 9600;
    serialConf2.isMaster = false;
    serialConf2.port = UART_NUM_2;
    serialConf2.txPin = TX_PIN_2;
    serialConf2.rxPin = RX_PIN_2;

    ESP_ERROR_CHECK(SerialChannel2.Init(serialConf2));
    ESP_LOGI(TAG, "Serial Channel 2 initiated");

    ESP_ERROR_CHECK(I2cMod.Init());
    ESP_LOGI(TAG, "I2C Module initiated");

    ESP_ERROR_CHECK(GpioMod.Init({GPIO_PIN_0, GPIO_PIN_1, GPIO_PIN_2, GPIO_PIN_3, GPIO_PIN_4, GPIO_PIN_5, GPIO_PIN_6,
                                  GPIO_PIN_7, GPIO_PIN_8, GPIO_PIN_9}));
    ESP_LOGI(TAG, "GPIO Module initiated");

    svc_config_t svcConfig;

    if (AstrOs_Storage.loadServiceConfig(&svcConfig))
    {
        ESP_LOGI(TAG, "Master MAC address loaded from AstrOs_Storage. " MACSTR, MAC2STR(svcConfig.masterMacAddress));
    }
    else
    {
        ESP_LOGI(TAG, "Service config not found in storage.");
    }

    auto name = std::string(svcConfig.name, strlen(svcConfig.name));
    auto fingerprint = std::string(svcConfig.fingerprint, strlen(svcConfig.fingerprint));
    // reinterpret_cast<char *>
    int peerCount = 0;
    espnow_peer_t peerList[10] = {};

    // Load current peer list.
    peerCount = AstrOs_Storage.loadEspNowPeerConfigs(peerList);

    ESP_LOGI(TAG, "Loaded %d peers", peerCount);

    for (int i = 0; i < peerCount; i++)
    {
        ESP_LOGI(TAG, "Peer %s: " MACSTR, peerList[i].name, MAC2STR(peerList[i].mac_addr));
    }

    astros_espnow_config_t config = {.masterMac = svcConfig.masterMacAddress,
                                     .name = name,
                                     .fingerprint = fingerprint,
                                     .isMaster = isMasterNode.load(),
                                     .peers = peerList,
                                     .peerCount = peerCount,
                                     .serviceQueue = serviceQueue,
                                     .interfaceQueue = interfaceResponseQueue,
                                     .espnowSend_cb = &espnowSendCallback,
                                     .espnowRecv_cb = &espnowRecvCallback,
                                     .cachePeer_cb = &cachePeer,
                                     .updateSeviceConfig_cb = &updateSeviceConfig,
                                     .displayUpdate_cb = &displaySetDefault};

    ESP_LOGI(TAG, "Master MAC: " MACSTR, MAC2STR(config.masterMac));
    ESP_LOGI(TAG, "Name: %s", config.name.c_str());
    ESP_LOGI(TAG, "Fingerprint: %s", config.fingerprint.c_str());

    AstrOs_EspNow.init(config);
    AstrOs_Display.init(i2cQueue);
    AstrOs_Display.setDefault(currentRank(), "", name);
}

#pragma endregion
#pragma region Timers

static void initTimers(void)
{
    const esp_timer_create_args_t pTimerArgs = {.callback = &pollingTimerCallback,
                                                .arg = NULL,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "polling",
                                                .skip_unhandled_events = true};

    const esp_timer_create_args_t mTimerArgs = {.callback = &maintenanceTimerCallback,
                                                .arg = NULL,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "maintenance",
                                                .skip_unhandled_events = true};

    // May need to make this a GPT timer?
    const esp_timer_create_args_t sTimerArgs = {.callback = &servoShutdownTimerCallback,
                                                .arg = NULL,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "servoShutdown",
                                                .skip_unhandled_events = true};

    ESP_ERROR_CHECK(esp_timer_create(&pTimerArgs, &pollingTimer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(pollingTimer, 2 * 1000 * 1000));
    ESP_LOGI("init_timer", "Started polling timer");

    ESP_ERROR_CHECK(esp_timer_create(&mTimerArgs, &maintenanceTimer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(maintenanceTimer, 10 * 1000 * 1000));
    ESP_LOGI("init_timer", "Started maintenance timer");

    ESP_ERROR_CHECK(esp_timer_create(&sTimerArgs, &servoShutdownTimer));
    ESP_ERROR_CHECK(esp_timer_start_once(servoShutdownTimer, 300 * 1000));
    ESP_LOGI("init_timer", "Started servo shutdown timer");
}

static void pollingTimerCallback(void *arg)
{
    int now = esp_timer_get_time();
    int seconds = (now - lastHeartBeat) / 1000;
    lastHeartBeat = now;
    ESP_LOGI(TAG, "Heartbeat, %d milliseconds since last", seconds);

    // Snapshot atomics once per branch — avoid re-reading across the two
    // checks (which could observe different values if another task writes
    // between loads) and make the atomicity explicit at each callsite.
    const bool master = isMasterNode.load();
    const bool discovery = discoveryMode.load();

    // Skip master polling work (POLL_ACK self-TX + POLL_PADAWANS dispatch)
    // during OTA — bench measurements show the contention with otaReceiverTask
    // and astrosRxTask stalls inbound FW_CHUNK for 1-3 s per 2-s poll cycle,
    // halving wire throughput. Heartbeat log + display timeout stay unconditional.
    //
    // Padawan-side OTA gate: writer is active while a transfer is in flight.
    // Pause maintenance work to keep SPI flash contention down (esp_ota_write
    // erase+program latency degrades when other tasks hit the flash bus
    // concurrently). OtaWriter::isActive() is safe to call on master too —
    // AstrOs_OtaWriter is never Init'd on master, so active_ stays false.
    const bool otaActive =
        AstrOs_OtaReceiver.isActive() || AstrOs_OtaForwarder.isActive() || AstrOs_OtaWriter.isActive();

    // only send register requests during discovery mode
    if (master && !discovery && !otaActive)
    {
        queue_espnow_msg_t msg;
        msg.data = nullptr;

        if (!polling)
        {
            msg.eventType = POLL_PADAWANS;
            polling = true;

            // Zero-init so std::string(fingerprint) is always safe even if the
            // NVS read fails (otherwise the uninitialized buffer could stringify
            // up to the first accidental null byte, reading unknown memory).
            char fingerprint[37] = {0};
            if (!AstrOs_Storage.getControllerFingerprint(fingerprint))
            {
                ESP_LOGW(TAG, "Failed to read controller fingerprint; sending empty value");
            }
            // Master self-POLL_ACK: report own build variant so the server can
            // pick the right firmware asset at flash time.
            AstrOs_SerialMsgHandler.sendPollAckNak("00:00:00:00:00:00", "master", std::string(fingerprint),
                                                   AstrOsConstants::Version, AstrOsConstants::Variant, true);
        }
        else
        {
            msg.eventType = EXPIRE_POLLS;
            polling = false;
        }

        if (xQueueSend(espnowQueue, &msg, pdMS_TO_TICKS(250)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Send espnow queue fail");
        }
    }
    else if (!master && discovery && !otaActive)
    {
        queue_espnow_msg_t msg;
        msg.data = nullptr;

        msg.eventType = SEND_REGISTRAION_REQ;

        if (xQueueSend(espnowQueue, &msg, pdMS_TO_TICKS(250)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Send espnow queue fail");
        }
    }

    if (!discovery && displayTimeout.load() > 0)
    {
        // Use fetch_sub's returned prior value to derive the post-decrement
        // value: a separate .load() after fetch_sub would race with the
        // writers at displayTimeout = defaultDisplayTimeout.load() elsewhere
        // and could mask a decrement that just crossed zero (or trigger a
        // spurious clear).
        const int previous = displayTimeout.fetch_sub(2);
        const int remaining = previous - 2;
        ESP_LOGD(TAG, "Display Timeout: %d", remaining);
        if (remaining <= 0)
        {
            AstrOs_Display.displayClear();
        }
    }
}

static void maintenanceTimerCallback(void *arg)
{
    ESP_LOGI(TAG, "RAM left %lu", esp_get_free_heap_size());

    uint32_t rxOverflow = astrosRxOverflowCount.load(std::memory_order_relaxed);
    uint32_t espnowMallocFail = espnowMallocFailureCount.load(std::memory_order_relaxed);
    uint32_t dispatchMallocFail = dispatchMallocFailureCount.load(std::memory_order_relaxed);
    if (rxOverflow != 0 || espnowMallocFail != 0 || dispatchMallocFail != 0)
    {
        ESP_LOGW(TAG,
                 "err-counters rx-overflow=%" PRIu32 " espnow-malloc-fail=%" PRIu32 " dispatch-malloc-fail=%" PRIu32,
                 rxOverflow, espnowMallocFail, dispatchMallocFail);
    }
}

void animationDispatchTask(void *arg)
{
    constexpr uint32_t MIN_WAKE_MS = 10;
    constexpr uint32_t IDLE_WAKE_MS = 250;

    while (1)
    {
        auto highWaterMark = uxTaskGetStackHighWaterMark(NULL);
        if (highWaterMark < 500)
        {
            ESP_LOGW(TAG, "Animation Dispatch Stack HWM: %d", highWaterMark);
        }

        uint32_t nextDelayMs = IDLE_WAKE_MS;

        if (AnimationCtrl.scriptIsLoaded())
        {
            auto cmd = AnimationCtrl.getNextCommandPtr();

            if (cmd == nullptr)
            {
                // getNextCommandPtr returns nullptr only on mutex timeout, in which
                // case it has also set scriptLoaded=false (halting the current
                // sequence — see AnimationController.cpp for the safety rationale).
                // We loop normally; on the next iteration scriptIsLoaded() returns
                // false and we take the idle wake interval until a future queueScript
                // resumes animation. Without this, a single transient mutex
                // contention would require a device reboot to restore animation
                // — bad, because third-party hardware may be in a non-safe state
                // that a recovery script (not a power-cycle) needs to address.
                ESP_LOGE(TAG, "Animation command pointer is null — script halted, dispatch task idling for recovery");
                nextDelayMs = IDLE_WAKE_MS;
            }
            else
            {
                MODULE_TYPE ct = cmd->type;
                std::string val = cmd->val;
                int module = cmd->module;

                switch (ct)
                {
                case MODULE_TYPE::NONE:
                {
                    ESP_LOGI(TAG, "NONE command queued, assume buffer?");
                    break;
                }
                case MODULE_TYPE::KANGAROO:
                case MODULE_TYPE::GENERIC_SERIAL:
                {
                    ESP_LOGI(TAG, "Serial command val: %s", val.c_str());

                    // replace any occurances of \n with actual new line character
                    std::string formatted;
                    for (size_t i = 0; i < val.size(); i++)
                    {
                        if (val[i] == '\\' && i + 1 < val.size() && val[i + 1] == 'n')
                        {
                            formatted += '\n';
                            i++; // skip the 'n'
                        }
                        else if (val[i] == '\\' && i + 1 < val.size() && val[i + 1] == 'r')
                        {
                            formatted += '\r';
                            i++; // skip the 'r'
                        }
                        else
                        {
                            formatted += val[i];
                        }
                    }

                    queue_serial_msg_t serialMsg;
                    serialMsg.message_id = 0;
                    serialMsg.data = (uint8_t *)malloc(formatted.size() + 1);
                    if (serialMsg.data == NULL)
                    {
                        ESP_LOGE(TAG, "Malloc serial dispatch data fail");
                        dispatchMallocFailureCount.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    memcpy(serialMsg.data, formatted.c_str(), formatted.size());
                    serialMsg.data[formatted.size()] = '\0';

                    if (module == 1)
                    {
                        if (xQueueSend(serialCh1Queue, &serialMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
                        {
                            ESP_LOGW(TAG, "Send serial queue fail");
                            free(serialMsg.data);
                        }
                    }
                    else if (module == 2)
                    {
                        if (xQueueSend(serialCh2Queue, &serialMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
                        {
                            ESP_LOGW(TAG, "Send serial queue fail");
                            free(serialMsg.data);
                        }
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Invalid serial module %d", module);
                        free(serialMsg.data);
                    }
                    break;
                }
                case MODULE_TYPE::MAESTRO:
                {
                    ESP_LOGI(TAG, "Maestro command val: %s", val.c_str());
                    queue_msg_t servoMsg;
                    servoMsg.message_id = module;
                    servoMsg.data = (uint8_t *)malloc(val.size() + 1);
                    if (servoMsg.data == NULL)
                    {
                        ESP_LOGE(TAG, "Malloc servo dispatch data fail");
                        dispatchMallocFailureCount.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    memcpy(servoMsg.data, val.c_str(), val.size());
                    servoMsg.data[val.size()] = '\0';

                    if (xQueueSend(servoQueue, &servoMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
                    {
                        ESP_LOGW(TAG, "Send servo queue fail");
                        free(servoMsg.data);
                    }
                    break;
                }
                case MODULE_TYPE::I2C:
                {
                    ESP_LOGI(TAG, "I2C command val: %s", val.c_str());
                    queue_msg_t i2cMsg;
                    i2cMsg.message_id = 0;
                    i2cMsg.data = (uint8_t *)malloc(val.size() + 1);
                    if (i2cMsg.data == NULL)
                    {
                        ESP_LOGE(TAG, "Malloc i2c dispatch data fail");
                        dispatchMallocFailureCount.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    memcpy(i2cMsg.data, val.c_str(), val.size());
                    i2cMsg.data[val.size()] = '\0';

                    if (xQueueSend(i2cQueue, &i2cMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
                    {
                        ESP_LOGW(TAG, "Send i2c queue fail");
                        free(i2cMsg.data);
                    }
                    break;
                }
                case MODULE_TYPE::GPIO:
                {
                    ESP_LOGI(TAG, "GPIO command val: %s", val.c_str());
                    queue_msg_t gpioMsg;
                    gpioMsg.message_id = 0;
                    gpioMsg.data = (uint8_t *)malloc(val.size() + 1);
                    if (gpioMsg.data == NULL)
                    {
                        ESP_LOGE(TAG, "Malloc gpio dispatch data fail");
                        dispatchMallocFailureCount.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    memcpy(gpioMsg.data, val.c_str(), val.size());
                    gpioMsg.data[val.size()] = '\0';

                    if (xQueueSend(gpioQueue, &gpioMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
                    {
                        ESP_LOGW(TAG, "Send gpio queue fail");
                        free(gpioMsg.data);
                    }
                    break;
                }
                default:
                    break;
                }

                // AnimationController already clamps its return value to a small
                // minimum, but keep a local floor in case that contract changes.
                // msTillNextServoCommand returns signed int; guard against a
                // negative sentinel wrapping to ~4 billion ms (~49 days) on the
                // unsigned cast.
                int rawDelay = AnimationCtrl.msTillNextServoCommand();
                uint32_t scriptDelay =
                    (rawDelay < static_cast<int>(MIN_WAKE_MS)) ? MIN_WAKE_MS : static_cast<uint32_t>(rawDelay);
                nextDelayMs = scriptDelay;
            }
        }
        else
        {
            nextDelayMs = IDLE_WAKE_MS;
        }

        // vTaskDelay (not vTaskDelayUntil): script delays are data-driven and
        // variable. The pre-refactor esp_timer_start_once(delay * 1000) scheduled
        // each re-arm relative to now; vTaskDelay preserves that contract, while
        // vTaskDelayUntil would produce catch-up bursts after long scripted waits.
        vTaskDelay(pdMS_TO_TICKS(nextDelayMs));
    }
}

// Periodically check if servos should be shut down to extend servo life.
// currently there hasn't been a demand for servos to hold tension. May need
// to add an option for that in the future
static void servoShutdownTimerCallback(void *arg)
{
    // Snapshot module handles under the mutex, then release it before calling
    // CheckServos(): CheckServos can enqueue UART commands via sendQueueMsg,
    // which spins on a per-module mutex. Holding maestroModulesMutex across
    // that would block the esp_timer task and any other caller of the map.
    // shared_ptr ownership keeps each module alive for the duration of the
    // snapshot even if loadMaestroConfigs removes it concurrently.
    std::vector<std::shared_ptr<MaestroModule>> snapshot;
    if (xSemaphoreTake(maestroModulesMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        snapshot.reserve(maestroModules.size());
        for (const auto &entry : maestroModules)
        {
            snapshot.push_back(entry.second);
        }
        xSemaphoreGive(maestroModulesMutex);
    }
    else
    {
        ESP_LOGW(TAG, "servoShutdownTimerCallback: maestroModulesMutex timeout — skipping cycle");
    }

    for (auto &maestroMod : snapshot)
    {
        maestroMod->CheckServos(300);
    }

    ESP_ERROR_CHECK(esp_timer_start_once(servoShutdownTimer, 300 * 1000));
}

#pragma endregion
#pragma region Tasks

void buttonListenerTask(void *arg)
{
    uint32_t buttonStartTime = 0;
    bool buttonHandled = true;

    while (1)
    {
        if (gpio_get_level(static_cast<gpio_num_t>(RESET_PIN)) == 0)
        {
            if (buttonStartTime == 0)
            {
                ESP_LOGI(TAG, "Button Pressed");
                buttonHandled = false;
                buttonStartTime = xTaskGetTickCount();
            }
        }
        else
        {
            if (!buttonHandled)
            {
                ESP_LOGI(TAG, "Button Released");
                buttonHandled = true;

                if ((xTaskGetTickCount() - buttonStartTime) > pdMS_TO_TICKS(LONG_PRESS_THRESHOLD_MS))
                {
                    ESP_LOGI(TAG, "Clearing ESP-NOW peer config");

                    AstrOs_Storage.clearServiceConfig();

                    esp_restart();
                }
                else if ((xTaskGetTickCount() - buttonStartTime) > pdMS_TO_TICKS(MEDIUM_PRESS_THRESHOLD_MS))
                {
                    queue_svc_cmd_t cmd;
                    if (discoveryMode.load())
                    {
                        cmd.cmd = SERVICE_COMMAND::ESPNOW_DISCOVERY_MODE_OFF;
                    }
                    else
                    {
                        cmd.cmd = SERVICE_COMMAND::ESPNOW_DISCOVERY_MODE_ON;
                    }

                    cmd.data = nullptr;

                    if (xQueueSend(serviceQueue, &cmd, pdMS_TO_TICKS(500)) != pdTRUE)
                    {
                        ESP_LOGW(TAG, "Send espnow queue fail");
                    }

                    ESP_LOGI(TAG, "Discovery Switch Sent");
                }
                else if (!discoveryMode.load())
                {
                    queue_svc_cmd_t cmd;
                    cmd.cmd = SERVICE_COMMAND::SHOW_DISPLAY;
                    cmd.data = nullptr;

                    if (xQueueSend(serviceQueue, &cmd, pdMS_TO_TICKS(500)) != pdTRUE)
                    {
                        ESP_LOGW(TAG, "Send espnow queue fail");
                    }

                    ESP_LOGI(TAG, "Show Display Sent");
                }
            }

            buttonStartTime = 0;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void interfaceResponseQueueTask(void *arg)
{
    QueueHandle_t serverResponseQueue;

    serverResponseQueue = (QueueHandle_t)arg;
    astros_interface_response_t msg;

    while (1)
    {
        if (xQueueReceive(serverResponseQueue, &(msg), 0))
        {
            auto highWaterMark = uxTaskGetStackHighWaterMark(NULL);
            if (highWaterMark < 500)
            {
                ESP_LOGW(TAG, "Server Response Queue Stack HWM: %d", highWaterMark);
            }

            switch (msg.type)
            {
            case AstrOsInterfaceResponseType::SEND_POLL_ACK:
            {
                // The interface queue's `message` field for SEND_POLL_ACK is packed by
                // AstrOsEspNow::handlePollAck as `fingerprint<US>version<US>variant`
                // for newer peers, or fewer pieces for older peers that omit the trailing
                // fields (splitString strips trailing empties). Missing pieces fall back
                // to "" — the server then skips populating its variant cache for those peers.
                auto pieces = AstrOsStringUtils::splitString(std::string(msg.message), UNIT_SEPARATOR);
                std::string fp = pieces.size() > 0 ? pieces[0] : "";
                std::string ver = pieces.size() > 1 ? pieces[1] : "";
                std::string variant = pieces.size() > 2 ? pieces[2] : "";
                AstrOs_SerialMsgHandler.sendPollAckNak(msg.peerMac, msg.peerName, fp, ver, variant, true);
                break;
            }
            case AstrOsInterfaceResponseType::SEND_POLL_NAK:
            {
                AstrOs_SerialMsgHandler.sendPollAckNak(msg.peerMac, msg.peerName, "", "", "", false);
                break;
            }
            case AstrOsInterfaceResponseType::REGISTRATION_SYNC:
            {
                handleRegistrationSync(msg);
                break;
            }
            case AstrOsInterfaceResponseType::SET_CONFIG:
            {
                handleSetConfig(msg);
                break;
            }
            case AstrOsInterfaceResponseType::SEND_CONFIG:
            {
                AstrOs_EspNow.sendBasicCommand(AstrOsPacketType::CONFIG, msg.peerMac, msg.originationMsgId,
                                               msg.message);
                break;
            }
            case AstrOsInterfaceResponseType::SAVE_SCRIPT:
            {
                handleSaveScript(msg);
                break;
            }
            case AstrOsInterfaceResponseType::SEND_SCRIPT:
            {
                AstrOs_EspNow.sendBasicCommand(AstrOsPacketType::SCRIPT_DEPLOY, msg.peerMac, msg.originationMsgId,
                                               msg.message);
                break;
            }
            case AstrOsInterfaceResponseType::SCRIPT_RUN:
            {
                handleRunSctipt(msg);
                break;
            }
            case AstrOsInterfaceResponseType::SEND_SCRIPT_RUN:
            {
                AstrOs_EspNow.sendBasicCommand(AstrOsPacketType::SCRIPT_RUN, msg.peerMac, msg.originationMsgId,
                                               msg.message);
                break;
            }
            case AstrOsInterfaceResponseType::COMMAND:
            {
                handleRunCommand(msg);
                break;
            }
            case AstrOsInterfaceResponseType::SEND_COMMAND:
            {
                AstrOs_EspNow.sendBasicCommand(AstrOsPacketType::COMMAND_RUN, msg.peerMac, msg.originationMsgId,
                                               msg.message);
                break;
            }
            case AstrOsInterfaceResponseType::PANIC_STOP:
            {
                handlePanicStop(msg);
                break;
            }
            case AstrOsInterfaceResponseType::SEND_PANIC_STOP:
            {
                AstrOs_EspNow.sendBasicCommand(AstrOsPacketType::PANIC_STOP, msg.peerMac, msg.originationMsgId,
                                               "PANIC");
                break;
            }
            case AstrOsInterfaceResponseType::FORMAT_SD:
            {
                auto id = std::string(msg.originationMsgId);

                queue_svc_cmd_t cmd;
                cmd.cmd = SERVICE_COMMAND::FORMAT_SD;
                cmd.data = (uint8_t *)malloc(id.size() + 1); // dummy data
                if (cmd.data == NULL)
                {
                    ESP_LOGE(TAG, "Malloc FORMAT_SD command data fail");
                    dispatchMallocFailureCount.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                memccpy(cmd.data, id.c_str(), 0, id.size());
                cmd.data[id.size()] = '\0';
                cmd.dataSize = id.size() + 1;

                if (xQueueSend(serviceQueue, &cmd, pdMS_TO_TICKS(500)) != pdTRUE)
                {
                    ESP_LOGW(TAG, "Send espnow queue fail");
                    free(cmd.data);
                }
                break;
            }
            case AstrOsInterfaceResponseType::SEND_FORMAT_SD:
            {
                AstrOs_EspNow.sendBasicCommand(AstrOsPacketType::FORMAT_SD, msg.peerMac, msg.originationMsgId,
                                               "FORMATSD");
                break;
            }
            case AstrOsInterfaceResponseType::SEND_CONFIG_ACK:
            {
                auto responseType = getSerialMessageType(msg.type);
                AstrOs_SerialMsgHandler.sendBasicAckNakResponse(responseType, msg.originationMsgId, msg.peerMac,
                                                                msg.peerName, msg.message);
                break;
            }
            case AstrOsInterfaceResponseType::SERVO_TEST:
            {
                handleServoTest(msg);
                break;
            }
            case AstrOsInterfaceResponseType::SEND_SERVO_TEST:
            {
                AstrOs_EspNow.sendBasicCommand(AstrOsPacketType::SERVO_TEST, msg.peerMac, msg.originationMsgId,
                                               msg.message);
                break;
            }
            case AstrOsInterfaceResponseType::SEND_CONFIG_NAK:
            case AstrOsInterfaceResponseType::SAVE_SCRIPT_ACK:
            case AstrOsInterfaceResponseType::SAVE_SCRIPT_NAK:
            case AstrOsInterfaceResponseType::SCRIPT_RUN_ACK:
            case AstrOsInterfaceResponseType::SCRIPT_RUN_NAK:
            case AstrOsInterfaceResponseType::FORMAT_SD_ACK:
            case AstrOsInterfaceResponseType::FORMAT_SD_NAK:
            case AstrOsInterfaceResponseType::COMMAND_ACK:
            case AstrOsInterfaceResponseType::COMMAND_NAK:
            case AstrOsInterfaceResponseType::SERVO_TEST_ACK:
            {
                auto responseType = getSerialMessageType(msg.type);
                AstrOs_SerialMsgHandler.sendBasicAckNakResponse(responseType, msg.originationMsgId, msg.peerMac,
                                                                msg.peerName, msg.message);
                break;
            }
            default:
                ESP_LOGE(TAG, "Unknown/Invalid message type: %d", static_cast<int>(msg.type));
                break;
            }

            free(msg.originationMsgId);
            free(msg.peerMac);
            free(msg.peerName);
            free(msg.message);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void astrosRxTask(void *arg)
{
    // 8 KB sized for FW_CHUNK lines (~5500 B after base64 + headers).
    size_t bufferLength = 8192;
    int bufferIndex = 0;
    uint8_t *data = (uint8_t *)malloc(RX_BUF_SIZE + 1);
    uint8_t *commandBuffer = (uint8_t *)malloc(bufferLength);

    while (1)
    {
        const int rxBytes = uart_read_bytes(ASTRO_PORT, data, RX_BUF_SIZE, pdMS_TO_TICKS(1000));

        if (rxBytes > 0)
        {
            auto highWaterMark = uxTaskGetStackHighWaterMark(NULL);
            if (highWaterMark < 500)
            {
                ESP_LOGW(TAG, "AstrOs RX Stack HWM: %d", highWaterMark);
            }

            for (int i = 0; i < rxBytes; i++)
            {
                if (data[i] == '\n')
                {
                    commandBuffer[bufferIndex] = '\0';

                    // Compact log for FW_CHUNK — dumping the full ~5.5 KB
                    // base64 body would compete with FW_CHUNK_ACK for the UART
                    // TX and trip the server's retransmit timer. The CRC is
                    // always the last 4 chars by protocol contract. Bounded
                    // scan over the first 32 bytes avoids touching the body
                    // for non-chunk messages.
                    bool isFwChunk = false;
                    const int scanEnd = bufferIndex < 32 ? bufferIndex : 32;
                    for (int s = 0; s + 8 <= scanEnd; s++)
                    {
                        if (memcmp(&commandBuffer[s], "FW_CHUNK", 8) == 0)
                        {
                            isFwChunk = true;
                            break;
                        }
                    }
                    if (isFwChunk && bufferIndex >= 4)
                    {
                        ESP_LOGI("AstrOs RX", "FW_CHUNK (%d bytes, crc=%.4s)", bufferIndex,
                                 &commandBuffer[bufferIndex - 4]);
                    }
                    else
                    {
                        ESP_LOGI("AstrOs RX", "Read %d bytes: '%s'", bufferIndex, commandBuffer);
                    }

                    AstrOs_SerialMsgHandler.handleMessage(
                        std::string(reinterpret_cast<char *>(commandBuffer), bufferIndex));

                    bufferIndex = 0;
                }
                else
                {
                    commandBuffer[bufferIndex] = data[i];
                    bufferIndex++;
                    if (bufferIndex >= bufferLength)
                    {
                        // Data loss — drops partial message + bytes until next line terminator.
                        ESP_LOGE("AstrOs RX", "Line overflow (>%zu B) — discarding partial message", bufferLength);
                        astrosRxOverflowCount.fetch_add(1, std::memory_order_relaxed);
                        bufferIndex = 0;
                    }
                }
            }
        }
    }
    free(data);
    free(commandBuffer);
}

void serviceQueueTask(void *arg)
{

    QueueHandle_t svcQueue;

    svcQueue = (QueueHandle_t)arg;
    queue_svc_cmd_t msg;
    while (1)
    {
        if (xQueueReceive(svcQueue, &(msg), 0))
        {
            auto highWaterMark = uxTaskGetStackHighWaterMark(NULL);
            if (highWaterMark < 500)
            {
                ESP_LOGW(TAG, "Service Queue Stack HWM: %d", highWaterMark);
            }

            switch (msg.cmd)
            {
            case SERVICE_COMMAND::FORMAT_SD:
            {
                ESP_LOGI(TAG, "Formatting SD card");
                std::string data(reinterpret_cast<char *>(msg.data), msg.dataSize);
                handleFormatSD(data);
                break;
            }
            case SERVICE_COMMAND::SHOW_DISPLAY:
            {
                AstrOs_Display.displayDefault();
                displayTimeout.store(defaultDisplayTimeout.load());
                break;
            }
            case SERVICE_COMMAND::ESPNOW_DISCOVERY_MODE_ON:
            {
                discoveryMode.store(true);
                AstrOs_Display.displayUpdate("Discovery", "Mode On");
                ESP_LOGI(TAG, "Discovery mode on");
                break;
            }
            case SERVICE_COMMAND::ESPNOW_DISCOVERY_MODE_OFF:
            {
                discoveryMode.store(false);
                AstrOs_Display.displayDefault();
                displayTimeout.store(defaultDisplayTimeout.load());
                ESP_LOGI(TAG, "Discovery mode off");
                break;
            }
            case SERVICE_COMMAND::ASTROS_INTERFACE_MESSAGE:
            {
                queue_msg_t serialMsg;
                serialMsg.message_id = 1;
                serialMsg.data = (uint8_t *)malloc(msg.dataSize + 1);
                if (serialMsg.data == NULL)
                {
                    ESP_LOGE(TAG, "Malloc ASTROS_INTERFACE_MESSAGE data fail");
                    dispatchMallocFailureCount.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                memcpy(serialMsg.data, msg.data, msg.dataSize);
                serialMsg.data[msg.dataSize] = '\n';
                serialMsg.dataSize = msg.dataSize + 1;

                if (xQueueSend(serialCh1Queue, &serialMsg, pdMS_TO_TICKS(500)) != pdTRUE)
                {
                    ESP_LOGW(TAG, "Sending AstrOs Interface message to serial queue fail");
                    free(serialMsg.data);
                }

                break;
            }
            case SERVICE_COMMAND::RELOAD_CONFIG:
            {
                ESP_LOGI(TAG, "Reloading config");
                loadMaestroConfigs();
                loadGpioConfig();
                break;
            }
            default:
                break;
            }

            free(msg.data);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void animationQueueTask(void *arg)
{

    QueueHandle_t animationQueue;

    animationQueue = (QueueHandle_t)arg;
    queue_ani_cmd_t msg;

    while (1)
    {
        if (xQueueReceive(animationQueue, &(msg), 0))
        {
            auto highWaterMark = uxTaskGetStackHighWaterMark(NULL);
            if (highWaterMark < 500)
            {
                ESP_LOGW(TAG, "Animation Queue Stack HWM: %d", highWaterMark);
            }

            switch (msg.cmd)
            {
            case ANIMATION_COMMAND::PANIC_STOP:
                AnimationCtrl.panicStop();
                break;
            case ANIMATION_COMMAND::RUN_ANIMATION:
                AnimationCtrl.queueScript(std::string(msg.data));
                break;
            default:
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/*void hardwareQueueTask(void *arg)
{

    QueueHandle_t hardwareQueue;

    hardwareQueue = (QueueHandle_t)arg;
    queue_hw_cmd_t msg;

    while (1)
    {
        if (xQueueReceive(hardwareQueue, &(msg), 0))
        {
            ESP_LOGI(TAG, "Hardware Queue Stack HWM: %d", uxTaskGetStackHighWaterMark(NULL));

            switch (msg.cmd)
            {
            case HARDWARE_COMMAND::SEND_SERIAL:
            {
                queue_msg_t serialMsg = {0, 0};
                strncpy(serialMsg.data, msg.data, sizeof(serialMsg.data));
                serialMsg.data[sizeof(serialMsg.data) - 1] = '\0';
                xQueueSend(serialQueue, &serialMsg, pdMS_TO_TICKS(2000));
                break;
            }
            case HARDWARE_COMMAND::LOAD_SERVO_CONFIG:
            {
                ServoMod.LoadServoConfig();
                break;
            }
            case HARDWARE_COMMAND::MOVE_SERVO:
            {
                queue_msg_t pwmMsg = {0, 0};
                strncpy(pwmMsg.data, msg.data, sizeof(pwmMsg.data));
                pwmMsg.data[sizeof(pwmMsg.data) - 1] = '\0';
                xQueueSend(servoQueue, &pwmMsg, pdMS_TO_TICKS(2000));
                break;
            }
            case HARDWARE_COMMAND::SEND_I2C:
            {
                queue_msg_t i2cMsg = {0, 0};
                strncpy(i2cMsg.data, msg.data, sizeof(i2cMsg.data));
                i2cMsg.data[sizeof(i2cMsg.data) - 1] = '\0';
                xQueueSend(i2cQueue, &i2cMsg, pdMS_TO_TICKS(2000));
                break;
            }
            case HARDWARE_COMMAND::DISPLAY_COMMAND:
            {
                queue_msg_t i2cMsg = {1, 0};
                strncpy(i2cMsg.data, msg.data, sizeof(i2cMsg.data));
                i2cMsg.data[sizeof(i2cMsg.data) - 1] = '\0';
                xQueueSend(i2cQueue, &i2cMsg, pdMS_TO_TICKS(2000));
                break;
            }
            break;
            default:
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
*/

void serialCh1QueueTask(void *arg)
{
    QueueHandle_t serialCh1Queue;

    serialCh1Queue = (QueueHandle_t)arg;
    queue_serial_msg_t msg;

    while (1)
    {
        if (xQueueReceive(serialCh1Queue, &(msg), 0))
        {
            auto highWaterMark = uxTaskGetStackHighWaterMark(NULL);
            if (highWaterMark < 500)
            {
                ESP_LOGW(TAG, "Serial CH1 Queue Stack HWM: %d", highWaterMark);
            }

            if (msg.message_id == 0)
            {
                SerialChannel1.SendCommand(msg.data);
            }
            else if (msg.message_id == 1)
            {
                std::string str(reinterpret_cast<char *>(msg.data), msg.dataSize);

                ESP_LOGD(TAG, "Serial message: %s", str.c_str());

                SerialChannel1.SendBytes(msg.baudrate, msg.data, msg.dataSize);
            }

            free(msg.data);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void serialCh2QueueTask(void *arg)
{
    QueueHandle_t serialCh2Queue;

    serialCh2Queue = (QueueHandle_t)arg;
    queue_serial_msg_t msg;

    while (1)
    {
        if (xQueueReceive(serialCh2Queue, &(msg), 0))
        {
            auto highWaterMark = uxTaskGetStackHighWaterMark(NULL);
            if (highWaterMark < 500)
            {
                ESP_LOGW(TAG, "Serial CH2 Queue Stack HWM: %d", highWaterMark);
            }

            if (msg.message_id == 0)
            {
                SerialChannel2.SendCommand(msg.data);
            }
            else if (msg.message_id == 1)
            {
                std::string str(reinterpret_cast<char *>(msg.data), msg.dataSize);

                ESP_LOGD(TAG, "Serial message: %s", str.c_str());
                SerialChannel2.SendBytes(msg.baudrate, msg.data, msg.dataSize);
            }

            free(msg.data);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void servoQueueTask(void *arg)
{
    QueueHandle_t pwmQueue;

    pwmQueue = (QueueHandle_t)arg;
    queue_msg_t msg;

    while (1)
    {
        if (xQueueReceive(pwmQueue, &(msg), 0))
        {
            auto highWaterMark = uxTaskGetStackHighWaterMark(NULL);
            if (highWaterMark < 500)
            {
                ESP_LOGW(TAG, "Servo Queue Stack HWM: %d", highWaterMark);
            }

            ESP_LOGD(TAG, "Servo Command received on queue => %s", msg.data);

            // Snapshot the target module under maestroModulesMutex, then
            // release before calling QueueCommand(): QueueCommand ultimately
            // calls sendQueueMsg() which spins on a per-module mutex and
            // blocks on queue sends. Holding the map mutex across that would
            // stall loadMaestroConfigs() and other callers of the map. The
            // shared_ptr keeps the module alive for the duration of the call
            // even if a concurrent config reload removes it from the map.
            std::shared_ptr<MaestroModule> target;
            if (xSemaphoreTake(maestroModulesMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
            {
                auto it = maestroModules.find(msg.message_id);
                if (it == maestroModules.end())
                {
                    ESP_LOGE(TAG, "Maestro module %d not found", msg.message_id);
                }
                else
                {
                    target = it->second;
                }
                xSemaphoreGive(maestroModulesMutex);
            }
            else
            {
                ESP_LOGW(TAG, "servoQueueTask: failed to acquire maestroModulesMutex within 1s");
            }

            if (target)
            {
                target->QueueCommand(msg.data);
            }
            free(msg.data);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void i2cQueueTask(void *arg)
{
    QueueHandle_t i2cQueue;

    i2cQueue = (QueueHandle_t)arg;
    queue_msg_t msg;

    while (1)
    {
        if (xQueueReceive(i2cQueue, &(msg), 0))
        {
            auto highWaterMark = uxTaskGetStackHighWaterMark(NULL);
            if (highWaterMark < 500)
            {
                ESP_LOGW(TAG, "I2C Queue Stack HWM: %d", highWaterMark);
            }

            ESP_LOGI(TAG, "I2C Command received on queue => %d, %s", msg.message_id, msg.data);

            if (msg.message_id == 0)
            {
                I2cMod.SendCommand(msg.data);
            }
            else if (msg.message_id == 1)
            {
                I2cMod.WriteDisplay(msg.data);
            }

            free(msg.data);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void gpioQueueTask(void *arg)
{
    QueueHandle_t gpioQueue;

    gpioQueue = (QueueHandle_t)arg;
    queue_msg_t msg;

    while (1)
    {
        if (xQueueReceive(gpioQueue, &(msg), 0))
        {
            auto highWaterMark = uxTaskGetStackHighWaterMark(NULL);
            if (highWaterMark < 500)
            {
                ESP_LOGW(TAG, "GPIO Queue Stack HWM: %d", highWaterMark);
            }

            ESP_LOGD(TAG, "GPIO Command received on queue => %d, %s", msg.message_id, msg.data);

            GpioMod.SendCommand(msg.data);

            free(msg.data);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void otaReceiverTask(void *arg)
{
    QueueHandle_t queue = (QueueHandle_t)arg;
    queue_ota_msg_t msg;

    // portMAX_DELAY is safe — this task is the queue's exclusive consumer
    // and has no other work to do.
    while (1)
    {
        if (xQueueReceive(queue, &msg, portMAX_DELAY))
        {
            auto highWaterMark = uxTaskGetStackHighWaterMark(NULL);
            if (highWaterMark < 500)
            {
                ESP_LOGW(TAG, "OTA Receiver Stack HWM: %d", highWaterMark);
            }

            AstrOs_OtaReceiver.process(msg);
        }
    }
}

void otaForwarderTask(void *arg)
{
    QueueHandle_t queue = (QueueHandle_t)arg;
    queue_ota_forwarder_msg_t msg;

    while (true)
    {
        if (xQueueReceive(queue, &msg, portMAX_DELAY) == pdTRUE)
        {
            AstrOs_OtaForwarder.process(msg);
        }

        UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
        if (hwm < 500)
        {
            ESP_LOGW(TAG, "OTA Forwarder Stack HWM: %u", (unsigned int)hwm);
        }
    }
}

void otaWriterTask(void *arg)
{
    QueueHandle_t queue = (QueueHandle_t)arg;
    queue_ota_writer_msg_t msg;

    while (true)
    {
        if (xQueueReceive(queue, &msg, portMAX_DELAY) == pdTRUE)
        {
            // process() internally calls freeOtaWriterMsg — do NOT free here.
            // Matches otaReceiverTask + otaForwarderTask convention.
            AstrOs_OtaWriter.process(msg);
        }

        UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
        if (hwm < 500)
        {
            ESP_LOGW(TAG, "OTA Writer Stack HWM: %u", (unsigned int)hwm);
        }
    }
}

void espnowQueueTask(void *arg)
{
    QueueHandle_t espnowQueue;

    espnowQueue = (QueueHandle_t)arg;

    vTaskDelay(pdMS_TO_TICKS(5 * 1000));

    ESP_LOGI(TAG, "ESP-NOW Queue started");

    AstrOs_Display.displayDefault();
    displayTimeout.store(defaultDisplayTimeout.load());

    queue_espnow_msg_t msg;

    while (1)
    {
        if (xQueueReceive(espnowQueue, &(msg), 0))
        {
            auto highWaterMark = uxTaskGetStackHighWaterMark(NULL);
            if (highWaterMark < 500)
            {
                ESP_LOGW(TAG, "ESP-NOW Queue Stack HWM: %d", highWaterMark);
            }

            switch (msg.eventType)
            {
            case SEND_REGISTRAION_REQ:
            {
                AstrOs_EspNow.sendRegistrationRequest();
                break;
            }
            case POLL_PADAWANS:
            {
                AstrOs_EspNow.pollPadawans();
                break;
            }
            case EXPIRE_POLLS:
            {
                AstrOs_EspNow.pollRepsonseTimeExpired();
                break;
            }
            case ESPNOW_SEND:
            {
                ESP_LOGD(TAG, "Send data to " MACSTR, MAC2STR(msg.dest));

                if (esp_now_send(msg.dest, msg.data, msg.data_len) != ESP_OK)
                {
                    ESP_LOGE(TAG, "Send error");
                }

                break;
            }
            case ESPNOW_RECV:
            {
                if (IS_BROADCAST_ADDR(msg.dest))
                {
                    ESP_LOGD(TAG, "Received broadcast data from: " MACSTR ", len: %d", MAC2STR(msg.src), msg.data_len);

                    // only handle broadcast messages in discovery mode
                    if (!discoveryMode.load())
                    {
                        break;
                    }

                    AstrOs_EspNow.handleMessage(msg.src, msg.data, msg.data_len);
                }
                else if (esp_now_is_peer_exist(msg.src))
                {
                    ESP_LOGD(TAG, "Received unicast data from peer: " MACSTR ", len: %d", MAC2STR(msg.src),
                             msg.data_len);

                    AstrOs_EspNow.handleMessage(msg.src, msg.data, msg.data_len);
                }
                else
                {
                    ESP_LOGW(TAG, "Peer does not exist:" MACSTR, MAC2STR(msg.src));
                }
                break;
            }
            default:
                ESP_LOGE(TAG, "Callback type error: %d", msg.eventType);
                break;
            }

            free(msg.data);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

#pragma endregion
#pragma region Config Methods

static void loadConfig()
{
    auto cfig = AstrOs_Storage.readFile("config.txt");

    if (cfig != "error")
    {
        ESP_LOGI(TAG, "Service config file: %s", cfig.c_str());

        auto lines = AstrOsStringUtils::splitStringOnLineEnd(cfig);

        for (auto line : lines)
        {
            auto parts = AstrOsStringUtils::splitString(line, '=');

            if (parts.size() == 2)
            {
                if (parts[0] == "isMasterNode")
                {

                    if (parts[1].find("true") != std::string::npos)
                    {
                        ESP_LOGI(TAG, "isMasterNode: %s", parts[1].c_str());
                        isMasterNode.store(true);
                        ASTRO_PORT = UART_NUM_1;
                        ESP_LOGI(TAG, "%s node using UART channel 1", currentRank());
                    }
                    else
                    {
                        ESP_LOGI(TAG, "isMasterNode: %s", parts[1].c_str());
                        isMasterNode.store(false);
                        ASTRO_PORT = UART_NUM_0;
                        ESP_LOGI(TAG, "%s node using UART channel 0", currentRank());
                    }
                }
            }
        }
    }
    else
    {
        ESP_LOGW(TAG, "Service config file not found");
    }
}

static void loadMaestroConfigs()
{
    ESP_LOGI(TAG, "Loading Maestro configurations");

    // Storage read happens outside the map mutex — it only touches NVS, not
    // the map, and keeping it outside shortens the critical section.
    auto maestroConfigs = AstrOs_Storage.loadMaestroConfigs();
    ESP_LOGI(TAG, "Loaded Maestro modules from file: %d", maestroConfigs.size());

    struct PendingUpdate
    {
        std::shared_ptr<MaestroModule> module;
        QueueHandle_t queue;
        int baudrate;
    };
    std::vector<PendingUpdate> pendingUpdates;
    std::vector<std::shared_ptr<MaestroModule>> toInitialize;

    if (xSemaphoreTake(maestroModulesMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "loadMaestroConfigs: failed to acquire maestroModulesMutex within 1s");
        return;
    }

    // get list of current maestro modules
    std::vector<int> currentModules;
    currentModules.reserve(maestroModules.size());
    for (const auto &entry : maestroModules)
    {
        currentModules.push_back(entry.first);
    }

    ESP_LOGI(TAG, "Current Maestro module count: %d", currentModules.size());

    for (auto &cfg : maestroConfigs)
    {
        auto existing = maestroModules.find(cfg.idx);
        if (existing != maestroModules.end())
        {
            // Module already present — record a pending UpdateConfig call to
            // apply outside the map lock. The map entry itself doesn't change.
            if (cfg.uartChannel == 1 && isMasterNode.load())
            {
                ESP_LOGE(TAG, "Master node cannot use UART channel 1 for Maestro module %d", cfg.idx);
                continue; // skip invalid configurations
            }
            else if (cfg.uartChannel == 1)
            {
                pendingUpdates.push_back({existing->second, serialCh1Queue, cfg.baudrate});
            }
            else if (cfg.uartChannel == 2)
            {
                pendingUpdates.push_back({existing->second, serialCh2Queue, cfg.baudrate});
            }
            else
            {
                ESP_LOGE(TAG, "Invalid UART channel %d for Maestro module %d", cfg.uartChannel, cfg.idx);
                continue; // skip invalid configurations
            }
            currentModules.erase(std::remove(currentModules.begin(), currentModules.end(), cfg.idx),
                                 currentModules.end());
        }
        else
        {
            // if the module is not loaded, create a new one
            if (cfg.uartChannel == 1 && isMasterNode.load())
            {
                ESP_LOGE(TAG, "Master node cannot use UART channel 1 for Maestro module %d", cfg.idx);
                continue; // skip invalid configurations
            }
            if (cfg.uartChannel == 1)
            {
                maestroModules[cfg.idx] = std::make_shared<MaestroModule>(serialCh1Queue, cfg.idx, cfg.baudrate);
            }
            else if (cfg.uartChannel == 2)
            {
                maestroModules[cfg.idx] = std::make_shared<MaestroModule>(serialCh2Queue, cfg.idx, cfg.baudrate);
            }
            else
            {
                ESP_LOGE(TAG, "Invalid UART channel %d for Maestro module %d", cfg.uartChannel, cfg.idx);
                continue; // skip invalid configurations
            }
        }
    }

    // remove any modules that are no longer in the configuration
    for (int idx : currentModules)
    {
        ESP_LOGI(TAG, "Removing Maestro module: %d", idx);
        if (maestroModules.find(idx) != maestroModules.end())
        {
            maestroModules.erase(idx);
        }
    }

    ESP_LOGI(TAG, "Maestro module count: %d", maestroModules.size());

    toInitialize.reserve(maestroModules.size());
    for (const auto &entry : maestroModules)
    {
        toInitialize.push_back(entry.second);
    }

    xSemaphoreGive(maestroModulesMutex);

    // Phase 2: apply recorded UpdateConfig calls outside the map mutex. Each
    // call may spin on the per-module mutex, but doing so no longer blocks
    // the container. shared_ptr ownership keeps the module alive even if a
    // concurrent reload removes its map entry in between.
    for (auto &pending : pendingUpdates)
    {
        pending.module->UpdateConfig(pending.queue, pending.baudrate);
    }

    // Phase 3: initialize each module (NVS read + HomeServos) outside the
    // map mutex. UpdateConfig has already landed above so LoadConfig sees
    // the intended queue/baud.
    for (auto &maestroMod : toInitialize)
    {
        maestroMod->LoadConfig();
    }
}

void loadGpioConfig()
{

    ESP_LOGI(TAG, "Loading GPIO configurations");

    auto config = AstrOs_Storage.loadGpioConfigs();
    GpioMod.UpdateConfig(config);
}

#pragma endregion
#pragma region Callbacks

bool cachePeer(espnow_peer_t peer)
{
    return AstrOs_Storage.saveEspNowPeer(peer);
}

void updateSeviceConfig(std::string name, uint8_t *mac)
{
    svc_config_t svcConfig;
    memcpy(svcConfig.masterMacAddress, mac, ESP_NOW_ETH_ALEN);
    strncpy(svcConfig.name, name.c_str(), sizeof(svcConfig.name));
    svcConfig.name[sizeof(svcConfig.name) - 1] = '\0';
    AstrOs_Storage.saveServiceConfig(svcConfig);
}

void displaySetDefault(std::string line1, std::string line2, std::string line3)
{
    return AstrOs_Display.setDefault(line1, line2, line3);
}

static void espnowSendCallback(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    if (tx_info == NULL)
    {
        ESP_LOGE(TAG, "Send cb arg error");
        return;
    }

    ESP_LOGD(TAG, "Sending data to " MACSTR " status: %d", MAC2STR(tx_info->des_addr), status);
}

static void espnowRecvCallback(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    ESP_LOGD(TAG, "espnowRecvCallback called");

    queue_espnow_msg_t msg;

    uint8_t *src_addr = recv_info->src_addr;
    uint8_t *dest_addr = recv_info->des_addr;

    if (src_addr == NULL || data == NULL || len <= 0)
    {
        ESP_LOGE(TAG, "Receive cb arg error");
        return;
    }

    msg.eventType = ESPNOW_RECV;
    memcpy(msg.src, src_addr, ESP_NOW_ETH_ALEN);
    memcpy(msg.dest, dest_addr, ESP_NOW_ETH_ALEN);
    msg.data = (uint8_t *)malloc(len);
    if (msg.data == NULL)
    {
        ESP_LOGE(TAG, "Malloc receive data fail");
        espnowMallocFailureCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    memcpy(msg.data, data, len);
    msg.data_len = len;
    if (xQueueSend(espnowQueue, &msg, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send receive queue fail");
        free(msg.data);
    }
}

#pragma endregion
#pragma region Interface Methods

static AstrOsSerialMessageType getSerialMessageType(AstrOsInterfaceResponseType type)
{
    switch (type)
    {
    case AstrOsInterfaceResponseType::SEND_CONFIG_ACK:
        return AstrOsSerialMessageType::DEPLOY_CONFIG_ACK;
    case AstrOsInterfaceResponseType::SEND_CONFIG_NAK:
        return AstrOsSerialMessageType::DEPLOY_CONFIG_NAK;
    case AstrOsInterfaceResponseType::SAVE_SCRIPT_ACK:
        return AstrOsSerialMessageType::DEPLOY_SCRIPT_ACK;
    case AstrOsInterfaceResponseType::SAVE_SCRIPT_NAK:
        return AstrOsSerialMessageType::DEPLOY_SCRIPT_NAK;
    case AstrOsInterfaceResponseType::SCRIPT_RUN_ACK:
        return AstrOsSerialMessageType::RUN_SCRIPT_ACK;
    case AstrOsInterfaceResponseType::SCRIPT_RUN_NAK:
        return AstrOsSerialMessageType::RUN_SCRIPT_NAK;
    case AstrOsInterfaceResponseType::FORMAT_SD_ACK:
        return AstrOsSerialMessageType::FORMAT_SD_ACK;
    case AstrOsInterfaceResponseType::FORMAT_SD_NAK:
        return AstrOsSerialMessageType::FORMAT_SD_NAK;
    case AstrOsInterfaceResponseType::SEND_SERVO_TEST_ACK:
        return AstrOsSerialMessageType::SERVO_TEST_ACK;

    default:
        return AstrOsSerialMessageType::UNKNOWN;
    }
}

static void handleRegistrationSync(astros_interface_response_t msg)
{
    std::vector<astros_peer_data_t> data = {};

    auto peers = AstrOs_EspNow.getPeers();

    for (auto peer : peers)
    {
        astros_peer_data_t pd;
        pd.name = peer.name;
        pd.mac = AstrOsStringUtils::macToString(peer.mac_addr);
        pd.fingerprint = "na";
        data.push_back(pd);
    }

    AstrOs_SerialMsgHandler.sendRegistraionAck(msg.originationMsgId, data);
}

static void handleSetConfig(astros_interface_response_t msg)
{
    auto success = AstrOs_Storage.saveModuleConfigs(msg.message);

    std::string fingerprint;

    if (success)
    {
        ESP_LOGI(TAG, "Updating Fingerprint");
        fingerprint = guid::generate_guid();

        success = AstrOs_Storage.setControllerFingerprint(fingerprint.c_str());
    }

    if (success)
    {
        AstrOs_EspNow.updateFingerprint(fingerprint);

        ESP_LOGI(TAG, "New Fingerprint: %s", fingerprint.c_str());

        // send servo reload message
        queue_svc_cmd_t reloadMsg;
        reloadMsg.cmd = SERVICE_COMMAND::RELOAD_CONFIG;
        reloadMsg.data = nullptr;

        if (xQueueSend(serviceQueue, &reloadMsg, pdMS_TO_TICKS(500)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Send servo reload fail");
        }
    }
    else
    {
        ESP_LOGE(TAG, "Failed to set config: %s", msg.message);
    }

    if (isMasterNode.load())
    {
        auto ackNak = success ? AstrOsSerialMessageType::DEPLOY_CONFIG_ACK : AstrOsSerialMessageType::DEPLOY_CONFIG_NAK;

        AstrOs_SerialMsgHandler.sendBasicAckNakResponse(ackNak, msg.originationMsgId, AstrOs_EspNow.getMac(), "master",
                                                        AstrOs_EspNow.getFingerprint());
    }
    else
    {
        AstrOs_EspNow.sendConfigAckNak(msg.originationMsgId, success);
    }
}

static void handleSaveScript(astros_interface_response_t msg)
{
    auto message = std::string(msg.message);
    auto parts = AstrOsStringUtils::splitString(message, UNIT_SEPARATOR);

    auto success = false;

    if (parts.size() != 2)
    {
        ESP_LOGE(TAG, "Invalid script message: %s", message.c_str());
    }
    else
    {
        success = AstrOs_Storage.saveFile("scripts/" + parts[0], parts[1]);
    }

    if (isMasterNode.load())
    {
        auto ackNak = success ? AstrOsSerialMessageType::DEPLOY_SCRIPT_ACK : AstrOsSerialMessageType::DEPLOY_SCRIPT_NAK;

        AstrOs_SerialMsgHandler.sendBasicAckNakResponse(ackNak, msg.originationMsgId, AstrOs_EspNow.getMac(), "master",
                                                        parts[0]);
    }
    else
    {
        auto ackNak = success ? AstrOsPacketType::SCRIPT_DEPLOY_ACK : AstrOsPacketType::SCRIPT_DEPLOY_NAK;
        AstrOs_EspNow.sendBasicAckNak(msg.originationMsgId, ackNak, parts[0]);
    }
}

static void handleRunSctipt(astros_interface_response_t msg)
{
    auto message = std::string(msg.message);
    auto parts = AstrOsStringUtils::splitString(message, UNIT_SEPARATOR);

    auto success = false;

    if (parts.size() != 1)
    {
        ESP_LOGE(TAG, "Invalid run script message: %s", message.c_str());
    }
    else
    {
        success = AnimationCtrl.queueScript(parts[0]);
    }

    if (isMasterNode.load())
    {
        auto ackNak = success ? AstrOsSerialMessageType::RUN_SCRIPT_ACK : AstrOsSerialMessageType::RUN_SCRIPT_NAK;

        AstrOs_SerialMsgHandler.sendBasicAckNakResponse(ackNak, msg.originationMsgId, AstrOs_EspNow.getMac(), "master",
                                                        parts[0]);
    }
    else
    {
        auto ackNak = success ? AstrOsPacketType::SCRIPT_RUN_ACK : AstrOsPacketType::SCRIPT_RUN_NAK;
        AstrOs_EspNow.sendBasicAckNak(msg.originationMsgId, ackNak, parts[0]);
    }
}

static void handleRunCommand(astros_interface_response_t msg)
{
    AnimationCtrl.queueCommand(msg.message);
}

static void handlePanicStop(astros_interface_response_t msg)
{
    AnimationCtrl.panicStop();
}

static void handleFormatSD(std::string id)
{
    auto cfig = AstrOs_Storage.readFile("config.txt");

    esp_err_t formatErr = AstrOs_Storage.formatSdCard();
    bool success = (formatErr == ESP_OK);

    if (!success)
    {
        ESP_LOGE(TAG, "formatSdCard failed: %s", esp_err_to_name(formatErr));
    }

    if (cfig != "error")
    {
        AstrOs_Storage.saveFile("config.txt", cfig);
    }

    if (isMasterNode.load())
    {
        auto ackNak = success ? AstrOsSerialMessageType::FORMAT_SD_ACK : AstrOsSerialMessageType::FORMAT_SD_NAK;

        AstrOs_SerialMsgHandler.sendBasicAckNakResponse(ackNak, id, AstrOs_EspNow.getMac(), "master", "FORMAT");
    }
    else
    {
        auto ackNak = success ? AstrOsPacketType::FORMAT_SD_ACK : AstrOsPacketType::FORMAT_SD_NAK;
        AstrOs_EspNow.sendBasicAckNak(id, ackNak, "FORMAT");
    }
}

static void handleServoTest(astros_interface_response_t msg)
{
    auto message = std::string(msg.message);
    auto parts = AstrOsStringUtils::splitString(message, ':');

    if (parts.size() != 4)
    {
        ESP_LOGE(TAG, "Invalid servo test message: %s", message.c_str());
    }
    else
    {
        // parts[0] reserverd for module type
        int idx = std::stoi(parts[1]);
        int servo = std::stoi(parts[2]);
        int ms = std::stoi(parts[3]);

        // Snapshot the target module under maestroModulesMutex, then release
        // before calling SetServoPosition(): that path reaches sendQueueMsg()
        // which spins on a per-module mutex and blocks on queue sends.
        // Holding the map mutex across it would stall loadMaestroConfigs()
        // and other callers. The shared_ptr keeps the module alive for the
        // call even if a concurrent config reload removes it from the map.
        std::shared_ptr<MaestroModule> target;
        if (xSemaphoreTake(maestroModulesMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            auto it = maestroModules.find(idx);
            if (it == maestroModules.end())
            {
                ESP_LOGE(TAG, "Maestro module %d not found", idx);
            }
            else
            {
                target = it->second;
            }
            xSemaphoreGive(maestroModulesMutex);
        }
        else
        {
            ESP_LOGW(TAG, "handleServoTest: failed to acquire maestroModulesMutex within 1s");
        }

        if (target)
        {
            target->SetServoPosition(servo, ms);
        }
    }

    if (isMasterNode.load())
    {
        AstrOs_SerialMsgHandler.sendBasicAckNakResponse(AstrOsSerialMessageType::SERVO_TEST_ACK, msg.originationMsgId,
                                                        AstrOs_EspNow.getMac(), "master", "SERVO_TEST");
    }
    else
    {
        AstrOs_EspNow.sendBasicAckNak(msg.originationMsgId, AstrOsPacketType::SERVO_TEST_ACK, "SERVO_TEST");
    }
}