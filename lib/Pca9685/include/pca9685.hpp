#ifndef PCA9685_HPP
#define PCA9685_HPP

#include <esp_system.h>
#include <driver/i2c.h>
#include <I2cMaster.hpp>

#define MODE1 0x00   /*!< Mode register 1 */
#define MODE2 0x01   /*!< Mode register 2 */
#define SUBADR1 0x02 /*!< I2C-bus subaddress 1 */
#define SUBADR2 0x03 /*!< I2C-bus subaddress 2 */
#define SUBADR3 0x04 /*!< I2C-bus subaddress 3 */

#define PRE_SCALE 0xfe        /*!< prescaler address for output frequency */
#define CLOCK_FREQ 25000000.0 /*!< 25MHz default osc clock */

class Pca9685
{
private:
    I2cMaster i2cMaster;
    uint8_t address;
    uint16_t frequency;
    int slop;
    bool reset();
    bool getPWMDetail(uint8_t channel, uint8_t *dataReadOn0, uint8_t *dataReadOn1, uint8_t *dataReadOff0, uint8_t *dataReadOff1);
public:
    Pca9685(/* args */);
    ~Pca9685();
    esp_err_t Init(I2cMaster i2cMaster, uint8_t address, uint16_t frequency, int slop);
    bool SetFrequency(uint16_t freq, int slop);
    uint16_t GetFrequency();
   
    bool SetPwm(uint8_t channel, uint16_t on, uint16_t off);
    bool GetPwm(uint8_t channel, uint16_t *on, uint16_t *off);
    uint8_t GetAddress();
};

#endif