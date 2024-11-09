#include <pca9685.hpp>

#include <esp_system.h>
#include <driver/i2c.h>
#include <math.h>
#include <esp_log.h>


Pca9685::Pca9685() {}
Pca9685::~Pca9685() {}

/// @brief 
/// @param i2cMaster 
/// @param addr 
/// @param frequency 
/// @param slop positive value decreases frequency
/// @return 
esp_err_t Pca9685::Init(I2cMaster i2cMaster, uint8_t addr, uint16_t frequency, int slop)
{
    this->i2cMaster = i2cMaster;
    this->address = addr;
   

    if (!this->i2cMaster.DeviceExists(this->address))
    {
        ESP_LOGI("PCA9685", "Device does not exist at address %d", this->address);
        return ESP_FAIL;
    }

    if (!this->reset())
    {
        ESP_LOGI("PCA9685", "Failed to reset at address %d", this->address);
        return ESP_FAIL;
    }

    if (!this->SetFrequency(frequency, slop))
    {
        ESP_LOGI("PCA9685", "Failed to set frequency at address %d", this->address);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/// @brief Get I2c address of the PCA9685
/// @return 
uint8_t Pca9685::GetAddress()
{
    return this->address;
}

/// @brief Sets the frequency of the PCA9685. Page 25 of the datasheet
// documents the process and has formula for calculating the frequency prescale.
/// @param freq 
/// @param slop this value is to compensate the clock inconsistancy, the default value is 1
/// @return 
bool Pca9685::SetFrequency(uint16_t freq, int slop)
{
    this->frequency = freq;
    this->slop = slop;

    // Send to sleep
    if (!this->i2cMaster.WriteByte(this->address, MODE1, 0x10))
    {
        return false;
    }

    // Set prescaler
    uint8_t prescale_val = round((CLOCK_FREQ / (4096 * freq)) - 1) + slop;

    if (!this->i2cMaster.WriteByte(this->address, PRE_SCALE, prescale_val))
    {
        return false;
    }

    // Clear sleep
    if (!this->i2cMaster.WriteByte(this->address, MODE1, 0x00))
    {
        return false;
    }
    
    // waitfor oscillator to stabilize
    vTaskDelay( 10 / portTICK_PERIOD_MS);

    // reset again
    Pca9685::reset();
    
    // Write 0xa0 for auto increment LED0_x after received cmd
    return this->i2cMaster.WriteByte(this->address, MODE1, 0xa0);
}

uint16_t Pca9685::GetFrequency()
{
    return this->frequency;
}

bool Pca9685::SetPwm(uint8_t channel, uint16_t on, uint16_t off)
{
    uint8_t pinAddress = 0x6 + 4 * channel;

    return this->i2cMaster.WriteTwoWords(this->address, pinAddress, on, off);
}

bool Pca9685::GetPwm(uint8_t channel, uint16_t *on, uint16_t *off)
{
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

bool Pca9685::reset()
{
   return this->i2cMaster.WriteByte(this->address, MODE1, 0x80);
}

bool Pca9685::getPWMDetail(uint8_t channel, uint8_t *dataReadOn0, uint8_t *dataReadOn1, uint8_t *dataReadOff0, uint8_t *dataReadOff1)
{
    uint8_t pinAddress = 0x6 + 4 * channel;
    uint16_t readOn0;
    uint16_t readOn1;

    if (this->i2cMaster.ReadTwoWords(this->address, pinAddress, &readOn0, &readOn1))
    {
        return false;
    }

    *dataReadOn0 = readOn0 & 0xff;
    *dataReadOn1 = readOn0 >> 8;
     

    pinAddress = 0x8 + 4 * channel;
    
    if (this->i2cMaster.ReadTwoWords(this->address, pinAddress, &readOn0, &readOn1))
    {
        return false;
    }

    *dataReadOff0 = readOn0 & 0xff;
    *dataReadOff1 = readOn0 >> 8;

    return true;
}
