#include <stdio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_netif.h>
#include <driver/rmt.h>
#include <esp_log.h>
#include <driver/uart.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <esp_random.h>
#include <string.h>

#include <AstrOsSerialMsgHandler.hpp>
#include <AnimationController.hpp>
#include <AnimationCommand.hpp>
#include <AstrOsDisplay.hpp>
#include <SerialModule.hpp>
#include <ServoModule.hpp>
#include <I2cModule.hpp>
#include <GpioModule.hpp>
#include <AstrOsUtility.h>
#include <AstrOsUtility_Esp.h>
#include <AstrOsEspNow.h>
#include <AstrOsNames.h>
#include <AstrOsStorageManager.hpp>
#include <AstrOsInterfaceResponseMsg.hpp>
#include <guid.h>

static const char *TAG = AstrOsConstants::ModuleName;

/**********************************
 * States
 **********************************/
static int displayTimeout = 0;
static int defaultDisplayTimeout = 10;
static bool discoveryMode = false;
static bool isMasterNode = false;
static uart_port_t ASTRO_PORT = UART_NUM_0;
static std::string rank = "Padawan";


/**********************************
 * Time trackers
 **********************************/
static uint64_t animation_loop_start = 0;
static uint64_t animation_loop_end = 0;
static uint64_t animation_loop_drift = 0;

static uint64_t lastHeartBeat = 0;


/**********************************
 * queues
 **********************************/

static QueueHandle_t animationQueue;
static QueueHandle_t interfaceResponseQueue;
static QueueHandle_t serviceQueue;
static QueueHandle_t serialQueue;
static QueueHandle_t servoQueue;
static QueueHandle_t i2cQueue;
static QueueHandle_t gpioQueue;
static QueueHandle_t espnowQueue;

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
static esp_timer_handle_t animationTimer;
static esp_timer_handle_t servoMoveTimer;

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

#define BAUD_RATE_1 (9600)
#define BAUD_RATE_2 (9600)
#define BAUD_RATE_3 (9600)

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

// timers
static void initTimers(void);
static void pollingTimerCallback(void *arg);
static void animationTimerCallback(void *arg);
static void maintenanceTimerCallback(void *arg);
static void servoMoveTimerCallback(void *arg);

// espnow call backs
bool cachePeer(espnow_peer_t peer);
void updateSeviceConfig(std::string name, uint8_t *mac);
void displaySetDefault(std::string line1, std::string line2, std::string line3);
static void espnowSendCallback(const uint8_t *mac_addr, esp_now_send_status_t status);
static void espnowRecvCallback(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);

// tasks
void buttonListenerTask(void *arg);
void astrosRxTask(void *arg);
void serviceQueueTask(void *arg);
void interfaceResponseQueueTask(void *arg);
void animationQueueTask(void *arg);
void serialQueueTask(void *arg);
void servoQueueTask(void *arg);
void i2cQueueTask(void *arg);
void gpioQueueTask(void *arg);
void espnowQueueTask(void *arg);

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

extern "C"
{
    void app_main(void);
}

void app_main()
{
    init();

    // core 1
    xTaskCreatePinnedToCore(&buttonListenerTask, "button_listener_task", 2048, (void *)serviceQueue, 5, NULL, 1);
    xTaskCreatePinnedToCore(&serviceQueueTask, "service_queue_task", 3072, (void *)serviceQueue, 6, NULL, 1);
    xTaskCreatePinnedToCore(&animationQueueTask, "animation_queue_task", 4096, (void *)animationQueue, 7, NULL, 1);
    xTaskCreatePinnedToCore(&interfaceResponseQueueTask, "interface_queue_task", 4096, (void *)interfaceResponseQueue, 10, NULL, 1);
    xTaskCreatePinnedToCore(&serialQueueTask, "serial_queue_task", 3072, (void *)serialQueue, 9, NULL, 1);
    xTaskCreatePinnedToCore(&servoQueueTask, "servo_queue_task", 4096, (void *)servoQueue, 10, NULL, 1);
    xTaskCreatePinnedToCore(&i2cQueueTask, "i2c_queue_task", 3072, (void *)i2cQueue, 8, NULL, 1);
    xTaskCreatePinnedToCore(&gpioQueueTask, "gpio_queue_task", 3072, (void *)gpioQueue, 8, NULL, 1);

    // core 0
    xTaskCreatePinnedToCore(&astrosRxTask, "astros_rx_task", 4096, (void *)animationQueue, 9, NULL, 0);
    xTaskCreatePinnedToCore(&espnowQueueTask, "espnow_queue_task", 4096, (void *)espnowQueue, 10, NULL, 0);

    initTimers();
}

void init(void)
{
    ESP_LOGI(TAG, "init called");

    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_POSEDGE; // Interrupt on rising edge
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = 1ULL << RESET_PIN;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    animationQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_ani_cmd_t));
    serviceQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_svc_cmd_t));
    interfaceResponseQueue = xQueueCreate(QUEUE_LENGTH, sizeof(astros_interface_response_t));
    serialQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_msg_t));
    servoQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_msg_t));
    i2cQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_msg_t));
    gpioQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_msg_t));
    espnowQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_espnow_msg_t));

    ESP_ERROR_CHECK(AstrOs_Storage.Init());

    loadConfig();

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, RX_BUF_SIZE * 2, 0, 0, NULL, 0));

    AstrOs_SerialMsgHandler.Init(interfaceResponseQueue, serialQueue);
    ESP_LOGI(TAG, "AstrOs Interface initiated");

    ESP_ERROR_CHECK(i2cMaster.Init((gpio_num_t) SDA_PIN, (gpio_num_t) SCL_PIN));
    ESP_LOGI(TAG, "I2C Master initiated");

    serial_config_t serialConf;

    if (isMasterNode)
    {
        serialConf.baudRate1 = ASTROS_INTEFACE_BAUD_RATE;
    }
    else
    {
        serialConf.baudRate1 = 9600;
    }

    serialConf.txPin1 = TX_PIN_1;
    serialConf.rxPin1 = RX_PIN_1;
    serialConf.baudRate2 = BAUD_RATE_2;
    serialConf.txPin2 = TX_PIN_2;
    serialConf.rxPin2 = RX_PIN_2;
    serialConf.baudRate3 = BAUD_RATE_3;
    serialConf.txPin3 = TX_PIN_3;
    serialConf.rxPin3 = RX_PIN_3;

    ESP_ERROR_CHECK(SerialMod.Init(serialConf));
    ESP_LOGI(TAG, "Serial Module initiated");

    servo_module_config_t servoConfig = {
        .board0Addr = SERVO_BOARD_0_ADDR,
        .board0Freq = 50,
        .board0Slop = 3,
        .board1Addr = SERVO_BOARD_1_ADDR,
        .board1Freq = 50,
        .board1Slop = 4};

    ESP_ERROR_CHECK(ServoMod.Init(i2cMaster, servoConfig));
    ESP_LOGI(TAG, "Servo Module initiated");

    ESP_ERROR_CHECK(I2cMod.Init());
    ESP_LOGI(TAG, "I2C Module initiated");

    ESP_ERROR_CHECK(GpioMod.Init(
        {GPIO_PIN_0, GPIO_PIN_1, GPIO_PIN_2, GPIO_PIN_3, GPIO_PIN_4,
         GPIO_PIN_5, GPIO_PIN_6, GPIO_PIN_7, GPIO_PIN_8, GPIO_PIN_9}));
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

    astros_espnow_config_t config = {
        .masterMac = svcConfig.masterMacAddress,
        .name = name,
        .fingerprint = fingerprint,
        .isMaster = isMasterNode,
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
    AstrOs_Display.setDefault(rank, "", name);
}

/******************************************
 * timers
 *****************************************/

static void initTimers(void)
{
    const esp_timer_create_args_t pTimerArgs = {
        .callback = &pollingTimerCallback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "polling",
        .skip_unhandled_events = true};

    const esp_timer_create_args_t aTimerArgs = {
        .callback = &animationTimerCallback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "animation",
        .skip_unhandled_events = true};

    const esp_timer_create_args_t mTimerArgs = {
        .callback = &maintenanceTimerCallback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "maintenance",
        .skip_unhandled_events = true};

    // May need to make this a GPT timer?
    const esp_timer_create_args_t sTimerArgs = {
        .callback = &servoMoveTimerCallback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "servoMove",
        .skip_unhandled_events = true};

    ESP_ERROR_CHECK(esp_timer_create(&pTimerArgs, &pollingTimer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(pollingTimer, 2 * 1000 * 1000));
    ESP_LOGI("init_timer", "Started polling timer");

    ESP_ERROR_CHECK(esp_timer_create(&aTimerArgs, &animationTimer));
    ESP_ERROR_CHECK(esp_timer_start_once(animationTimer, 5 * 1000 * 1000));
    ESP_LOGI("init_timer", "Started animation timer");

    ESP_ERROR_CHECK(esp_timer_create(&mTimerArgs, &maintenanceTimer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(maintenanceTimer, 10 * 1000 * 1000));
    ESP_LOGI("init_timer", "Started maintenance timer");

    ESP_ERROR_CHECK(esp_timer_create(&sTimerArgs, &servoMoveTimer));
    ESP_ERROR_CHECK(esp_timer_start_once(servoMoveTimer, 500 * 1000));
    ESP_LOGI("init_timer", "Started servo move timer");
}

static void pollingTimerCallback(void *arg)
{
    int now = esp_timer_get_time();
    int seconds = (now - lastHeartBeat) / 1000;
    lastHeartBeat = now;
    ESP_LOGI(TAG, "Heartbeat, %d milliseconds since last", seconds);

    // only send register requests during discovery mode
    if (isMasterNode && !discoveryMode)
    {
        queue_espnow_msg_t msg;
        msg.data = nullptr;

        if (!polling)
        {
            msg.eventType = POLL_PADAWANS;
            polling = true;

            char *fingerprint = (char *)malloc(37);
            AstrOs_Storage.getControllerFingerprint(fingerprint);
            AstrOs_SerialMsgHandler.sendPollAckNak("00:00:00:00:00:00", "master",
                                                   std::string(fingerprint), true);
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
    else if (!isMasterNode && discoveryMode)
    {
        queue_espnow_msg_t msg;
        msg.data = nullptr;

        msg.eventType = SEND_REGISTRAION_REQ;

        if (xQueueSend(espnowQueue, &msg, pdMS_TO_TICKS(250)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Send espnow queue fail");
        }
    }

    if (!discoveryMode && displayTimeout > 0)
    {
        ESP_LOGD(TAG, "Display Timeout: %d", displayTimeout);
        displayTimeout -= 2;
        if (displayTimeout <= 0)
        {
            AstrOs_Display.displayClear();
        }
    }
}

static void maintenanceTimerCallback(void *arg)
{
    ESP_LOGI(TAG, "RAM left %lu", esp_get_free_heap_size());
}

static void animationTimerCallback(void *arg)
{
    esp_timer_stop(animationTimer);

    if (AnimationCtrl.scriptIsLoaded())
    {
        CommandTemplate *cmd = AnimationCtrl.getNextCommandPtr();

        CommandType ct = cmd->type;
        std::string val = cmd->val;

        switch (ct)
        {
        case CommandType::Kangaroo:
        case CommandType::GenericSerial:
        {
            ESP_LOGI(TAG, "Serial command val: %s", val.c_str());
            queue_msg_t msg;
            msg.data = (uint8_t *)malloc(val.size() + 1);
            memcpy(msg.data, val.c_str(), val.size());
            msg.data[val.size()] = '\0';

            if (xQueueSend(serialQueue, &msg, pdMS_TO_TICKS(2000)) != pdTRUE)
            {
                ESP_LOGW(TAG, "Send serial queue fail");
                free(msg.data);
            }
            break;
        }
        case CommandType::PWM:
        {
            ESP_LOGI(TAG, "PWM command val: %s", val.c_str());
            queue_msg_t servoMsg;
            servoMsg.data = (uint8_t *)malloc(val.size() + 1);
            memcpy(servoMsg.data, val.c_str(), val.size());
            servoMsg.data[val.size()] = '\0';

            if (xQueueSend(servoQueue, &servoMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
            {
                ESP_LOGW(TAG, "Send servo queue fail");
                free(servoMsg.data);
            }
            break;
        }
        case CommandType::I2C:
        {
            ESP_LOGI(TAG, "I2C command val: %s", val.c_str());
            queue_msg_t i2cMsg;
            i2cMsg.data = (uint8_t *)malloc(val.size() + 1);
            memcpy(i2cMsg.data, val.c_str(), val.size());
            i2cMsg.data[val.size()] = '\0';

            if (xQueueSend(i2cQueue, &i2cMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
            {
                ESP_LOGW(TAG, "Send i2c queue fail");
                free(i2cMsg.data);
            }
            break;
        }
        case CommandType::Gpio:
        {
            ESP_LOGI(TAG, "GPIO command val: %s", val.c_str());
            queue_msg_t gpioMsg;
            gpioMsg.data = (uint8_t *)malloc(val.size() + 1);
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

        delete (cmd);

        ESP_ERROR_CHECK(esp_timer_start_once(animationTimer, AnimationCtrl.msTillNextServoCommand() * 1000));
    }
    else
    {
        ESP_ERROR_CHECK(esp_timer_start_once(animationTimer, 250 * 1000));
    }
}


static void servoMoveTimerCallback(void *arg)
{
    // get time at start of loop
    auto loopStart = esp_timer_get_time();

    ServoMod.MoveServos();

    // get time at end of loop
    auto loopEnd = esp_timer_get_time();

    // start loop  so it will trigger 500 ms from the start of the loop
    int ms = SERVO_MOVE_INTERVAL - std::round((loopEnd - loopStart) / 1000);
    auto timeToNextMove = std::clamp(ms, 100, SERVO_MOVE_INTERVAL);
    ESP_ERROR_CHECK(esp_timer_start_once(servoMoveTimer, timeToNextMove * 1000));
}

/******************************************
 * tasks
 *****************************************/

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
                    if (discoveryMode)
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
                else if (!discoveryMode)
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
                AstrOs_SerialMsgHandler.sendPollAckNak(msg.peerMac, msg.peerName, msg.message, true);
                break;
            }
            case AstrOsInterfaceResponseType::SEND_POLL_NAK:
            {
                AstrOs_SerialMsgHandler.sendPollAckNak(msg.peerMac, msg.peerName, "", false);
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
                AstrOs_EspNow.sendBasicCommand(AstrOsPacketType::CONFIG, msg.peerMac, msg.originationMsgId, msg.message);
                break;
            }
            case AstrOsInterfaceResponseType::SAVE_SCRIPT:
            {
                handleSaveScript(msg);
                break;
            }
            case AstrOsInterfaceResponseType::SEND_SCRIPT:
            {
                AstrOs_EspNow.sendBasicCommand(AstrOsPacketType::SCRIPT_DEPLOY, msg.peerMac, msg.originationMsgId, msg.message);
                break;
            }
            case AstrOsInterfaceResponseType::SCRIPT_RUN:
            {
                handleRunSctipt(msg);
                break;
            }
            case AstrOsInterfaceResponseType::SEND_SCRIPT_RUN:
            {
                AstrOs_EspNow.sendBasicCommand(AstrOsPacketType::SCRIPT_RUN, msg.peerMac, msg.originationMsgId, msg.message);
                break;
            }
            case AstrOsInterfaceResponseType::COMMAND:
            {
                handleRunCommand(msg);
                break;
            }
            case AstrOsInterfaceResponseType::SEND_COMMAND:
            {
                AstrOs_EspNow.sendBasicCommand(AstrOsPacketType::COMMAND_RUN, msg.peerMac, msg.originationMsgId, msg.message);
                break;
            }
            case AstrOsInterfaceResponseType::PANIC_STOP:
            {
                handlePanicStop(msg);
                break;
            }
            case AstrOsInterfaceResponseType::SEND_PANIC_STOP:
            {
                AstrOs_EspNow.sendBasicCommand(AstrOsPacketType::PANIC_STOP, msg.peerMac, msg.originationMsgId, "PANIC");
                break;
            }
            case AstrOsInterfaceResponseType::FORMAT_SD:
            {
                auto id = std::string(msg.originationMsgId);

                queue_svc_cmd_t cmd;
                cmd.cmd = SERVICE_COMMAND::FORMAT_SD;
                cmd.data = (uint8_t *)malloc(id.size() + 1); // dummy data
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
                AstrOs_EspNow.sendBasicCommand(AstrOsPacketType::FORMAT_SD, msg.peerMac, msg.originationMsgId, "FORMATSD");
                break;
            }
            case AstrOsInterfaceResponseType::SEND_CONFIG_ACK:
            {
                auto responseType = getSerialMessageType(msg.type);
                AstrOs_SerialMsgHandler.sendBasicAckNakResponse(responseType, msg.originationMsgId, msg.peerMac, msg.peerName, msg.message);
                break;
            }
            case AstrOsInterfaceResponseType::SERVO_TEST:{
                handleServoTest(msg);
            }
            case AstrOsInterfaceResponseType::SEND_SERVO_TEST:
            {
                AstrOs_EspNow.sendBasicCommand(AstrOsPacketType::SERVO_TEST, msg.peerMac, msg.originationMsgId, msg.message);
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
                AstrOs_SerialMsgHandler.sendBasicAckNakResponse(responseType, msg.originationMsgId, msg.peerMac, msg.peerName, msg.message);
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

    size_t bufferLength = 2000;
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
                    ESP_LOGI("AstrOs RX", "Read %d bytes: '%s'", bufferIndex, commandBuffer);

                    AstrOs_SerialMsgHandler.handleMessage(std::string(reinterpret_cast<char *>(commandBuffer), bufferIndex));

                    bufferIndex = 0;
                }
                else
                {
                    commandBuffer[bufferIndex] = data[i];
                    bufferIndex++;
                    if (bufferIndex >= bufferLength)
                    {
                        ESP_LOGW("AstrOs RX", "Buffer overflow");
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
            }
            case SERVICE_COMMAND::SHOW_DISPLAY:
            {
                AstrOs_Display.displayDefault();
                displayTimeout = defaultDisplayTimeout;
                break;
            }
            case SERVICE_COMMAND::ESPNOW_DISCOVERY_MODE_ON:
            {
                discoveryMode = true;
                AstrOs_Display.displayUpdate("Discovery", "Mode On");
                ESP_LOGI(TAG, "Discovery mode on");
                break;
            }
            case SERVICE_COMMAND::ESPNOW_DISCOVERY_MODE_OFF:
            {
                discoveryMode = false;
                AstrOs_Display.displayDefault();
                displayTimeout = defaultDisplayTimeout;
                ESP_LOGI(TAG, "Discovery mode off");
                break;
            }
            case SERVICE_COMMAND::ASTROS_INTERFACE_MESSAGE:
            {
                queue_msg_t serialMsg;
                serialMsg.message_id = 1;
                serialMsg.data = (uint8_t *)malloc(msg.dataSize + 1);
                memcpy(serialMsg.data, msg.data, msg.dataSize);
                serialMsg.data[msg.dataSize] = '\n';
                serialMsg.dataSize = msg.dataSize + 1;

                if (xQueueSend(serialQueue, &serialMsg, pdMS_TO_TICKS(500)) != pdTRUE)
                {
                    ESP_LOGW(TAG, "Sending AstrOs Interface message to serial queue fail");
                    free(serialMsg.data);
                }

                break;
            }
            case SERVICE_COMMAND::RELOAD_SERVO_CONFIG:
            {
                ServoMod.LoadServoConfig();
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

void serialQueueTask(void *arg)
{
    QueueHandle_t serialQueue;

    serialQueue = (QueueHandle_t)arg;
    queue_msg_t msg;

    while (1)
    {
        if (xQueueReceive(serialQueue, &(msg), 0))
        {
            auto highWaterMark = uxTaskGetStackHighWaterMark(NULL);
            if (highWaterMark < 500)
            {
                ESP_LOGW(TAG, "Serial Queue Stack HWM: %d", highWaterMark);
            }

            if (msg.message_id == 0)
            {
                SerialMod.SendCommand(msg.data);
            }
            else if (msg.message_id == 1)
            {
                std::string str(reinterpret_cast<char *>(msg.data), msg.dataSize);

                ESP_LOGD(TAG, "Serial message: %s", str.c_str());
                SerialMod.SendBytes(1, msg.data, msg.dataSize);
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
            ServoMod.QueueCommand(msg.data);

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

            ESP_LOGD(TAG, "I2C Command received on queue => %d, %s", msg.message_id, msg.data);

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

void espnowQueueTask(void *arg)
{
    QueueHandle_t espnowQueue;

    espnowQueue = (QueueHandle_t)arg;

    vTaskDelay(pdMS_TO_TICKS(5 * 1000));

    ESP_LOGI(TAG, "ESP-NOW Queue started");

    AstrOs_Display.displayDefault();
    displayTimeout = defaultDisplayTimeout;

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
                    if (!discoveryMode)
                    {
                        break;
                    }

                    AstrOs_EspNow.handleMessage(msg.src, msg.data, msg.data_len);
                }
                else if (esp_now_is_peer_exist(msg.src))
                {
                    ESP_LOGD(TAG, "Received unicast data from peer: " MACSTR ", len: %d", MAC2STR(msg.src), msg.data_len);

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

/**********************************
 * Configuration Methods
 *********************************/

static void loadConfig()
{
    auto cfig = AstrOs_Storage.readFile("config.txt");

    if (cfig != "error")
    {
        ESP_LOGI(TAG, "Service config file: %s", cfig.c_str());

        auto lines = AstrOsStringUtils::splitString(cfig, '\n');

        for (auto line : lines)
        {
            auto parts = AstrOsStringUtils::splitString(line, '=');

            if (parts.size() == 2)
            {
                if (parts[0] == "isMasterNode")
                {
                    isMasterNode = parts[1] == "true";
                    ASTRO_PORT = isMasterNode ? UART_NUM_1 : UART_NUM_0;
                    rank = isMasterNode ? "Master" : "Padawan";
                }
            }
        }
    }
    else
    {
        ESP_LOGW(TAG, "Service config file not found");
    }
}

/**********************************
 * callbacks
 *********************************/

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

static void espnowSendCallback(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (mac_addr == NULL)
    {
        ESP_LOGE(TAG, "Send cb arg error");
        return;
    }

    ESP_LOGD(TAG, "Sending data to " MACSTR " status: %d", MAC2STR(mac_addr), status);
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

/**********************************
 * Interface Response Methods
 *********************************/
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
    auto success = AstrOs_Storage.saveServoConfig(msg.message);

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
        reloadMsg.cmd = SERVICE_COMMAND::RELOAD_SERVO_CONFIG;
        reloadMsg.data = nullptr;

        if (xQueueSend(serviceQueue, &reloadMsg, pdMS_TO_TICKS(500)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Send servo reload fail");
        }
    }

    if (isMasterNode)
    {
        auto ackNak = success ? AstrOsSerialMessageType::DEPLOY_CONFIG_ACK : AstrOsSerialMessageType::DEPLOY_CONFIG_NAK;

        AstrOs_SerialMsgHandler.sendBasicAckNakResponse(ackNak, msg.originationMsgId, AstrOs_EspNow.getMac(),
                                                        "master", AstrOs_EspNow.getFingerprint());
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

    if (isMasterNode)
    {
        auto ackNak = success ? AstrOsSerialMessageType::DEPLOY_SCRIPT_ACK : AstrOsSerialMessageType::DEPLOY_SCRIPT_NAK;

        AstrOs_SerialMsgHandler.sendBasicAckNakResponse(ackNak, msg.originationMsgId, AstrOs_EspNow.getMac(), "master", parts[0]);
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

    if (isMasterNode)
    {
        auto ackNak = success ? AstrOsSerialMessageType::RUN_SCRIPT_ACK : AstrOsSerialMessageType::RUN_SCRIPT_NAK;

        AstrOs_SerialMsgHandler.sendBasicAckNakResponse(ackNak, msg.originationMsgId, AstrOs_EspNow.getMac(), "master", parts[0]);
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

    auto success = AstrOs_Storage.formatSdCard();

    if (cfig != "error")
    {
        AstrOs_Storage.saveFile("config.txt", cfig);
    }

    if (isMasterNode)
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
    auto parts = AstrOsStringUtils::splitString(message, UNIT_SEPARATOR);

    if (parts.size() != 3)
    {
        ESP_LOGE(TAG, "Invalid servo test message: %s", message.c_str());
    }
    else
    {
        int board= std::stoi(parts[0]);
        int servo = std::stoi(parts[1]);
        int ms = std::stoi(parts[2]);
        ServoMod.SetServoPosition(board, servo, ms);
    }

    if (isMasterNode)
    {
        AstrOs_SerialMsgHandler.sendBasicAckNakResponse(AstrOsSerialMessageType::SERVO_TEST_ACK, msg.originationMsgId, AstrOs_EspNow.getMac(), "master", "SERVO_TEST");
    }
    else
    {
        AstrOs_EspNow.sendBasicAckNak(msg.originationMsgId, AstrOsPacketType::SERVO_TEST_ACK, "SERVO_TEST");
    }
}