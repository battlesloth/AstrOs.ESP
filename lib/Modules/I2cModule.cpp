#include <I2cModule.h>
#include <AnimationCommand.h>

#include "esp_system.h"
#include <esp_log.h>
#include <driver/i2c.h>
#include <string>
#include <string.h>

static const char *TAG = "I2cModule";

I2cModule I2cMod;


I2cModule::I2cModule(/* args */)
{
}

I2cModule::~I2cModule()
{
}

esp_err_t I2cModule::Init(){
    esp_err_t result = ESP_OK ;

    return result;
}


void I2cModule::SendCommand(const char* cmd){
    
    auto command = I2cCommand(cmd);

    ESP_LOGI(TAG, "Sending Command => %s", cmd); 


    I2cModule::write(command.channel, (uint8_t*) command.value.c_str(),  command.value.size());
}


esp_err_t I2cModule::write(uint8_t addr, uint8_t *data, size_t size)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | WRITE_BIT, ACK_CHECK_EN);
    i2c_master_write(cmd, data, size, ACK_CHECK_EN);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    return ret;
}