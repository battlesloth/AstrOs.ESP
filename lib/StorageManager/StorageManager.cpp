#include <StorageManager.h>
#include <AstrOsUtility.h>
#include <NvsManager.h>

#include <stdio.h>
#include <stdbool.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <string.h>

#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

/**********************************
 * SD Card
 **********************************/

#define MOUNT_POINT "/sdcard"
#define PIN_NUM_MISO 19 // 2
#define PIN_NUM_MOSI 23 // 15
#define PIN_NUM_CLK 18  // 14
#define PIN_NUM_CS 4    // 13
#define SPI_DMA_CHAN 1

static const char *TAG = "StorageManager";

static sdmmc_card_t *card;

StorageManager Storage;

StorageManager::StorageManager() {}
StorageManager::~StorageManager()
{

    const char mountPoint[] = MOUNT_POINT;

    // All done, unmount partition and disable SPI peripheral
    esp_vfs_fat_sdcard_unmount(mountPoint, card);
    ESP_LOGI(TAG, "Card unmounted");

    // deinitialize the bus after all devices are removed
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_host_device_t device = static_cast<spi_host_device_t>(host.slot);
    spi_bus_free(device);
}

esp_err_t StorageManager::Init()
{

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_LOGI(TAG, "Erasing flash");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    else
    {
        logError(TAG, __FUNCTION__, __LINE__, err);
        return err;
    }

    return StorageManager::mountSdCard();
}

bool StorageManager::saveServiceConfig(svc_config_t config)
{
    return nvsSaveServiceConfig(config);
}

bool StorageManager::loadServiceConfig(svc_config_t *config)
{
    return nvsLoadServiceConfig(config);
}

bool StorageManager::clearServiceConfig()
{
    return nvsClearServiceConfig();
}

bool StorageManager::formatSdCard()
{
    return true;
}

bool StorageManager::readSdCard()
{

    char entryPath[] = "/sdcard/";
    char entrysize[16];
    const char *entrytype;

    struct dirent *entry;
    struct stat entry_stat;

    DIR *dir = opendir(entryPath);
    const size_t dirpath_len = strlen(entryPath);

    if (!dir)
    {
        ESP_LOGE(TAG, "Failed to open dir");
        return false;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        entrytype = (entry->d_type == DT_DIR ? "directory" : "file");

        strlcpy(entryPath + dirpath_len, entry->d_name, sizeof(entryPath) - dirpath_len);
        if (stat(entryPath, &entry_stat) == -1)
        {
            ESP_LOGE(TAG, "Failed to stat %s : %s", entrytype, entry->d_name);
            continue;
        }
        sprintf(entrysize, "%ld", entry_stat.st_size);
        ESP_LOGI(TAG, "Found %s : %s (%s bytes)", entrytype, entry->d_name, entrysize);
    }

    closedir(dir);

    return true;
}

esp_err_t StorageManager::mountSdCard()
{
    esp_err_t err;

    esp_vfs_fat_mount_config_t mountConfig = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024};

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
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        ESP_LOGE(TAG, "Failed to initialize bus.");
        return err;
    }

    sdspi_device_config_t slotConfig = SDSPI_DEVICE_CONFIG_DEFAULT();
    slotConfig.gpio_cs = static_cast<gpio_num_t>(PIN_NUM_CS);
    slotConfig.host_id = device;

    ESP_LOGI(TAG, "Mounting file system");

    err = esp_vfs_fat_sdspi_mount(mountPoint, &host, &slotConfig, &mountConfig, &card);

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        ESP_LOGE(TAG, "Failed to mount file system.");
        return err;
    }

    ESP_LOGI(TAG, "FIle system mounted");

    sdmmc_card_print_info(stdout, card);

    return err;
}
