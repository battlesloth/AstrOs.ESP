#include <pca9685.h>

#include "esp_system.h"
#include <esp_log.h>
#include <driver/i2c.h>
#include <math.h>


#define WRITE_BIT I2C_MASTER_WRITE           
#define READ_BIT I2C_MASTER_READ   

static const char *TAG = "PCA9685_Driver";

Pca9685::Pca9685(/* args */) {}
Pca9685::~Pca9685() {}

esp_err_t Pca9685::Init(uint8_t addr, uint16_t frequency)
{
    esp_err_t result = ESP_OK;

    Pca9685::address = addr;

    result = Pca9685::reset();
    if (result != ESP_OK){
        ESP_ERROR_CHECK(result);
        return result;
    }
    
    result = Pca9685::setFrequency(frequency);
    if (result != ESP_OK){
        ESP_ERROR_CHECK(result);
        return result;
    }
   
    return result;
}

uint8_t Pca9685::getAddress(){
    return Pca9685::address;
}

esp_err_t Pca9685::reset(){

    esp_err_t result;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);

    i2c_master_write_byte(cmd, (Pca9685::address << 1) | WRITE_BIT, ACK_CHECK_EN);

    i2c_master_write_byte(cmd, MODE1, ACK_CHECK_EN);

    i2c_master_write_byte(cmd, 0x80, ACK_CHECK_EN);
 
    i2c_master_stop(cmd);
  
    result = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(1000));
 
    i2c_cmd_link_delete(cmd);

    vTaskDelay(pdMS_TO_TICKS(50));

    return result;
}

esp_err_t Pca9685::setFrequency(uint16_t freq){
    
    esp_err_t result;

   // Send to sleep
    result = Pca9685::writeByte(MODE1, 0x10);
    if (result != ESP_OK){
        return result;
    }

    // Set prescaler
    // calculation on page 25 of datasheet
    uint8_t prescale_val = round((CLOCK_FREQ / 4096 / (0.9*freq)) - 1+0.5);
    result = Pca9685::writeByte(PRE_SCALE, prescale_val);
    if (result != ESP_OK) {
        return result;
    }

    // reset again
    Pca9685::reset();

    // Send to sleep again
    result = Pca9685::writeByte(MODE1, 0x10);
    if (result != ESP_OK) {
        return result;
    }

    // wait
    vTaskDelay(pdMS_TO_TICKS(5));

    // Write 0xa0 for auto increment LED0_x after received cmd
    result = Pca9685::writeByte(MODE1, 0xa0);

    return result;
}

esp_err_t Pca9685::getFrequency(uint16_t* freq){
    
    esp_err_t ret;

    uint8_t prescale;
    ret = Pca9685::readWord(PRE_SCALE, &prescale);
    *freq = CLOCK_FREQ /((uint32_t)4096 * (prescale +1));

    return ret;
}

esp_err_t Pca9685::setPwm(uint8_t channel, uint16_t on, uint16_t off){
    
    esp_err_t ret;

    uint8_t pinAddress = 0x6 + 4 * channel;
    ret = Pca9685::writeTwoWord(pinAddress & 0xff, on, off);

    return ret;
}

esp_err_t Pca9685::getPwm(uint8_t channel, uint16_t* on, uint16_t* off){
    
    esp_err_t result;

    uint8_t readPWMValueOn0;
    uint8_t readPWMValueOn1;
    uint8_t readPWMValueOff0;
    uint8_t readPWMValueOff1;

    result = getPWMDetail(channel, &readPWMValueOn0, &readPWMValueOn1, &readPWMValueOff0, &readPWMValueOff1);

    *on = (readPWMValueOn1 << 8) | readPWMValueOn0;
    *off = (readPWMValueOff1 << 8) | readPWMValueOff0;

    return result;
}

esp_err_t Pca9685::getPWMDetail(uint8_t channel, uint8_t* dataReadOn0, uint8_t* dataReadOn1, uint8_t* dataReadOff0, uint8_t* dataReadOff1){
    
    esp_err_t result;

     uint8_t pinAddress = 0x6 + 4 * channel;

    result = readTwoWord(pinAddress, dataReadOn0, dataReadOn1);
    if (result != ESP_OK) {
        return result;
    }

    pinAddress = 0x8 + 4 * channel;
    result = readTwoWord(pinAddress, dataReadOff0, dataReadOff1);

    return result;
}

esp_err_t Pca9685::writeByte(uint8_t regaddr, uint8_t value){

    esp_err_t result;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (Pca9685::address << 1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
    i2c_master_write_byte(cmd, regaddr, ACK_CHECK_EN);
    i2c_master_write_byte(cmd, value, NACK_VAL);
    i2c_master_stop(cmd);
    result = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000/portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);

    return result;
}

esp_err_t Pca9685::writeWord(uint8_t regaddr, uint16_t value){

    esp_err_t result;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (Pca9685::address << 1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
    i2c_master_write_byte(cmd, regaddr, ACK_CHECK_EN);
    i2c_master_write_byte(cmd, value & 0xff, ACK_VAL);
    i2c_master_write_byte(cmd, value >> 8, NACK_VAL);
    i2c_master_stop(cmd);
    result = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000/portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);

    return result;
}

esp_err_t Pca9685::writeTwoWord(uint8_t regaddr, uint16_t valueOn, uint16_t valueOff){
    
    esp_err_t result;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (Pca9685::address << 1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
    i2c_master_write_byte(cmd, regaddr, ACK_CHECK_EN);
    i2c_master_write_byte(cmd, valueOn & 0xff, ACK_VAL);
    i2c_master_write_byte(cmd, valueOn >> 8, NACK_VAL);
    i2c_master_write_byte(cmd, valueOff & 0xff, ACK_VAL);
    i2c_master_write_byte(cmd, valueOff >> 8, NACK_VAL);
    i2c_master_stop(cmd);
    result = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000/portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);

    return result;  
}

esp_err_t Pca9685::readWord(uint8_t regaddr, uint8_t* value){
    
    esp_err_t ret;

    uint8_t valueA;
    uint8_t valueB;

    ret = readTwoWord(regaddr, &valueA, &valueB);
    if (ret != ESP_OK) {
        return ret;
    }

    *value = (valueB << 8) | valueA;

    return ret;
}

esp_err_t Pca9685::readTwoWord(uint8_t regaddr, uint8_t* valueA, uint8_t* valueB){
    
    esp_err_t result;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (Pca9685::address << 1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
    i2c_master_write_byte(cmd, regaddr, ACK_CHECK_EN);
    i2c_master_stop(cmd);
    result = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_RATE_MS);
    i2c_cmd_link_delete(cmd);
    if (result != ESP_OK) {
        return result;
    }
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (Pca9685::address << 1) | I2C_MASTER_READ, ACK_CHECK_EN);
    i2c_master_read_byte(cmd, valueA, (i2c_ack_type_t) ACK_VAL);
    i2c_master_read_byte(cmd, valueB, (i2c_ack_type_t) NACK_VAL);
    i2c_master_stop(cmd);
    result = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_RATE_MS);
    i2c_cmd_link_delete(cmd);
    
    return result;
}
