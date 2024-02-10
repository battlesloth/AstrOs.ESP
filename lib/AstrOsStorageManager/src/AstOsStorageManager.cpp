#include <AstrOsStorageManager.hpp>
#include <AstrOsUtility.h>
#include <AstrOsEspNowUtility.h>
#include <AstrOsUtility_Esp.h>
#include <NvsManager.h>

#include <stdio.h>
#include <stdbool.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <string>
#include <vector>

#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "vfs_fat_internal.h"
#include "sdmmc_cmd.h"
#include "esp_spiffs.h"

/**********************************
 * SD Card
 **********************************/

#ifdef USE_SPIFFS
#define MOUNT_POINT "/spiffs"
#else
#define MOUNT_POINT "/sdcard"
#endif

#define PIN_NUM_MISO 19 // 2
#define PIN_NUM_MOSI 23 // 15
#define PIN_NUM_CLK 18  // 14
#define PIN_NUM_CS 4    // 13
#define SPI_DMA_CHAN 1

static const char *TAG = "StorageManager";

static sdmmc_card_t *card;

AstrOsStorageManager AstrOs_Storage;

AstrOsStorageManager::AstrOsStorageManager() {}
AstrOsStorageManager::~AstrOsStorageManager()
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

esp_err_t AstrOsStorageManager::Init()
{

    ESP_LOGI(TAG, "Initalizing Flash");

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
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            return err;
        }
    }

    ESP_LOGI(TAG, "Mounting SD Card");

#ifndef DARTHSERVO
    return AstrOsStorageManager::mountSdCard();
#else
    return err;
#endif
}

bool AstrOsStorageManager::saveServiceConfig(svc_config_t config)
{
    return nvsSaveServiceConfig(config);
}

bool AstrOsStorageManager::loadServiceConfig(svc_config_t *config)
{
    return nvsLoadServiceConfig(config);
}

bool AstrOsStorageManager::clearServiceConfig()
{
    return nvsClearServiceConfig();
}

bool AstrOsStorageManager::setControllerFingerprint(const char *fingerprint)
{
    return nvsSetControllerFingerprint(fingerprint);
}

bool AstrOsStorageManager::getControllerFingerprint(char *fingerprint)
{
    return nvsGetControllerFingerprint(fingerprint);
}

bool AstrOsStorageManager::saveServoConfig(int boardId, servo_channel *servos, int arraySize)
{
    return nvsSaveServoConfig(boardId, servos, arraySize);
}

bool AstrOsStorageManager::loadServoConfig(int boardId, servo_channel *servos, int arraySize)
{
    return nvsLoadServoConfig(boardId, servos, arraySize);
}

/*******************************
 * ESP-NOW configs
 ********************************/

bool AstrOsStorageManager::saveEspNowPeerConfigs(espnow_peer_t *config, int arraySize)
{
    return nvsSaveEspNowPeerConfigs(config, arraySize);
}

bool AstrOsStorageManager::saveEspNowPeer(espnow_peer_t config)
{
    return nvsSaveEspNowPeer(config);
}

int AstrOsStorageManager::loadEspNowPeerConfigs(espnow_peer_t *config)
{
    return nvsLoadEspNowPeerConfigs(config);
}

bool AstrOsStorageManager::saveFile(std::string filename, std::string data)
{

#ifdef USE_SPIFFS
    return AstrOsStorageManager::saveFileSpiffs(filename, data);
#else
    return AstrOsStorageManager::saveFileSd(filename, data);
#endif
}

bool AstrOsStorageManager::deleteFile(std::string filename)
{

#ifdef USE_SPIFFS
    return AstrOsStorageManager::deleteFileSpiffs(filename);
#else
    return AstrOsStorageManager::deleteFileSd(filename);
#endif
}

std::string AstrOsStorageManager::readFile(std::string filename)
{

#ifdef USE_SPIFFS
    return AstrOsStorageManager::readFileSpiffs(filename);
#else
    return AstrOsStorageManager::readFileSd(filename);
#endif
}

bool AstrOsStorageManager::fileExists(std::string filename)
{

#ifdef USE_SPIFFS
    return AstrOsStorageManager::fileExistsSpiffs(filename);
#else
    return AstrOsStorageManager::fileExistsSd(filename);
#endif
}

std::vector<std::string> AstrOsStorageManager::listFiles(std::string folder)
{
#ifdef USE_SPIFFS
    return AstrOsStorageManager::listFilesSpiffs(folder);
#else
    return AstrOsStorageManager::listFilesSd(folder);
#endif
}

/**************************************************************
 * SD Card methods
 ***************************************************************/

bool AstrOsStorageManager::formatSdCard()
{
    char drv[3] = {'0', ':', 0};
    const size_t workbuf_size = 4096;
    void *workbuf = NULL;
    ESP_LOGI(TAG, "Formatting the SD card");

    size_t allocation_unit_size = 16 * 1024;

    workbuf = ff_memalloc(workbuf_size);
    if (workbuf == NULL)
    {
        ESP_LOGE(TAG, "Error formatting SD card: ESP_ERR_NO_MEM");
        return false;
    }

    size_t alloc_unit_size = esp_vfs_fat_get_allocation_unit_size(
        card->csd.sector_size,
        allocation_unit_size);

    MKFS_PARM param;

    param.fmt = FM_ANY;
    param.au_size = alloc_unit_size;

    FRESULT res = f_mkfs(drv, &param, workbuf, workbuf_size);
    if (res != FR_OK)
    {
        ESP_LOGE(TAG, "Error formatting SD card: f_mkfs failed (%d)", res);
        return false;
    }

    free(workbuf);

    mkdir("/sdcard/scripts", 0777);

    ESP_LOGI(TAG, "Successfully formatted the SD card");

    return true;
}

esp_err_t AstrOsStorageManager::mountSdCard()
{
    esp_err_t err;

    esp_vfs_fat_mount_config_t mountConfig;

    mountConfig.format_if_mount_failed = true;
    mountConfig.max_files = 5;
    mountConfig.allocation_unit_size = 16 * 1024;

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
        .max_transfer_sz = 4000};

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

bool AstrOsStorageManager::saveFileSd(std::string filename, std::string data)
{
    FILE *fd = NULL;

    std::string path = AstrOsStorageManager::setFilePath(filename);

    ESP_LOGI(TAG, "Saving %s", path.c_str());

    if (access(path.c_str(), F_OK) == 0)
    {
        ESP_LOGI(TAG, "File %s exists already. Deleting...", path.c_str());
        unlink(path.c_str());
    }

    fd = fopen(path.c_str(), "w");
    if (!fd)
    {
        ESP_LOGE(TAG, "Failed to create file : %s", path.c_str());
        return false;
    }

    fwrite(data.c_str(), sizeof(char), strlen(data.c_str()) + 1, fd);

    fclose(fd);

    ESP_LOGI(TAG, "Saved %s", path.c_str());

    return true;
}

bool AstrOsStorageManager::deleteFileSd(std::string filename)
{

    std::string path = AstrOsStorageManager::setFilePath(filename);

    if (access(path.c_str(), F_OK) == 0)
    {
        ESP_LOGI(TAG, "File %s exists already. Deleting...", path.c_str());
        unlink(path.c_str());
    }

    return true;
}

bool AstrOsStorageManager::fileExistsSd(std::string filename)
{
    std::string path = AstrOsStorageManager::setFilePath(filename);

    return access(path.c_str(), F_OK) == 0;
}

std::string AstrOsStorageManager::readFileSd(std::string filename)
{
    std::string path = AstrOsStorageManager::setFilePath(filename);

    if (access(path.c_str(), F_OK) != 0)
    {
        ESP_LOGE(TAG, "File does not exist: %s", path.c_str());
        return "error";
    }

    FILE *f = fopen(path.c_str(), "r");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to open %s for reading", path.c_str());
        return "error";
    }

    std::string result = "";

    char segment[100];

    while (fgets(segment, sizeof(segment), f))
    {
        result.append(segment);
    }

    fclose(f);

    return result;
}

std::vector<std::string> AstrOsStorageManager::listFilesSd(std::string folder)
{
    std::vector<std::string> result;

    const char *entrytype;
    struct dirent *entry;
    struct stat entry_stat;

    std::string entryPath = AstrOsStorageManager::setFilePath(folder);

    DIR *dir = opendir(entryPath.c_str());

    if (!dir)
    {
        ESP_LOGE(TAG, "Failed to open dir %s", entryPath.c_str());
        return result;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        entrytype = (entry->d_type == DT_DIR ? "directory" : "file");

        const char *ep = entryPath.c_str();

        if (stat(ep, &entry_stat) == -1)
        {
            ESP_LOGE(TAG, "Failed to stat %s : %s", entrytype, entry->d_name);
            continue;
        }

        result.push_back(entry->d_name);
    }

    closedir(dir);

    return result;
}

/**************************************************************
 * SPIFFS methods
 ***************************************************************/

bool AstrOsStorageManager::saveFileSpiffs(std::string filename, std::string data)
{
    esp_vfs_spiffs_conf_t config = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&config);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return false;
    }

    ESP_LOGE(TAG, "Creating file: %s", filename.c_str());

    std::string path = AstrOsStorageManager::setFilePath(filename);

    FILE *f = fopen(path.c_str(), "w");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return false;
    }

    fprintf(f, data.c_str());
    fclose(f);

    esp_vfs_spiffs_unregister(NULL);

    return true;
}

bool AstrOsStorageManager::deleteFileSpiffs(std::string filename)
{
    esp_vfs_spiffs_conf_t config = {
        .base_path = MOUNT_POINT,
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&config);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return false;
    }

    std::string path = AstrOsStorageManager::setFilePath(filename);

    struct stat st;
    if (stat(path.c_str(), &st) == 0)
    {
        unlink(path.c_str());
        return true;
    }

    ESP_LOGI(TAG, "File %s does not exist!", filename.c_str());

    esp_vfs_spiffs_unregister(NULL);
    return false;
}

bool AstrOsStorageManager::fileExistsSpiffs(std::string filename)
{
    esp_vfs_spiffs_conf_t config = {
        .base_path = MOUNT_POINT,
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&config);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return false;
    }

    std::string path = AstrOsStorageManager::setFilePath(filename);

    struct stat st;
    if (stat(path.c_str(), &st) == 0)
    {
        esp_vfs_spiffs_unregister(NULL);
        return true;
    }

    ESP_LOGI(TAG, "File %s does not exist!", filename.c_str());

    esp_vfs_spiffs_unregister(NULL);
    return false;
}

std::string AstrOsStorageManager::readFileSpiffs(std::string filename)
{
    esp_vfs_spiffs_conf_t config = {
        .base_path = MOUNT_POINT,
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&config);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return "error";
    }

    std::string path = AstrOsStorageManager::setFilePath(filename);

    FILE *file = fopen(path.c_str(), "r");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "File does not exist!");
        return "error";
    }

    std::string result = "";

    char segment[256];
    while (fgets(segment, sizeof(segment), file) != NULL)
    {
        result.append(segment);
    }
    fclose(file);

    esp_vfs_spiffs_unregister(NULL);

    return result;
}

std::vector<std::string> AstrOsStorageManager::listFilesSpiffs(std::string folder)
{
    std::vector<std::string> result;

    esp_vfs_spiffs_conf_t config = {
        .base_path = MOUNT_POINT,
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&config);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return result;
    }

    std::string path = AstrOsStorageManager::setFilePath(folder);

    DIR *dir = opendir(path.c_str());
    if (dir == NULL)
    {
        esp_vfs_spiffs_unregister(NULL);
        return result;
    }

    while (true)
    {
        struct dirent *de = readdir(dir);
        if (!de)
        {
            break;
        }

        result.push_back(de->d_name);
    }

    closedir(dir);
    esp_vfs_spiffs_unregister(NULL);

    return result;
}

/**************************************************************
 * Utility methods
 ***************************************************************/

std::string AstrOsStorageManager::setFilePath(std::string filename)
{

    std::string out = MOUNT_POINT + std::string("/") + filename;

    return out;
}