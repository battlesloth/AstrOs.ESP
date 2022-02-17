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
#include <AstrOsUtility.h>
#include <AstrOsNetwork.h>
#include <AstrOsConstants.h>

#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = AstrOsConstants::ModuleName;


/**********************************
 * SD Card
 **********************************/

#define MOUNT_POINT "/sdcard"
#define PIN_NUM_MISO 19 //2
#define PIN_NUM_MOSI 23 //15
#define PIN_NUM_CLK  18 //14
#define PIN_NUM_CS   4 //13
#define SPI_DMA_CHAN 1

 sdmmc_card_t *card;

/**********************************
 * network
 **********************************/
static const char *WIFI_AP_SSID = AstrOsConstants::ApSsid;
static const char *WIFI_AP_PASS = AstrOsConstants::Password;

/**********************************
 * queues
 **********************************/

static QueueHandle_t animationQueue;
static QueueHandle_t kangarooQueue;
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

static esp_timer_handle_t animationTimer;

/**********************************
 * animation
 **********************************/
#define QUEUE_LENGTH 5

/**********************************
 * Kangaroo Interface
 **********************************/

#define KI_TX_PIN (GPIO_NUM_23)
#define KI_RX_PIN (GPIO_NUM_22)
#define KI_BAUD_RATE (9600)

/**********************************
 * Method definitions
 *********************************/
static void initTimers(void);
static void animationTimerCallback(void *arg);

void astrosRxTask(void *arg);
void serviceQueueTask(void *arg);
void animationQueueTask(void *arg);
void kangarooQueueTask(void *arg);

esp_err_t mountSdCard(void);


extern "C"
{
    void app_main(void);
}

void init(void)
{
    ESP_LOGI(TAG, "init called");

    animationQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_msg_t));
    kangarooQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_msg_t));
    serviceQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_cmd_t));

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_LOGI(TAG, "Erasing flash");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    } 
    ESP_ERROR_CHECK( err );

    err = mountSdCard();

    ESP_ERROR_CHECK(uart_driver_install(ASTRO_PORT, RX_BUF_SIZE * 2, 0, 0, NULL, 0));

    //init TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    // init defult background loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    AstrOs.Init(animationQueue);
    ESP_LOGI(TAG, "AstrOs Interface initiated");
    ESP_ERROR_CHECK(init_kangaroo_interface(KI_BAUD_RATE, KI_RX_PIN, KI_TX_PIN));
    ESP_LOGI(TAG, "Kangaroo Interface initiated");

    initTimers();

    ESP_ERROR_CHECK(astrOsNetwork.init(WIFI_AP_SSID, WIFI_AP_PASS, serviceQueue));

    svc_config_t config;
    
    if (loadServiceConfig(&config)){
        astrOsNetwork.connectToNetwork(config.networkSSID, config.networkPass);
    } else{
        astrOsNetwork.startWifiAp();
    }
}

void app_main()
{
    init();

    xTaskCreate(&serviceQueueTask, "service_queue_task", 2048, (void *)serviceQueue, 10, NULL);
    xTaskCreate(&animationQueueTask, "animation_queue_task", 2048, (void *)animationQueue, 10, NULL);
    xTaskCreate(&kangarooQueueTask, "kangaroo_queue_task", 2048, (void *)kangarooQueue, 10, NULL);

    xTaskCreate(&astrosRxTask, "astros_rx_task", 2048, (void *)animationQueue, 10, NULL);

    xTaskCreate(&kangaroo_rx_task, "kangaroo_rx_task", 2048, (void *)kangarooQueue, 5, NULL);
    xTaskCreate(&kangaroo_tx_task, "kangaroo_tx_task", 2048, (void *)kangarooQueue, 9, NULL);
}


/******************************************
 * timers
 *****************************************/

static void initTimers(void)
{
    const esp_timer_create_args_t timerArgs = {
        .callback = &animationTimerCallback,
        .name = "animation",
        .skip_unhandled_events = true};

    ESP_ERROR_CHECK(esp_timer_create(&timerArgs, &animationTimer));
    ESP_ERROR_CHECK(esp_timer_start_once(animationTimer, 5000 * 1000));
    ESP_LOGI("init_timer", "Started animation timer");
}

static void animationTimerCallback(void *arg)
{
    esp_timer_stop(animationTimer);

    if (AnimationCtrl.servoScriptIsLoaded())
    {
        char *cmd = AnimationCtrl.getNextServoCommand();

        ESP_LOGI("animation_callback", "cmd: %s", cmd);

        ESP_ERROR_CHECK(esp_timer_start_periodic(animationTimer, AnimationCtrl.msTillNextServoCommand() * 1000));
    }
    else
    {
        ESP_ERROR_CHECK(esp_timer_start_periodic(animationTimer, 250 * 1000));
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
    queue_cmd_t msg;
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

                if (!loadServiceConfig(&config)){
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
    queue_msg_t msg;

    while (1)
    {
        if (xQueueReceive(animationQueue, &(msg), 0))
        {
            AnimationCtrl.queueScript(msg.data);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void kangarooQueueTask(void *arg)
{

    QueueHandle_t kangarooQueue;

    kangarooQueue = (QueueHandle_t)arg;
    queue_msg_t msg;

    while (1)
    {
        if (xQueueReceive(kangarooQueue, &(msg), 0))
        {
            ESP_LOGI("kagaroo queue", "message: %s", msg.data);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t mountSdCard(){
    esp_err_t err;

    esp_vfs_fat_mount_config_t mountConfig = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    const char mountPoint[] = MOUNT_POINT;
    ESP_LOGI(TAG, "Initalizing SD card");

    ESP_LOGI(TAG, "Using SPI peripheral");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    spi_host_device_t device = static_cast<spi_host_device_t>(host.slot);

    err = spi_bus_initialize(device, &bus_cfg, SPI_DMA_CHAN);
    if (logError(TAG, __FUNCTION__, __LINE__, err)) {
        ESP_LOGE(TAG, "Failed to initialize bus.");
        return err;
    }

    sdspi_device_config_t slotConfig = SDSPI_DEVICE_CONFIG_DEFAULT();
    slotConfig.gpio_cs = static_cast<gpio_num_t>(PIN_NUM_CS);
    slotConfig.host_id = device;
    
    ESP_LOGI(TAG, "Mounting file system");

    err = esp_vfs_fat_sdspi_mount(mountPoint, &host, &slotConfig, &mountConfig, &card);

    if (logError(TAG, __FUNCTION__, __LINE__, err)){
        ESP_LOGE(TAG, "Failed to mount file system.");
        return err;
    }

    ESP_LOGI(TAG, "FIle system mounted");
    
    sdmmc_card_print_info(stdout, card);

    char entryPath[] = "/sdcard/";
    char entrysize[16];
    const char *entrytype;

    struct dirent *entry;
    struct stat entry_stat;

    mkdir("/sdcard/test", 0777);

    DIR *dir = opendir("/sdcard/");
    const size_t dirpath_len = strlen("/sdcard/");

    if (!dir) {
        ESP_LOGE(TAG, "Failed to open dir");

        return ESP_FAIL;
    }

     while ((entry = readdir(dir)) != NULL) {
        entrytype = (entry->d_type == DT_DIR ? "directory" : "file");

        strlcpy(entryPath + dirpath_len, entry->d_name, sizeof(entryPath) - dirpath_len);
        if (stat(entryPath, &entry_stat) == -1) {
            ESP_LOGE(TAG, "Failed to stat %s : %s", entrytype, entry->d_name);
            continue;
        }
        sprintf(entrysize, "%ld", entry_stat.st_size);
        ESP_LOGI(TAG, "Found %s : %s (%s bytes)", entrytype, entry->d_name, entrysize);
     }


    closedir(dir);

    return err;
}
