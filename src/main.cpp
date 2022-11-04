#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>
#include <driver/rmt.h>
#include <esp_log.h>
#include <driver/uart.h>
#include <nvs_flash.h>
#include <esp_event.h>


#include <AstrOsInterface.h>
#include <KangarooInterface.h>
#include <AnimationController.h>
#include <AnimationCommand.h>
#include <SerialModule.h>
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
static QueueHandle_t serialQueue;
static QueueHandle_t serviceQueue;

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
 * Kangaroo Interface
 **********************************/

#define KI_TX_PIN (GPIO_NUM_12)
#define KI_RX_PIN (GPIO_NUM_13)
#define KI_BAUD_RATE (9600)

/**********************************
 * Method definitions
 *********************************/
static void initTimers(void);
static void animationTimerCallback(void *arg);
static void maintenanceTimerCallback(void *arg);

void astrosRxTask(void *arg);
void serviceQueueTask(void *arg);
void animationQueueTask(void *arg);
void serialQueueTask(void *arg);

esp_err_t mountSdCard(void);


extern "C"
{
    void app_main(void);
}

void init(void)
{
    ESP_LOGI(TAG, "init called");

    animationQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_ani_cmd_t));
    serialQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_msg_t));
    serviceQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_svc_cmd_t));

    ESP_ERROR_CHECK(Storage.Init());

    ESP_ERROR_CHECK(uart_driver_install(ASTRO_PORT, RX_BUF_SIZE * 2, 0, 0, NULL, 0));

    //init TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    // init defult background loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    AstrOs.Init(animationQueue);
    ESP_LOGI(TAG, "AstrOs Interface initiated");
     
    ESP_ERROR_CHECK(SerialMod.Init(KI_BAUD_RATE, KI_RX_PIN, KI_TX_PIN));
    ESP_LOGI(TAG, "Serial Module initiated");

    initTimers();

    ESP_ERROR_CHECK(astrOsNetwork.init(WIFI_AP_SSID, WIFI_AP_PASS, serviceQueue, animationQueue));

    svc_config_t config;
    
    if (Storage.loadServiceConfig(&config)){
        astrOsNetwork.connectToNetwork(config.networkSSID, config.networkPass);
    } else{
        astrOsNetwork.startWifiAp();
    }
}

void app_main()
{
    init();

    xTaskCreate(&serviceQueueTask, "service_queue_task", 2048, (void *)serviceQueue, 10, NULL);
    xTaskCreate(&animationQueueTask, "animation_queue_task", 4096, (void *)animationQueue, 10, NULL);
    xTaskCreate(&serialQueueTask, "serial_queue_task", 2048, (void *)serialQueue, 10, NULL);

    xTaskCreate(&astrosRxTask, "astros_rx_task", 2048, (void *)animationQueue, 10, NULL);
}


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

static void maintenanceTimerCallback(void *arg){    
    ESP_LOGI(TAG, "RAM left %d", esp_get_free_heap_size());
}

static void animationTimerCallback(void *arg)
{
    esp_timer_stop(animationTimer);

    if (AnimationCtrl.scriptIsLoaded())
    {
        CommandTemplate* cmd = AnimationCtrl.getNextCommandPtr();

        CommandType ct = cmd->type;
        std::string val = cmd->val;

        switch (ct)
        {
            case CommandType::Kangaroo:
            case CommandType::GenericSerial:
            {
                SerialCommand scd = SerialCommand(val);
                ESP_LOGI(TAG, "val: %s", scd.GetValue().c_str());
                queue_msg_t msg = {0, 0};
                strncpy(msg.data, scd.GetValue().c_str(), sizeof(msg.data));
                msg.data[sizeof(msg.data) - 1] = '\0';
                xQueueSend(serialQueue, &msg, pdMS_TO_TICKS(2000));
                break;
            }
            default:
                break;
        }

        delete(cmd);
        
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
void astrosRxTask(void *arg)
{

    uint8_t *data = (uint8_t *)malloc(RX_BUF_SIZE + 1);
    while (1)
    {

        const int rxBytes = uart_read_bytes(ASTRO_PORT, data, RX_BUF_SIZE, pdMS_TO_TICKS(1000));

        if (rxBytes > 0)
        {
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
            switch (msg.cmd)
            {
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

                if (!Storage.loadServiceConfig(&config)){
                    ESP_LOGI(TAG, "Switch to network requested, but configuration couldn't load");
                    break;
                }
                
                bool wifiConnected = false;
                bool wifiStopped = astrOsNetwork.stopWifiAp();
                if (wifiStopped)
                {
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

void serialQueueTask(void *arg)
{

    QueueHandle_t serialQueue;

    serialQueue = (QueueHandle_t)arg;
    queue_msg_t msg;

    while (1)
    {
        if (xQueueReceive(serialQueue, &(msg), 0))
        {
            ESP_LOGI(TAG, "Serial command received on queue => %s", msg.data);
            SerialMod.SendCommand(msg.data);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


