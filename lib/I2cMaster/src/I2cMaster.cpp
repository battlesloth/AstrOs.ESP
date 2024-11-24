#include "I2cMaster.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>

#define I2C_TIMEOUT_MS 50
#define SEMAPHORE_TIMEOUT 1000

I2cMaster i2cMaster;
SemaphoreHandle_t i2cBusMutext;

I2cMaster::I2cMaster()
{
    i2cBusMutext = xSemaphoreCreateMutex();
}

I2cMaster::~I2cMaster() {}

esp_err_t I2cMaster::Init(gpio_num_t sda, gpio_num_t scl)
{
    i2c_config_t conf;

    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = sda;
    conf.scl_io_num = scl;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 100000;
    conf.clk_flags = 0;

    esp_err_t err = i2c_param_config(I2C_PORT, &conf);
    
    if (err != ESP_OK)
    {
        ESP_LOGE("I2C", "Failed to configure I2C: %s", esp_err_to_name(err));
        return err;
    }
    
    return i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0);

}

bool I2cMaster::DeviceExists(uint8_t addr)
{
    esp_err_t res = ESP_OK;

    if (xSemaphoreTake(i2cBusMutext, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE)
    {
            i2c_cmd_handle_t cmd = i2c_cmd_link_create();
            i2c_master_start(cmd);
            i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, 1 /* expect ack */);
            i2c_master_stop(cmd);
    
            res = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10 / portTICK_PERIOD_MS);
        xSemaphoreGive(i2cBusMutext);
    } 
    return res == ESP_OK;
}

bool I2cMaster::WriteByte(uint8_t addr, uint8_t registerAddr, uint8_t byte)
{
    esp_err_t err = ESP_OK;

    if (xSemaphoreTake(i2cBusMutext, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE)
    {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
        i2c_master_write_byte(cmd, registerAddr, ACK_CHECK_EN);
        i2c_master_write_byte(cmd, byte, NACK_VAL);
        i2c_master_stop(cmd);
        err = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);
        xSemaphoreGive(i2cBusMutext);
    }

    if (err != ESP_OK)
    {
        ESP_LOGE("I2C", "Failed to write to device: %s", esp_err_to_name(err));
    }

    return err == ESP_OK;
}

bool I2cMaster::WriteWord(uint8_t addr, uint8_t registerAddr, uint16_t word)
{
    esp_err_t err = ESP_OK;

    if (xSemaphoreTake(i2cBusMutext, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE)
    {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
        i2c_master_write_byte(cmd, registerAddr, ACK_CHECK_EN);
        i2c_master_write_byte(cmd, word & 0xff, ACK_VAL);
        i2c_master_write_byte(cmd, word >> 8, NACK_VAL);
        i2c_master_stop(cmd);
        err = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);

        xSemaphoreGive(i2cBusMutext);
    }

    if (err != ESP_OK)
    {
        ESP_LOGE("I2C", "Failed to write to device: %s", esp_err_to_name(err));
    }

    return err == ESP_OK;
}

bool I2cMaster::WriteTwoWords(uint8_t addr, uint8_t registerAddr, uint16_t word1, uint16_t word2){

    esp_err_t err = ESP_OK;

    if (xSemaphoreTake(i2cBusMutext, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE)
    {
        
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
        i2c_master_write_byte(cmd, registerAddr, ACK_CHECK_EN);
        i2c_master_write_byte(cmd, word1 & 0xff, ACK_VAL);
        i2c_master_write_byte(cmd, word1 >> 8, NACK_VAL);
        i2c_master_write_byte(cmd, word2 & 0xff, ACK_VAL);
        i2c_master_write_byte(cmd, word2 >> 8, NACK_VAL);
        i2c_master_stop(cmd);
        err = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);

        xSemaphoreGive(i2cBusMutext);
    }

    if (err != ESP_OK)
    {
        ESP_LOGE("I2C", "Failed to write to device: %s", esp_err_to_name(err));
    }

    return err == ESP_OK;
}

bool I2cMaster::ReadByte(uint8_t addr, uint8_t registerAddr, uint8_t *data)
{
    esp_err_t err = ESP_OK;

    if (xSemaphoreTake(i2cBusMutext, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE)
    {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
        i2c_master_write_byte(cmd, registerAddr, ACK_CHECK_EN);
        i2c_master_stop(cmd);
        err = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);
        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to write to device: %s", esp_err_to_name(err));
            xSemaphoreGive(i2cBusMutext);
            return false;
        }
       
        cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, ACK_CHECK_EN);
        i2c_master_read_byte(cmd, data, (i2c_ack_type_t)NACK_VAL);
        i2c_master_stop(cmd);
        err = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);

        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to read from device: %s", esp_err_to_name(err));
            xSemaphoreGive(i2cBusMutext);
            return false;
        }
        
        xSemaphoreGive(i2cBusMutext);
    }

    if (err != ESP_OK)
    {
        ESP_LOGE("I2C", "Failed to read from device: %s", esp_err_to_name(err));
    }

    return err == ESP_OK;
}

bool I2cMaster::ReadWord(uint8_t addr, uint8_t registerAddr, uint16_t *data)
{
esp_err_t err = ESP_OK;

    if (xSemaphoreTake(i2cBusMutext, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE)
    {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
        i2c_master_write_byte(cmd, registerAddr, ACK_CHECK_EN);
        i2c_master_stop(cmd);
        err = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);
        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to write to device: %s", esp_err_to_name(err));
            xSemaphoreGive(i2cBusMutext);
            return false;
        }
        cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);

        uint8_t buffer1;
        uint8_t buffer2;

        cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, ACK_CHECK_EN);
        i2c_master_read_byte(cmd, &buffer1, (i2c_ack_type_t)ACK_VAL);
        i2c_master_read_byte(cmd, &buffer2, (i2c_ack_type_t)NACK_VAL);
        i2c_master_stop(cmd);
        err = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);

        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to read from device: %s", esp_err_to_name(err));
            xSemaphoreGive(i2cBusMutext);
            return false;
        }

        *data = (buffer1 << 8) | buffer2;
        
        i2c_cmd_link_delete(cmd);
        xSemaphoreGive(i2cBusMutext);
    }

    if (err != ESP_OK)
    {
        ESP_LOGE("I2C", "Failed to read from device: %s", esp_err_to_name(err));
    }

    return err == ESP_OK;
}

bool I2cMaster::ReadTwoWords(uint8_t addr, uint8_t registerAddr, uint16_t *data1, uint16_t *data2)
{
    esp_err_t err = ESP_OK;

    if (xSemaphoreTake(i2cBusMutext, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE)
    {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
        i2c_master_write_byte(cmd, registerAddr, ACK_CHECK_EN);
        i2c_master_stop(cmd);
        err = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);
        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to write to device: %s", esp_err_to_name(err));
            xSemaphoreGive(i2cBusMutext);
            return false;
        }
        cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);

        uint8_t buffer1;
        uint8_t buffer2;

        cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, ACK_CHECK_EN);
        i2c_master_read_byte(cmd, &buffer1, (i2c_ack_type_t)ACK_VAL);
        i2c_master_read_byte(cmd, &buffer2, (i2c_ack_type_t)NACK_VAL);
        i2c_master_stop(cmd);
        err = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);

        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to read from device:%s", esp_err_to_name(err));
            xSemaphoreGive(i2cBusMutext);
            return false;
        }

        *data1 = (buffer1 << 8) | buffer2;
        
        cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, ACK_CHECK_EN);
        i2c_master_read_byte(cmd, &buffer1, (i2c_ack_type_t)ACK_VAL);
        i2c_master_read_byte(cmd, &buffer2, (i2c_ack_type_t)NACK_VAL);
        i2c_master_stop(cmd);
        err = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);

        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to read from device:%s", esp_err_to_name(err));     
            xSemaphoreGive(i2cBusMutext);
            return false;
        }
        
        *data2 = (buffer1 << 8) | buffer2;

        i2c_cmd_link_delete(cmd);
        xSemaphoreGive(i2cBusMutext);
    }

    if (err != ESP_OK)
    {
        ESP_LOGE("I2C", "Failed to read from device:%s", esp_err_to_name(err));
    }

    return err == ESP_OK;
}

/*
#include "I2cMaster.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <string.h>

#define I2C_TIMEOUT_MS 50
#define SEMAPHORE_TIMEOUT 1000

I2cMaster i2cMaster;
SemaphoreHandle_t i2cBusMutext;

I2cMaster::I2cMaster()
{
    i2cBusMutext = xSemaphoreCreateMutex();
}

I2cMaster::~I2cMaster() {}

esp_err_t I2cMaster::Init(gpio_num_t sda, gpio_num_t scl)
{


    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0));
    i2c_master_bus_config_t busConfig = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        }};
    return i2c_new_master_bus(&busConfig, &this->i2cBusHandle);
}

bool I2cMaster::DeviceExists(uint16_t addr)
{
    if (xSemaphoreTake(i2cBusMutext, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE)
    {
        return i2c_master_probe(this->i2cBusHandle, addr, I2C_TIMEOUT_MS) == ESP_OK;
        xSemaphoreGive(i2cBusMutext);
    }
    return false;
}

bool I2cMaster::WriteByte(uint16_t addr, uint8_t registerAddr, uint8_t byte)
{
    esp_err_t err = ESP_OK;

    i2c_device_config_t i2c_dev_conf = {
        .device_address = addr,
        .scl_speed_hz = i2cFrequency,
    };

    i2c_master_dev_handle_t devHandle;

    if (xSemaphoreTake(i2cBusMutext, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE)
    {
        if (i2c_master_bus_add_device(i2cBusHandle, &i2c_dev_conf, &devHandle) != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to add device to bus");
            return false;
        }

        uint8_t *data = (uint8_t *)malloc(2);
        data[0] = registerAddr;
        data[1] = byte;

        err = i2c_master_transmit(devHandle, data, 2, I2C_TIMEOUT_MS);

        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to write to device: %d", err);
        }

        free(data);
        err = i2c_master_bus_rm_device(devHandle);
        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to remove device from bus: %d", err);
        }

        xSemaphoreGive(i2cBusMutext);
    }

    return err == ESP_OK;
}

bool I2cMaster::ReadByte(uint16_t addr, uint8_t registerAddr, uint8_t *data)
{
    esp_err_t err = ESP_OK;
    i2c_device_config_t i2c_dev_conf = {
        .device_address = addr,
        .scl_speed_hz = i2cFrequency,
    };

    i2c_master_dev_handle_t devHandle;

    if (xSemaphoreTake(i2cBusMutext, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE)
    {
        if (i2c_master_bus_add_device(i2cBusHandle, &i2c_dev_conf, &devHandle) != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to add device to bus");
            return false;
        }

        err = i2c_master_transmit_receive(devHandle, &registerAddr, 1, data, 1, I2C_TIMEOUT_MS);
        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to read from device: %d", err);
        }
        else
        {
            ESP_LOGI("I2C", "Read data: %02x %02x %02x %02x", data[0], data[1], data[2], data[3]);
        }

        free(data);
        err = i2c_master_bus_rm_device(devHandle);

        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to remove device from bus: %d", err);
        }

        xSemaphoreGive(i2cBusMutext);
    }
    return err == ESP_OK;
}

bool I2cMaster::WriteBytes(uint16_t addr, uint8_t registerAddr, uint8_t *data, size_t len)
{
    esp_err_t err = ESP_OK;
    i2c_device_config_t i2c_dev_conf = {
        .device_address = addr,
        .scl_speed_hz = i2cFrequency,
    };

    i2c_master_dev_handle_t devHandle;
    if (xSemaphoreTake(i2cBusMutext, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE)
    {
        if (i2c_master_bus_add_device(i2cBusHandle, &i2c_dev_conf, &devHandle) != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to add device to bus");
            return false;
        }

        uint8_t *dataToSend = (uint8_t *)malloc(len + 1);
        dataToSend[0] = registerAddr;
        memcpy(dataToSend + 1, data, len);

        err = i2c_master_transmit(devHandle, dataToSend, len + 1, I2C_TIMEOUT_MS);
        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to write to device: %d", err);
        }

        free(dataToSend);
        err = i2c_master_bus_rm_device(devHandle);

        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to remove device from bus: %d", err);
        }

        xSemaphoreGive(i2cBusMutext);
    }
    return err == ESP_OK;
}

bool I2cMaster::WriteWord(uint16_t addr, uint8_t registerAddr, uint16_t word)
{
    esp_err_t err = ESP_OK;
    i2c_device_config_t i2c_dev_conf = {
        .device_address = addr,
        .scl_speed_hz = i2cFrequency,
    };

    i2c_master_dev_handle_t devHandle;

    if (xSemaphoreTake(i2cBusMutext, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE)
    {
        if (i2c_master_bus_add_device(i2cBusHandle, &i2c_dev_conf, &devHandle) != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to add device to bus");
            return false;
        }

        uint8_t *data = (uint8_t *)malloc(3);
        data[0] = registerAddr;
        data[1] = word & 0x00FF;
        data[2] = word >> 8;

        err = i2c_master_transmit(devHandle, data, 3, I2C_TIMEOUT_MS);
        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to write to device: %d", err);
        }

        free(data);
        err = i2c_master_bus_rm_device(devHandle);

        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to remove device from bus: %d", err);
        }

        xSemaphoreGive(i2cBusMutext);
    }
    return err == ESP_OK;
}

bool I2cMaster::ReadWord(uint16_t addr, uint8_t registerAddr, uint16_t *data)
{
    esp_err_t err = ESP_OK;
    i2c_device_config_t i2c_dev_conf = {
        .device_address = addr,
        .scl_speed_hz = i2cFrequency,
    };

    i2c_master_dev_handle_t devHandle;
    if (xSemaphoreTake(i2cBusMutext, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE)
    {
        if (i2c_master_bus_add_device(i2cBusHandle, &i2c_dev_conf, &devHandle) != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to add device to bus");
            return false;
        }

        uint8_t *dataRead = (uint8_t *)malloc(2);
        err = i2c_master_transmit_receive(devHandle, &registerAddr, 1, dataRead, 2, I2C_TIMEOUT_MS);
        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to read from device: %d", err);
        }

        *data = (dataRead[1] << 8) | dataRead[0];

        free(dataRead);
        err = i2c_master_bus_rm_device(devHandle);

        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to remove device from bus: %d", err);
        }

        xSemaphoreGive(i2cBusMutext);
    }
    return err == ESP_OK;
}

bool I2cMaster::WriteTwoWords(uint16_t addr, uint8_t registerAddr, uint16_t data1, uint16_t data2)
{
    esp_err_t err = ESP_OK;
    i2c_device_config_t i2c_dev_conf = {
        .device_address = addr,
        .scl_speed_hz = i2cFrequency,
    };

    i2c_master_dev_handle_t devHandle;

    if (xSemaphoreTake(i2cBusMutext, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE)
    {
        if (i2c_master_bus_add_device(i2cBusHandle, &i2c_dev_conf, &devHandle) != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to add device to bus");
            return false;
        }

        uint8_t *data = (uint8_t *)malloc(5);
        data[0] = registerAddr;
        data[1] = data1 & 0x00FF;
        data[2] = data1 >> 8;
        data[3] = data2 & 0x00FF;
        data[4] = data2 >> 8;

        err = i2c_master_transmit(devHandle, data, 5, I2C_TIMEOUT_MS);
        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to write to device: %d", err);
        }

        free(data);
        err = i2c_master_bus_rm_device(devHandle);

        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to remove device from bus: %d", err);
        }

        xSemaphoreGive(i2cBusMutext);
    }
    return err == ESP_OK;
}

bool I2cMaster::ReadTwoWords(uint16_t addr, uint8_t registerAddr, uint16_t *data1, uint16_t *data2)
{
    esp_err_t err = ESP_OK;
    i2c_device_config_t i2c_dev_conf = {
        .device_address = addr,
        .scl_speed_hz = i2cFrequency,
    };

    i2c_master_dev_handle_t devHandle;

    if (xSemaphoreTake(i2cBusMutext, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE)
    {
        if (i2c_master_bus_add_device(i2cBusHandle, &i2c_dev_conf, &devHandle) != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to add device to bus");
            return false;
        }

        uint8_t *dataRead = (uint8_t *)malloc(4);

        err = i2c_master_transmit_receive(devHandle, &registerAddr, 1, dataRead, 4, I2C_TIMEOUT_MS);
        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to read from device: %d", err);
        }

        *data1 = (dataRead[1] << 8) | dataRead[0];
        *data2 = (dataRead[3] << 8) | dataRead[2];

        free(dataRead);
        err = i2c_master_bus_rm_device(devHandle);

        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to remove device from bus: %d", err);
        }

        xSemaphoreGive(i2cBusMutext);
    }
    return err == ESP_OK;
}
*/