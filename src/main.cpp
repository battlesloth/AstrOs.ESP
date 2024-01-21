#include <stdio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>
#include "esp_timer.h"
#include "esp_netif.h"
#include <driver/rmt.h>
#include <esp_log.h>
#include <driver/uart.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <esp_random.h>

#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_crc.h"
#include "esp_wifi_types.h"

#include <AstrOsInterface.h>
#include <KangarooInterface.h>
#include <AnimationController.h>
#include <AnimationCommand.h>
#include <DisplayCommand.h>
#include <SerialModule.h>
#include <ServoModule.h>
#include <I2cModule.h>
#include <AstrOsUtility.h>
#include <AstrOsEspNow.h>
#include <AstrOsEspNowUtility.h>
#include <AstrOsNetwork.h>
#include <AstrOsConstants.h>
#include <AstrOsNames.h>
#include <StorageManager.h>

static const char *TAG = AstrOsConstants::ModuleName;

/**********************************
 * Jedi Knight or Padawan
 **********************************/
#ifdef JEDI_KNIGHT
#define isMaster true
#define rank "Jedi Knight"
#else
#define isMaster false
#define rank "Padawan"
#endif

/**********************************
 * queues
 **********************************/

static QueueHandle_t animationQueue;
static QueueHandle_t hardwareQueue;
static QueueHandle_t serviceQueue;
static QueueHandle_t serialQueue;
static QueueHandle_t servoQueue;
static QueueHandle_t i2cQueue;
static QueueHandle_t espnowQueue;

/**********************************
 * UART
 **********************************/

static const int RX_BUF_SIZE = 1024;

#define ASTRO_PORT UART_NUM_0
#define KANGAROO_PORT UART_NUM_1

/**********************************
 * timers
 **********************************/

static esp_timer_handle_t maintenanceTimer;
static esp_timer_handle_t animationTimer;
static esp_timer_handle_t heartbeatTimer;

/**********************************
 * animation
 **********************************/
#define QUEUE_LENGTH 5

/**********************************
 * Reset Button
 **********************************/
#define RESET_GPIO (GPIO_NUM_13)
#define MEDIUM_PRESS_THRESHOLD_MS 3000 // 3 second
#define LONG_PRESS_THRESHOLD_MS 10000  // 10 seconds

/**********************************
 * Kangaroo Interface
 **********************************/

#ifdef DARTHSERVO
#define KI_TX_PIN (GPIO_NUM_16)
#define KI_RX_PIN (GPIO_NUM_3)
#else
#define BAUD_RATE_1 (9600)
#define TX_PIN_1 (GPIO_NUM_2)
#define RX_PIN_1 (GPIO_NUM_0)
#define BAUD_RATE_2 (9600)
#define TX_PIN_2 (GPIO_NUM_32)
#define RX_PIN_2 (GPIO_NUM_33)
#define BAUD_RATE_3 (9600)
#define TX_PIN_3 (GPIO_NUM_12)
#define RX_PIN_3 (GPIO_NUM_13)
#endif

/**********************************
 * I2C Settings
 **********************************/
#ifdef DARTHSERVO
#define SDA_PIN (GPIO_NUM_18)
#define SCL_PIN (GPIO_NUM_17)
#else
#define SDA_PIN (GPIO_NUM_21)
#define SCL_PIN (GPIO_NUM_22)
#endif
#define I2C_PORT I2C_NUM_0

/**********************************
 * Servo Settings
 **********************************/
#ifdef DARTHSERVO
#define SERVO_BOARD_0_ADDR 0x40
#define SERVO_BOARD_1_ADDR 0x41
#else
#define SERVO_BOARD_0_ADDR 0x40
#define SERVO_BOARD_1_ADDR 0x41
#endif

/**********************************
 * ESP-NOW
 **********************************/

static SemaphoreHandle_t masterMacMutex;
static uint8_t master_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/**********************************
 * Method definitions
 *********************************/
void init(void);

static void initTimers(void);
static void animationTimerCallback(void *arg);
static void maintenanceTimerCallback(void *arg);
static void heartbeatTimerCallback(void *arg);

// ESP-NOW
static esp_err_t wifiInit(void);
static esp_err_t espnowInit(void);
static void espnowDeinit();
static void espnowSendCallback(const uint8_t *mac_addr, esp_now_send_status_t status);
static void espnowRecvCallback(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);
int espnowDataParse(uint8_t *data, int data_len, uint8_t *state, uint16_t *seq, int *magic);
void espnowDataPrepare(uint8_t *data);

void buttonListenerTask(void *arg);
void astrosRxTask(void *arg);
void serviceQueueTask(void *arg);
void hardwareQueueTask(void *arg);
void animationQueueTask(void *arg);
void serialQueueTask(void *arg);
void servoQueueTask(void *arg);
void i2cQueueTask(void *arg);
void espnowQueueTask(void *arg);

esp_err_t mountSdCard(void);

extern "C"
{
    void app_main(void);
}

void app_main()
{
    init();

    xTaskCreate(&buttonListenerTask, "button_listener_task", 2048, (void *)serviceQueue, 10, NULL);
    xTaskCreate(&serviceQueueTask, "service_queue_task", 3072, (void *)serviceQueue, 10, NULL);
    xTaskCreate(&animationQueueTask, "animation_queue_task", 4096, (void *)animationQueue, 10, NULL);
    xTaskCreate(&hardwareQueueTask, "hardware_queue_task", 4096, (void *)hardwareQueue, 10, NULL);
    xTaskCreate(&serialQueueTask, "serial_queue_task", 3072, (void *)serialQueue, 10, NULL);
    xTaskCreate(&servoQueueTask, "servo_queue_task", 4096, (void *)servoQueue, 10, NULL);
    xTaskCreate(&i2cQueueTask, "i2c_queue_task", 3072, (void *)i2cQueue, 10, NULL);
    xTaskCreate(&astrosRxTask, "astros_rx_task", 2048, (void *)animationQueue, 10, NULL);
    xTaskCreate(&espnowQueueTask, "espnow_queue_task", 4096, (void *)espnowQueue, 10, NULL);

    initTimers();
}

void init(void)
{
    ESP_LOGI(TAG, "init called");

    masterMacMutex = xSemaphoreCreateMutex();
    if (masterMacMutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize the serial mutex");
    }

    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_POSEDGE; // Interrupt on rising edge
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = 1ULL << RESET_GPIO;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    animationQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_ani_cmd_t));
    serviceQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_svc_cmd_t));
    hardwareQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_hw_cmd_t));
    serialQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_msg_t));
    servoQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_msg_t));
    i2cQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_msg_t));
    espnowQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_espnow_msg_t));

    ESP_ERROR_CHECK(Storage.Init());

    uint8_t mac[ESP_NOW_ETH_ALEN] = {0};

    if (Storage.loadMasterMacAddress(mac))
    {
        ESP_LOGI(TAG, "Master MAC address loaded from storage: " MACSTR, MAC2STR(mac));
        memcpy(master_mac, mac, ESP_NOW_ETH_ALEN);
    }
    else
    {
        ESP_LOGI(TAG, "Master MAC address not found in storage");
    }

    ESP_ERROR_CHECK(uart_driver_install(ASTRO_PORT, RX_BUF_SIZE * 2, 0, 0, NULL, 0));

    AstrOs.Init(animationQueue);
    ESP_LOGI(TAG, "AstrOs Interface initiated");

    i2c_config_t conf;

    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = SDA_PIN;
    conf.scl_io_num = SCL_PIN;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 100000;
    conf.clk_flags = 0;

    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0));

    serial_config_t serialConf;

    serialConf.baudRate1 = 9600;
    serialConf.txPin1 = TX_PIN_1;
    serialConf.rxPin1 = RX_PIN_1;
    serialConf.baudRate2 = 9600;
    serialConf.txPin2 = TX_PIN_2;
    serialConf.rxPin2 = RX_PIN_2;
    serialConf.baudRate3 = 9600;
    serialConf.txPin3 = TX_PIN_3;
    serialConf.rxPin3 = RX_PIN_3;

    ESP_ERROR_CHECK(SerialMod.Init(serialConf));
    ESP_LOGI(TAG, "Serial Module initiated");

    ESP_ERROR_CHECK(ServoMod.Init(SERVO_BOARD_0_ADDR, SERVO_BOARD_1_ADDR));
    ESP_LOGI(TAG, "Servo Module initiated");

    ESP_ERROR_CHECK(I2cMod.Init());
    ESP_LOGI(TAG, "I2C Module initiated");

    ESP_ERROR_CHECK(wifiInit());
    ESP_ERROR_CHECK(espnowInit());

    AstrOs_EspNow.init();
}

/******************************************
 * timers
 *****************************************/

static void initTimers(void)
{
    const esp_timer_create_args_t aTimerArgs = {
        .callback = &animationTimerCallback,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "animation",
        .skip_unhandled_events = true};

    const esp_timer_create_args_t mTimerArgs = {
        .callback = &maintenanceTimerCallback,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "maintenance",
        .skip_unhandled_events = true};

    const esp_timer_create_args_t heartbeatTimerArgs = {
        .callback = &heartbeatTimerCallback,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "heartbeat",
        .skip_unhandled_events = true};

    ESP_ERROR_CHECK(esp_timer_create(&aTimerArgs, &animationTimer));
    ESP_ERROR_CHECK(esp_timer_start_once(animationTimer, 5 * 1000 * 1000));
    ESP_LOGI("init_timer", "Started animation timer");

    ESP_ERROR_CHECK(esp_timer_create(&mTimerArgs, &maintenanceTimer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(maintenanceTimer, 10 * 1000 * 1000));
    ESP_LOGI("init_timer", "Started maintenance timer");

    ESP_ERROR_CHECK(esp_timer_create(&heartbeatTimerArgs, &heartbeatTimer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(heartbeatTimer, 2 * 1000 * 1000));
}

static void heartbeatTimerCallback(void *arg)
{
    ESP_LOGI(TAG, "Heartbeat");

    if (!isMaster) // && !IS_BROADCAST_ADDR(master_mac))
    {
        queue_espnow_msg_t msg;
        msg.id = ESPNOW_SEND_HEARTBEAT;
        msg.data = (uint8_t *)malloc(11);
        msg.data_len = 11;
        memcpy(msg.data, "heartbeat", 10);

        if (xQueueSend(espnowQueue, &msg, pdMS_TO_TICKS(2000)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Send espnow queue fail");
            free(msg.data);
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
            queue_msg_t msg = {0, 0};
            strncpy(msg.data, val.c_str(), sizeof(msg.data));
            msg.data[sizeof(msg.data) - 1] = '\0';
            xQueueSend(serialQueue, &msg, pdMS_TO_TICKS(2000));
            break;
        }
        case CommandType::PWM:
        {
            ESP_LOGI(TAG, "PWM command val: %s", val.c_str());
            queue_msg_t servoMsg = {0, 0};
            strncpy(servoMsg.data, val.c_str(), sizeof(servoMsg.data));
            servoMsg.data[sizeof(servoMsg.data) - 1] = '\0';
            xQueueSend(servoQueue, &servoMsg, pdMS_TO_TICKS(2000));
            break;
        }
        case CommandType::I2C:
        {
            ESP_LOGI(TAG, "I2C command val: %s", val.c_str());
            queue_msg_t i2cMsg = {0, 0};
            strncpy(i2cMsg.data, val.c_str(), sizeof(i2cMsg.data));
            i2cMsg.data[sizeof(i2cMsg.data) - 1] = '\0';
            xQueueSend(i2cQueue, &i2cMsg, pdMS_TO_TICKS(2000));
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

/******************************************
 * tasks
 *****************************************/

void buttonListenerTask(void *arg)
{
    uint32_t buttonStartTime = 0;
    bool discoveryModeActive = false;
    bool buttonHandled = true;

    while (1)
    {
        if (gpio_get_level(RESET_GPIO) == 0)
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

                    Storage.clearEspNowPeerConfig();

                    esp_restart();
                }
                else if ((xTaskGetTickCount() - buttonStartTime) > pdMS_TO_TICKS(MEDIUM_PRESS_THRESHOLD_MS))
                {
                    queue_espnow_msg_t msg;
                    msg.data = (uint8_t *)malloc(sizeof(uint8_t));
                    if (discoveryModeActive)
                    {
                        msg.id = ESPNOW_DISCOVERY_MODE_OFF;
                        discoveryModeActive = false;
                    }
                    else
                    {
                        msg.id = ESPNOW_DISCOVERY_MODE_ON;
                        discoveryModeActive = true;
                    }

                    if (xQueueSend(espnowQueue, &msg, pdMS_TO_TICKS(2000)) != pdTRUE)
                    {
                        ESP_LOGW(TAG, "Send espnow queue fail");
                        free(msg.data);
                    }

                    ESP_LOGI(TAG, "Discovery Switch Sent");
                }
            }

            buttonStartTime = 0;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void astrosRxTask(void *arg)
{

    uint8_t *data = (uint8_t *)malloc(RX_BUF_SIZE + 1);
    while (1)
    {

        const int rxBytes = uart_read_bytes(ASTRO_PORT, data, RX_BUF_SIZE, pdMS_TO_TICKS(1000));

        if (rxBytes > 0)
        {
            ESP_LOGI(TAG, "AstrOs RX Stack HWM: %d", uxTaskGetStackHighWaterMark(NULL));

            data[rxBytes] = '\0';
            ESP_LOGI("AstrOs RX", "Read %d bytes: '%s'", rxBytes, data);

            char msg[rxBytes];
            memcpy(msg, data, rxBytes);
            AstrOs.handleMessage(msg);
        }
    }
    free(data);
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
            ESP_LOGI(TAG, "Service Queue Stack HWM: %d", uxTaskGetStackHighWaterMark(NULL));

            switch (msg.cmd)
            {
            case SERVICE_COMMAND::SWITCH_TO_DISCOVERY:
                ESP_LOGI(TAG, "Switch to discovery requested");
                break;
            default:
                break;
            }
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
            ESP_LOGI(TAG, "Animation Queue Stack HWM: %d", uxTaskGetStackHighWaterMark(NULL));
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

void hardwareQueueTask(void *arg)
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

void serialQueueTask(void *arg)
{
    QueueHandle_t serialQueue;

    serialQueue = (QueueHandle_t)arg;
    queue_msg_t msg;

    while (1)
    {
        if (xQueueReceive(serialQueue, &(msg), 0))
        {
            ESP_LOGI(TAG, "Serial Queue Stack HWM: %d", uxTaskGetStackHighWaterMark(NULL));
            ESP_LOGI(TAG, "Serial command received on queue => %s", msg.data);
            SerialMod.SendCommand(msg.data);
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
            ESP_LOGI(TAG, "Servo Queue Stack HWM: %d", uxTaskGetStackHighWaterMark(NULL));
            ESP_LOGI(TAG, "Servo Command received on queue => %s", msg.data);
            ServoMod.QueueCommand(msg.data);
        }

        ServoMod.MoveServos();

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
            ESP_LOGI(TAG, "I2C Queue Stack HWM: %d", uxTaskGetStackHighWaterMark(NULL));
            ESP_LOGI(TAG, "I2C Command received on queue => %s", msg.data);
            if (msg.message_id == 0)
            {
                I2cMod.SendCommand(msg.data);
            }
            else if (msg.message_id == 1)
            {
                I2cMod.WriteDisplay(msg.data);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void espnowQueueTask(void *arg)
{
    QueueHandle_t espnowQueue;

    espnowQueue = (QueueHandle_t)arg;

    vTaskDelay(pdMS_TO_TICKS(5 * 1000));

    queue_hw_cmd_t displayUpdate = {HARDWARE_COMMAND::DISPLAY_COMMAND, NULL};
    DisplayCommand cmd = DisplayCommand();
    cmd.setValue("", rank, "");
    strncpy(displayUpdate.data, cmd.toString().c_str(), sizeof(displayUpdate.data));
    displayUpdate.data[sizeof(displayUpdate.data) - 1] = '\0';
    xQueueSend(hardwareQueue, &displayUpdate, pdMS_TO_TICKS(2000));

    // Add broadcast peer information to peer list.
    AstrOs_EspNow.addPeer(broadcast_mac);

    int peerCount = 0;
    espnow_peer_t peerList[10] = {};

    // Load current peer list.
    peerCount = Storage.loadEspNowPeerConfigs(peerList);

    ESP_LOGI(TAG, "Loaded %d peers", peerCount);

    for (int i = 0; i < peerCount; i++)
    {
        AstrOs_EspNow.addPeer(peerList[i].mac_addr);
    }

    bool discoveryMode = false;
    queue_espnow_msg_t msg;

    while (1)
    {
        if (xQueueReceive(espnowQueue, &(msg), 0))
        {
            ESP_LOGI(TAG, "ESP-NOW Queue Stack HWM: %d", uxTaskGetStackHighWaterMark(NULL));

            switch (msg.id)
            {
            case ESPNOW_DISCOVERY_MODE_ON:
            {
                discoveryMode = true;
                queue_hw_cmd_t msg = {HARDWARE_COMMAND::DISPLAY_COMMAND, NULL};
                DisplayCommand cmd = DisplayCommand();
                cmd.setValue("Discovery", "", "Mode On");
                strncpy(msg.data, cmd.toString().c_str(), sizeof(msg.data));
                msg.data[sizeof(msg.data) - 1] = '\0';
                xQueueSend(hardwareQueue, &msg, pdMS_TO_TICKS(2000));
                ESP_LOGI(TAG, "Discovery mode on");
                break;
            }
            case ESPNOW_DISCOVERY_MODE_OFF:
            {
                discoveryMode = false;
                queue_hw_cmd_t msg = {HARDWARE_COMMAND::DISPLAY_COMMAND, NULL};
                DisplayCommand cmd = DisplayCommand();
                cmd.setValue("", rank, "");
                strncpy(msg.data, cmd.toString().c_str(), sizeof(msg.data));
                msg.data[sizeof(msg.data) - 1] = '\0';
                xQueueSend(hardwareQueue, &msg, pdMS_TO_TICKS(2000));
                ESP_LOGI(TAG, "Discovery mode off");
                break;
            }
            case ESPNOW_SEND_HEARTBEAT:
            {
                uint8_t *destMac = (uint8_t *)malloc(ESP_NOW_ETH_ALEN);
                bool gotMac = false;

                while (!gotMac)
                {
                    if (xSemaphoreTake(masterMacMutex, 100 / portTICK_PERIOD_MS))
                    {
                        memcpy(destMac, master_mac, ESP_NOW_ETH_ALEN);
                        gotMac = true;
                        xSemaphoreGive(masterMacMutex);
                    }
                    else
                    {
                        vTaskDelay(10 / portTICK_PERIOD_MS);
                    }
                }

                if (esp_now_send(destMac, msg.data, msg.data_len) != ESP_OK)
                {
                    ESP_LOGE(TAG, "Send error");
                }

                free(destMac);

                break;
            }
            case ESPNOW_SEND:
            {
                ESP_LOGI(TAG, "Send data to " MACSTR, MAC2STR(msg.dest));

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
                    ESP_LOGI(TAG, "Received broadcast data from: " MACSTR ", len: %d", MAC2STR(msg.src), msg.data_len);

                    /* If MAC address does not exist in peer list, add it to peer list. */
                    if (esp_now_is_peer_exist(msg.src) == false && discoveryMode)
                    {
                        if (peerCount == ESPNOW_PEER_LIMIT)
                        {
                            ESP_LOGI(TAG, "Max peers have already been added.");
                            break;
                        }

                        ESP_LOGI(TAG, "Adding new peer:" MACSTR, MAC2STR(msg.src));

                        AstrOs_EspNow.addPeer(msg.src);

                        // add to peer cache
                        espnow_peer_t newPeer;

                        // peer id is 0 indexed
                        newPeer.id = peerCount;

                        if (isMaster)
                        {
                            std::string name = astrOsGetName(newPeer.id);
                            char nameBuf[16];

                            memcpy(newPeer.name, name.c_str(), name.length() + 1);
                        }
                        else
                        {
                            memcpy(newPeer.name, "master\0", 8);
                        }

                        memcpy(newPeer.mac_addr, msg.src, ESP_NOW_ETH_ALEN);
                        memset(newPeer.crypto_key, 0, ESP_NOW_KEY_LEN);
                        newPeer.is_paired = true;

                        Storage.saveEspNowPeer(newPeer);

                        peerList[peerCount] = newPeer;

                        peerCount++;

                        if (isMaster)
                        { // send registration message
                            queue_espnow_msg_t regMsg = AstrOs_EspNow.generateRegisterMessage(broadcast_mac);

                            if (xQueueSend(espnowQueue, &regMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
                            {
                                ESP_LOGW(TAG, "Send espnow queue fail");
                                free(regMsg.data);
                            }
                        }
                        else
                        {
                            uint8_t cachedMac[ESP_NOW_ETH_ALEN] = {0};
                            memccpy(cachedMac, msg.src, 0, ESP_NOW_ETH_ALEN);

                            Storage.saveMasterMacAddress(cachedMac);

                            bool macSet = false;
                            while (!macSet)
                            {
                                if (xSemaphoreTake(masterMacMutex, 100 / portTICK_PERIOD_MS))
                                {
                                    memcpy(master_mac, msg.src, ESP_NOW_ETH_ALEN);
                                    macSet = true;
                                    xSemaphoreGive(masterMacMutex);
                                }
                                else
                                {
                                    vTaskDelay(10 / portTICK_PERIOD_MS);
                                }
                            }
                        }
                    }
                    // TODO: test code, remove
                    else if (discoveryMode && isMaster)
                    {
                        // send registration message
                        queue_espnow_msg_t regMsg;
                        regMsg.id = ESPNOW_SEND;
                        memccpy(regMsg.dest, msg.src, 0, ESP_NOW_ETH_ALEN);
                        regMsg.data = (uint8_t *)malloc(11);
                        regMsg.data_len = 11;
                        memcpy(regMsg.data, "registered", 10);

                        if (xQueueSend(espnowQueue, &regMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
                        {
                            ESP_LOGW(TAG, "Send espnow queue fail");
                            free(regMsg.data);
                        }
                    }
                }
                else if (esp_now_is_peer_exist(msg.src))
                {
                    ESP_LOGI(TAG, "Received unicast data from peer: " MACSTR ", len: %d", MAC2STR(msg.src), msg.data_len);
                }
                else
                {
                    ESP_LOGI(TAG, "Peer does not exist:" MACSTR, MAC2STR(msg.src));
                }
                break;
            }
            default:
                ESP_LOGE(TAG, "Callback type error: %d", msg.id);
                break;
            }

            free(msg.data);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/******************************************
 * ESP-NOW
 *****************************************/

int espnowDataParse(uint8_t *data, int data_len, uint8_t *state, uint16_t *seq, int *magic)
{
    if (data_len < 4)
    {
        ESP_LOGE(TAG, "Data length too short");
        return -1;
    }

    *state = data[0];
    *seq = data[1] | (data[2] << 8);
    *magic = data[3];

    return 0;
}

static void espnowSendCallback(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (mac_addr == NULL)
    {
        ESP_LOGE(TAG, "Send cb arg error");
        return;
    }

    ESP_LOGI(TAG, "Sending data to " MACSTR " status: %d", MAC2STR(mac_addr), status);
}

static void espnowRecvCallback(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    ESP_LOGI(TAG, "espnowRecvCallback called");

    queue_espnow_msg_t msg;

    uint8_t *src_addr = recv_info->src_addr;
    uint8_t *dest_addr = recv_info->des_addr;

    if (src_addr == NULL || data == NULL || len <= 0)
    {
        ESP_LOGE(TAG, "Receive cb arg error");
        return;
    }

    if (IS_BROADCAST_ADDR(dest_addr))
    {
        ESP_LOGI(TAG, "Receive broadcast ESPNOW data");
    }
    else
    {
        ESP_LOGI(TAG, "Receive unicast ESPNOW data");
    }

    msg.id = ESPNOW_RECV;
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
    if (xQueueSend(espnowQueue, &msg, ESPNOW_MAXDELAY) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send receive queue fail");
        free(msg.data);
    }
}

/* Parse received ESPNOW data. */
int espnowDataParse(uint8_t *data, uint16_t data_len, uint8_t *state, uint16_t *seq, int *magic)
{
    /*espnow_data_t *buf = (espnow_data_t *)data;
    uint16_t crc, crc_cal = 0;

    if (data_len < sizeof(espnow_data_t))
    {
        ESP_LOGE(TAG, "Receive ESPNOW data too short, len:%d", data_len);
        return -1;
    }

    *state = buf->state;
    *seq = buf->seq_num;
    *magic = buf->magic;
    crc = buf->crc;
    buf->crc = 0;
    crc_cal = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, data_len);

    if (crc_cal == crc)
    {
        return buf->type;
    }
*/
    return -1;
}

/*Prepare ESPNOW data to be sent.
void espnowDataPrepare(espnow_send_param_t *send_param)
{
    espnow_data_t *buf = (espnow_data_t *)send_param->buffer;

    assert(send_param->len >= sizeof(espnow_data_t));

    buf->type = IS_BROADCAST_ADDR(send_param->dest_mac) ? ESPNOW_DATA_BROADCAST : ESPNOW_DATA_UNICAST;
    buf->state = send_param->state;
    buf->seq_num = espnow_seq[buf->type]++;
    buf->crc = 0;
    buf->magic = send_param->magic;
    // Fill all remaining bytes after the data with random values
    esp_fill_random(buf->payload, send_param->len - sizeof(espnow_data_t));
    buf->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, send_param->len);
}
*/
static esp_err_t wifiInit(void)
{

    ESP_LOGI(TAG, "wifiInit called");

    esp_err_t err = ESP_OK;

    err = esp_netif_init();
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = esp_event_loop_create_default();
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    err = esp_wifi_init(&cfg);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = esp_wifi_set_mode(ESPNOW_WIFI_MODE);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = esp_wifi_start();
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    err = esp_wifi_set_protocol(ESPNOW_WIFI_IF, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    ESP_LOGI(TAG, "wifiInit complete");

    return err;
}

static esp_err_t espnowInit(void)
{

    ESP_LOGI(TAG, "espnowInit called");

    esp_err_t err = ESP_OK;

    err = esp_now_init();
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = esp_now_register_send_cb(espnowSendCallback);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = esp_now_register_recv_cb(espnowRecvCallback);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    ESP_LOGI(TAG, "espnowInit called");

    return err;
}

static void espnowDeinit()
{
    ESP_LOGI(TAG, "espnowDeinit called");
    vSemaphoreDelete(espnowQueue);
    esp_now_deinit();
}
