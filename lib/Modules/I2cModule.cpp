#include <I2cModule.h>
#include <AnimationCommand.h>

#include "esp_system.h"
#include <esp_log.h>
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
    ESP_LOGI(TAG, "Sending Command => %s", cmd); 
}