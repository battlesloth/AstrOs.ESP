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
#include <SerialModule.h>
#include <ServoModule.h>
#include <I2cModule.h>
#include <AstrOsUtility.h>
#include <AstrOsNetwork.h>
#include <AstrOsConstants.h>
#include <StorageManager.h>

static const char *TAG = AstrOsConstants::ModuleName;

/**********************************
 * network
 **********************************/
static const char *WIFI_AP_SSID = AstrOsConstants::ApSsid;
static const char *WIFI_AP_PASS = AstrOsConstants::Password;

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
#define CONFIG_ESPNOW_PMK "pmk1234567890123"
#define CONFIG_ESPNOW_LMK "lmk1234567890123"
#define ESPNOW_CHANNEL 1
#define ESPNOW_CHANNEL 1
#define ESPNOW_SEND_COUNT 100
#define ESPNOW_SEND_DELAY 1000
#define ESPNOW_SEND_LEN 200
#define ESPNOW_WIFI_MODE WIFI_MODE_STA
#define ESPNOW_WIFI_IF WIFI_IF_STA
#define ESPNOW_MAXDELAY 512

#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"

static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint16_t espnow_seq[ESPNOW_DATA_MAX] = {0, 0};

/**********************************
 * Method definitions
 *********************************/
static void initTimers(void);
static void animationTimerCallback(void *arg);
static void maintenanceTimerCallback(void *arg);

// ESP-NOW
static esp_err_t wifiInit(void);
static esp_err_t espnowInit(void);
static void espnowDeinit(espnow_send_param_t *send_param);
static void espnowSendCallback(const uint8_t *mac_addr, esp_now_send_status_t status);
static void espnowRecvCallback(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);
int espnowDataParse(uint8_t *data, int data_len, uint8_t *state, uint16_t *seq, int *magic);
void espnowDataPrepare(espnow_send_param_t *send_param);

static esp_err_t example_espnow_init();

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

void init(void)
{
    ESP_LOGI(TAG, "init called");

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
    // espnowQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_msg_t));

    ESP_ERROR_CHECK(Storage.Init());

    ESP_ERROR_CHECK(uart_driver_install(ASTRO_PORT, RX_BUF_SIZE * 2, 0, 0, NULL, 0));

    // init TCP/IP stack
    // ESP_ERROR_CHECK(esp_netif_init());
    // init default background loop
    // ESP_ERROR_CHECK(esp_event_loop_create_default());

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

    initTimers();

    /*const char alphanum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    const int string_length = 3;
    std::string random_string(WIFI_AP_SSID);

    for (int i = 0; i < string_length; ++i)
    {
        int num = esp_random() % (sizeof(alphanum) - 1);

        ESP_LOGI(TAG, "%d", num);
        ESP_LOGI(TAG, "%c", alphanum[num]);
        random_string += alphanum[num];
    }

    ESP_LOGI(TAG, "%s", random_string.c_str());

    ESP_ERROR_CHECK(astrOsNetwork.init(random_string.c_str(), WIFI_AP_PASS, serviceQueue, animationQueue, hardwareQueue));
    */

    ESP_ERROR_CHECK(wifiInit());
    // ESP_ERROR_CHECK(espnowInit());
}
/*
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

    example_espnow_init();
    // xTaskCreate(&espnowQueueTask, "espnow_queue_task", 4096, (void *)espnowQueue, 10, NULL);

       svc_config_t config;


        if (Storage.loadServiceConfig(&config))
        {
            ESP_LOGI(TAG, "Network SSID: %s", config.networkSSID);

            std::string temp = std::string(config.networkPass);

            temp.replace(temp.begin() + 1, temp.end() - 1, std::string(temp.length() - 2, '*'));

            ESP_LOGI(TAG, "Network Password: %s", temp.c_str());

            astrOsNetwork.connectToNetwork(config.networkSSID, config.networkPass);
        }
        else
        {
            astrOsNetwork.startWifiAp();
        }

}
*/
/******************************************
 * timers
 *****************************************/

static void initTimers(void)
{
    const esp_timer_create_args_t aTimerArgs = {
        .callback = &animationTimerCallback,
        .name = "animation",
        .skip_unhandled_events = true};

    const esp_timer_create_args_t mTimerArgs = {
        .callback = &maintenanceTimerCallback,
        .name = "maintenance",
        .skip_unhandled_events = true};

    ESP_ERROR_CHECK(esp_timer_create(&aTimerArgs, &animationTimer));
    ESP_ERROR_CHECK(esp_timer_start_once(animationTimer, 5000 * 1000));
    ESP_LOGI("init_timer", "Started animation timer");

    ESP_ERROR_CHECK(esp_timer_create(&mTimerArgs, &maintenanceTimer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(maintenanceTimer, 10 * 1000 * 1000));
    ESP_LOGI("init_timer", "Started maintenance timer");
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

    QueueHandle_t svcQueue;

    svcQueue = (QueueHandle_t)arg;
    uint32_t buttonStartTime = 0;
    bool restartSent = false;
    bool buttonHandled = true;

    uint8_t *data = (uint8_t *)malloc(RX_BUF_SIZE + 1);
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
                    ESP_LOGI(TAG, "Restarting ESP32");
                    esp_restart();
                }
                else if ((xTaskGetTickCount() - buttonStartTime) > pdMS_TO_TICKS(MEDIUM_PRESS_THRESHOLD_MS))
                {
                    queue_svc_cmd_t msg = {SERVICE_COMMAND::SWITCH_TO_DISCOVERY, NULL};
                    xQueueSend(svcQueue, &msg, pdMS_TO_TICKS(2000));
                    ESP_LOGI(TAG, "Discovery Switch Sent");
                }
            }

            buttonStartTime = 0;
            restartSent = false;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    free(data);
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
            /*
            case SERVICE_COMMAND::START_WIFI_AP:
                astrOsNetwork.startWifiAp();
                break;
            case SERVICE_COMMAND::STOP_WIFI_AP:
                astrOsNetwork.stopWifiAp();
                break;
            case SERVICE_COMMAND::CONNECT_TO_NETWORK:
                astrOsNetwork.connectToNetwork("", "");
                break;
            case SERVICE_COMMAND::DISCONNECT_FROM_NETWORK:
                astrOsNetwork.disconnectFromNetwork();
                break;
            case SERVICE_COMMAND::SWITCH_TO_NETWORK:
            {
                svc_config_t config;

                if (!Storage.loadServiceConfig(&config))
                {
                    ESP_LOGI(TAG, "Switch to network requested, but configuration couldn't load");
                    break;
                }

                bool wifiConnected = false;
                bool wifiStopped = astrOsNetwork.stopWifiAp();
                if (wifiStopped)
                {
                    ESP_LOGI(TAG, "Network SSID: %s", config.networkSSID);

                    std::string temp = std::string(config.networkPass);

                    temp.replace(temp.begin() + 1, temp.end() - 1, std::string(temp.length() - 2, '*'));

                    ESP_LOGI(TAG, "Network Password: %s", temp.c_str());

                    wifiConnected = astrOsNetwork.connectToNetwork(config.networkSSID, config.networkPass);
                }
                if (wifiConnected)
                {
                    ESP_LOGI(TAG, "Succesfully switched to network!");
                }
                else
                {
                    astrOsNetwork.disconnectFromNetwork();
                    astrOsNetwork.startWifiAp();
                }
                break;
            }
            case SERVICE_COMMAND::SWITCH_TO_WIFI_AP:
            {
                ESP_LOGI(TAG, "Switching to WIFI AP");
                bool apStarted = false;
                bool wifiStopped = astrOsNetwork.disconnectFromNetwork();
                if (wifiStopped)
                {
                    apStarted = astrOsNetwork.startWifiAp();
                }
                if (apStarted)
                {
                    ESP_LOGI(TAG, "Succesfully switched to WIFI AP!");
                }
                else
                {
                    ESP_LOGI(TAG, "Restarting ESP32");
                    esp_restart();
                }
                break;
            }
            */
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
    espnow_event_t evt;
    uint8_t recv_state = 0;
    uint16_t recv_seq = 0;
    int recv_magic = 0;
    bool is_broadcast = false;
    int ret;

    vTaskDelay(5000 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "Start sending broadcast data");

    espnow_send_param_t *send_param;

    /* Add broadcast peer information to peer list. */
    esp_now_peer_info_t *peer = new esp_now_peer_info_t();
    if (peer == NULL)
    {
        ESP_LOGE(TAG, "Malloc peer information fail");
        esp_now_deinit();
        vTaskDelete(NULL);
    }

    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = ESPNOW_CHANNEL;
    peer->ifidx = ESPNOW_WIFI_IF;
    peer->encrypt = false;
    memcpy(peer->peer_addr, broadcast_mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK(esp_now_add_peer(peer));
    delete (peer);

    /* Initialize sending parameters. */
    send_param = new espnow_send_param_t();
    if (send_param == NULL)
    {
        ESP_LOGE(TAG, "Malloc send parameter fail");
        esp_now_deinit();
        vTaskDelete(NULL);
    }

    memset(send_param, 0, sizeof(espnow_send_param_t));
    send_param->unicast = false;
    send_param->broadcast = true;
    send_param->state = 0;
    send_param->magic = esp_random();
    send_param->count = ESPNOW_SEND_COUNT;
    send_param->delay = ESPNOW_SEND_DELAY;
    send_param->len = ESPNOW_SEND_LEN;
    send_param->buffer = new uint8_t(ESPNOW_SEND_LEN);
    if (send_param->buffer == NULL)
    {
        ESP_LOGE(TAG, "Malloc send buffer fail");
        delete (send_param);
        esp_now_deinit();
        vTaskDelete(NULL);
    }

    memcpy(send_param->dest_mac, broadcast_mac, ESP_NOW_ETH_ALEN);

    /* Start sending broadcast ESPNOW data. */

    if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK)
    {
        ESP_LOGE(TAG, "Send error");
        espnowDeinit(send_param);
        vTaskDelete(NULL);
    }

    while (1)
    {
        if (xQueueReceive(espnowQueue, &(evt), 0))
        {
            ESP_LOGI(TAG, "ESP-NOW Queue Stack HWM: %d", uxTaskGetStackHighWaterMark(NULL));
            // ESP_LOGI(TAG, "ESP-NOW Command received on queue => %s", evt.info.recv_cb.data);

            switch (evt.id)
            {
            case ESPNOW_SEND_CB:
            {
                espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
                is_broadcast = IS_BROADCAST_ADDR(send_cb->mac_addr);

                ESP_LOGI(TAG, "Send data to " MACSTR ", status1: %d", MAC2STR(send_cb->mac_addr), send_cb->status);

                if (is_broadcast && (send_param->broadcast == false))
                {
                    ESP_LOGI(TAG, "break");
                    break;
                }

                if (!is_broadcast)
                {
                    send_param->count--;
                    if (send_param->count == 0)
                    {
                        ESP_LOGI(TAG, "Send done");
                        // espnowDeinit(send_param);
                        // vTaskDelete(NULL);
                    }
                }

                ESP_LOGI(TAG, "1");

                /* Delay a while before sending the next data. */
                if (send_param->delay > 0)
                {
                    ESP_LOGI(TAG, "2");

                    vTaskDelay(ESPNOW_SEND_DELAY / portTICK_PERIOD_MS);
                }
                ESP_LOGI(TAG, "3");

                ESP_LOGI(TAG, "send data to " MACSTR "", MAC2STR(send_cb->mac_addr));

                memcpy(send_param->dest_mac, send_cb->mac_addr, ESP_NOW_ETH_ALEN);
                espnowDataPrepare(send_param);

                /* Send the next data after the previous data is sent. */
                if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK)
                {
                    ESP_LOGE(TAG, "Send error");
                    // espnowDeinit(send_param);
                    // vTaskDelete(NULL);
                }
                break;
            }
            case ESPNOW_RECV_CB:
            {
                espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;

                ret = espnowDataParse(recv_cb->data, recv_cb->data_len, &recv_state, &recv_seq, &recv_magic);
                free(recv_cb->data);
                if (ret == ESPNOW_DATA_BROADCAST)
                {
                    ESP_LOGI(TAG, "Receive %dth broadcast data from: " MACSTR ", len: %d", recv_seq, MAC2STR(recv_cb->mac_addr), recv_cb->data_len);

                    /* If MAC address does not exist in peer list, add it to peer list. */
                    if (esp_now_is_peer_exist(recv_cb->mac_addr) == false)
                    {
                        esp_now_peer_info_t *peer = new esp_now_peer_info_t();
                        if (peer == NULL)
                        {
                            ESP_LOGE(TAG, "Malloc peer information fail");
                            // espnowDeinit(send_param);
                            // vTaskDelete(NULL);
                        }
                        memset(peer, 0, sizeof(esp_now_peer_info_t));
                        peer->channel = ESPNOW_CHANNEL;
                        peer->ifidx = ESPNOW_WIFI_IF;
                        peer->encrypt = true;
                        memcpy(peer->lmk, CONFIG_ESPNOW_LMK, ESP_NOW_KEY_LEN);
                        memcpy(peer->peer_addr, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
                        ESP_ERROR_CHECK(esp_now_add_peer(peer));
                        delete (peer);
                    }

                    /* Indicates that the device has received broadcast ESPNOW data. */
                    if (send_param->state == 0)
                    {
                        send_param->state = 1;
                    }

                    /* If receive broadcast ESPNOW data which indicates that the other device has received
                     * broadcast ESPNOW data and the local magic number is bigger than that in the received
                     * broadcast ESPNOW data, stop sending broadcast ESPNOW data and start sending unicast
                     * ESPNOW data.
                     */
                    if (recv_state == 1)
                    {
                        /* The device which has the bigger magic number sends ESPNOW data, the other one
                         * receives ESPNOW data.
                         */
                        if (send_param->unicast == false && send_param->magic >= recv_magic)
                        {
                            ESP_LOGI(TAG, "Start sending unicast data");
                            ESP_LOGI(TAG, "send data to " MACSTR "", MAC2STR(recv_cb->mac_addr));

                            /* Start sending unicast ESPNOW data. */
                            memcpy(send_param->dest_mac, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
                            espnowDataPrepare(send_param);
                            if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK)
                            {
                                ESP_LOGE(TAG, "Send error");
                                // espnowDeinit(send_param);
                                // vTaskDelete(NULL);
                            }
                            else
                            {
                                send_param->broadcast = false;
                                send_param->unicast = true;
                            }
                        }
                    }
                }
                else if (ret == ESPNOW_DATA_UNICAST)
                {
                    ESP_LOGI(TAG, "Receive %dth unicast data from: " MACSTR ", len: %d", recv_seq, MAC2STR(recv_cb->mac_addr), recv_cb->data_len);

                    /* If receive unicast ESPNOW data, also stop sending broadcast ESPNOW data. */
                    send_param->broadcast = false;
                }
                else
                {
                    ESP_LOGI(TAG, "Receive error data from: " MACSTR "", MAC2STR(recv_cb->mac_addr));
                }
                break;
            }
            default:
                ESP_LOGE(TAG, "Callback type error: %d", evt.id);
                break;
            }
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

int espnowDataPrepare()
{
    return 0;
}

static void espnowSendCallback(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    espnow_event_t evt;
    espnow_event_send_cb_t *send_cb = &evt.info.send_cb;

    if (mac_addr == NULL)
    {
        ESP_LOGE(TAG, "Send cb arg error");
        return;
    }

    evt.id = ESPNOW_SEND_CB;
    memcpy(send_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    send_cb->status = status;
    if (xQueueSend(espnowQueue, &evt, ESPNOW_MAXDELAY) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send send queue fail");
    }
}

static void espnowRecvCallback(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    espnow_event_t evt;
    espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
    uint8_t *mac_addr = recv_info->src_addr;
    uint8_t *des_addr = recv_info->des_addr;

    if (mac_addr == NULL || data == NULL || len <= 0)
    {
        ESP_LOGE(TAG, "Receive cb arg error");
        return;
    }

    if (IS_BROADCAST_ADDR(des_addr))
    {
        /* If added a peer with encryption before, the receive packets may be
         * encrypted as peer-to-peer message or unencrypted over the broadcast channel.
         * Users can check the destination address to distinguish it.
         */
        ESP_LOGD(TAG, "Receive broadcast ESPNOW data");
    }
    else
    {
        ESP_LOGD(TAG, "Receive unicast ESPNOW data");
    }

    evt.id = ESPNOW_RECV_CB;
    memcpy(recv_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    recv_cb->data = new uint8_t(len);
    if (recv_cb->data == NULL)
    {
        ESP_LOGE(TAG, "Malloc receive data fail");
        return;
    }
    memcpy(recv_cb->data, data, len);
    recv_cb->data_len = len;
    if (xQueueSend(espnowQueue, &evt, ESPNOW_MAXDELAY) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send receive queue fail");
        delete (recv_cb->data);
    }
}

/* Parse received ESPNOW data. */
int espnowDataParse(uint8_t *data, uint16_t data_len, uint8_t *state, uint16_t *seq, int *magic)
{
    espnow_data_t *buf = (espnow_data_t *)data;
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

    return -1;
}

/* Prepare ESPNOW data to be sent. */
void espnowDataPrepare(espnow_send_param_t *send_param)
{
    espnow_data_t *buf = (espnow_data_t *)send_param->buffer;

    assert(send_param->len >= sizeof(espnow_data_t));

    buf->type = IS_BROADCAST_ADDR(send_param->dest_mac) ? ESPNOW_DATA_BROADCAST : ESPNOW_DATA_UNICAST;
    buf->state = send_param->state;
    buf->seq_num = espnow_seq[buf->type]++;
    buf->crc = 0;
    buf->magic = send_param->magic;
    /* Fill all remaining bytes after the data with random values */
    esp_fill_random(buf->payload, send_param->len - sizeof(espnow_data_t));
    buf->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, send_param->len);
}

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

static void espnowDeinit(espnow_send_param_t *send_param)
{
    ESP_LOGI(TAG, "espnowDeinit called");

    delete (send_param->buffer);
    delete (send_param);
    vSemaphoreDelete(espnowQueue);
    esp_now_deinit();
}

static void example_espnow_task(void *pvParameter)
{

    espnow_event_t evt;
    uint8_t recv_state = 0;
    uint16_t recv_seq = 0;
    int recv_magic = 0;
    bool is_broadcast = false;
    int ret;

    vTaskDelay(5000 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "Start sending broadcast data");

    /* Start sending broadcast ESPNOW data. */
    espnow_send_param_t *send_param = (espnow_send_param_t *)pvParameter;
    if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK)
    {
        ESP_LOGE(TAG, "Send error");
        espnowDeinit(send_param);
        vTaskDelete(NULL);
    }

    while (xQueueReceive(espnowQueue, &evt, portMAX_DELAY) == pdTRUE)
    {
        switch (evt.id)
        {
        case ESPNOW_SEND_CB:
        {
            espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
            is_broadcast = IS_BROADCAST_ADDR(send_cb->mac_addr);

            ESP_LOGD(TAG, "Send data to " MACSTR ", status1: %d", MAC2STR(send_cb->mac_addr), send_cb->status);

            if (is_broadcast && (send_param->broadcast == false))
            {
                break;
            }

            if (!is_broadcast)
            {
                send_param->count--;
                if (send_param->count == 0)
                {
                    ESP_LOGI(TAG, "Send done");
                    espnowDeinit(send_param);
                    vTaskDelete(NULL);
                }
            }

            /* Delay a while before sending the next data. */
            if (send_param->delay > 0)
            {
                vTaskDelay(send_param->delay / portTICK_PERIOD_MS);
            }

            ESP_LOGI(TAG, "send data to " MACSTR "", MAC2STR(send_cb->mac_addr));

            memcpy(send_param->dest_mac, send_cb->mac_addr, ESP_NOW_ETH_ALEN);
            espnowDataPrepare(send_param);

            /* Send the next data after the previous data is sent. */
            if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK)
            {
                ESP_LOGE(TAG, "Send error");
                espnowDeinit(send_param);
                vTaskDelete(NULL);
            }
            break;
        }
        case ESPNOW_RECV_CB:
        {
            espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;

            ret = espnowDataParse(recv_cb->data, recv_cb->data_len, &recv_state, &recv_seq, &recv_magic);
            free(recv_cb->data);
            if (ret == ESPNOW_DATA_BROADCAST)
            {
                ESP_LOGI(TAG, "Receive %dth broadcast data from: " MACSTR ", len: %d", recv_seq, MAC2STR(recv_cb->mac_addr), recv_cb->data_len);

                /* If MAC address does not exist in peer list, add it to peer list. */
                if (esp_now_is_peer_exist(recv_cb->mac_addr) == false)
                {
                    esp_now_peer_info_t *peer = new esp_now_peer_info_t();
                    if (peer == NULL)
                    {
                        ESP_LOGE(TAG, "Malloc peer information fail");
                        espnowDeinit(send_param);
                        vTaskDelete(NULL);
                    }
                    memset(peer, 0, sizeof(esp_now_peer_info_t));
                    peer->channel = ESPNOW_CHANNEL;
                    peer->ifidx = ESPNOW_WIFI_IF;
                    peer->encrypt = true;
                    memcpy(peer->lmk, CONFIG_ESPNOW_LMK, ESP_NOW_KEY_LEN);
                    memcpy(peer->peer_addr, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
                    ESP_ERROR_CHECK(esp_now_add_peer(peer));
                    delete (peer);
                }

                /* Indicates that the device has received broadcast ESPNOW data. */
                if (send_param->state == 0)
                {
                    send_param->state = 1;
                }

                /* If receive broadcast ESPNOW data which indicates that the other device has received
                 * broadcast ESPNOW data and the local magic number is bigger than that in the received
                 * broadcast ESPNOW data, stop sending broadcast ESPNOW data and start sending unicast
                 * ESPNOW data.
                 */
                if (recv_state == 1)
                {
                    /* The device which has the bigger magic number sends ESPNOW data, the other one
                     * receives ESPNOW data.
                     */
                    if (send_param->unicast == false && send_param->magic >= recv_magic)
                    {
                        ESP_LOGI(TAG, "Start sending unicast data");
                        ESP_LOGI(TAG, "send data to " MACSTR "", MAC2STR(recv_cb->mac_addr));

                        /* Start sending unicast ESPNOW data. */
                        memcpy(send_param->dest_mac, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
                        espnowDataPrepare(send_param);
                        if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK)
                        {
                            ESP_LOGE(TAG, "Send error");
                            espnowDeinit(send_param);
                            vTaskDelete(NULL);
                        }
                        else
                        {
                            send_param->broadcast = false;
                            send_param->unicast = true;
                        }
                    }
                }
            }
            else if (ret == ESPNOW_DATA_UNICAST)
            {
                ESP_LOGI(TAG, "Receive %dth unicast data from: " MACSTR ", len: %d", recv_seq, MAC2STR(recv_cb->mac_addr), recv_cb->data_len);

                /* If receive unicast ESPNOW data, also stop sending broadcast ESPNOW data. */
                send_param->broadcast = false;
            }
            else
            {
                ESP_LOGI(TAG, "Receive error data from: " MACSTR "", MAC2STR(recv_cb->mac_addr));
            }
            break;
        }
        default:
            ESP_LOGE(TAG, "Callback type error: %d", evt.id);
            break;
        }
    }
}

static esp_err_t example_espnow_init(void)
{
    espnow_send_param_t *send_param;

    espnowQueue = xQueueCreate(6, sizeof(espnow_event_t));
    if (espnowQueue == NULL)
    {
        ESP_LOGE(TAG, "Create mutex fail");
        return ESP_FAIL;
    }

    /* Initialize ESPNOW and register sending and receiving callback function. */
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnowSendCallback));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnowRecvCallback));
#if CONFIG_ESPNOW_ENABLE_POWER_SAVE
    ESP_ERROR_CHECK(esp_now_set_wake_window(CONFIG_ESPNOW_WAKE_WINDOW));
    ESP_ERROR_CHECK(esp_wifi_connectionless_module_set_wake_interval(CONFIG_ESPNOW_WAKE_INTERVAL));
#endif
    /* Set primary master key. */
    ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK));

    /* Add broadcast peer information to peer list. */
    esp_now_peer_info_t *peer = new esp_now_peer_info_t();
    if (peer == NULL)
    {
        ESP_LOGE(TAG, "Malloc peer information fail");
        vSemaphoreDelete(espnowQueue);
        esp_now_deinit();
        return ESP_FAIL;
    }
    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = ESPNOW_CHANNEL;
    peer->ifidx = ESPNOW_WIFI_IF;
    peer->encrypt = false;
    memcpy(peer->peer_addr, broadcast_mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK(esp_now_add_peer(peer));
    delete (peer);

    /* Initialize sending parameters. */
    send_param = new espnow_send_param_t();
    if (send_param == NULL)
    {
        ESP_LOGE(TAG, "Malloc send parameter fail");
        vSemaphoreDelete(espnowQueue);
        esp_now_deinit();
        return ESP_FAIL;
    }
    memset(send_param, 0, sizeof(espnow_send_param_t));
    send_param->unicast = false;
    send_param->broadcast = true;
    send_param->state = 0;
    send_param->magic = esp_random();
    send_param->count = ESPNOW_SEND_COUNT;
    send_param->delay = ESPNOW_SEND_DELAY;
    send_param->len = ESPNOW_SEND_LEN;
    send_param->buffer = new uint8_t(ESPNOW_SEND_LEN);
    if (send_param->buffer == NULL)
    {
        ESP_LOGE(TAG, "Malloc send buffer fail");
        delete (send_param);
        vSemaphoreDelete(espnowQueue);
        esp_now_deinit();
        return ESP_FAIL;
    }
    memcpy(send_param->dest_mac, broadcast_mac, ESP_NOW_ETH_ALEN);
    espnowDataPrepare(send_param);

    xTaskCreate(example_espnow_task, "example_espnow_task", 2048, send_param, 4, NULL);

    return ESP_OK;
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifiInit();
    example_espnow_init();
}