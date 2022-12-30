#ifndef PCA9685_H
#define PCA9685_H

#include "esp_system.h"
#include <driver/i2c.h>

#define ACK_CHECK_EN    0x1     /*!< I2C master will check ack from slave */
#define ACK_CHECK_DIS   0x0     /*!< I2C master will not check ack from slave */
#define ACK_VAL         0x0     /*!< I2C ack value */
#define NACK_VAL        0x1     /*!< I2C nack value */

#define MODE1           0x00    /*!< Mode register 1 */
#define MODE2           0x01    /*!< Mode register 2 */
#define SUBADR1         0x02    /*!< I2C-bus subaddress 1 */
#define SUBADR2         0x03    /*!< I2C-bus subaddress 2 */
#define SUBADR3         0x04    /*!< I2C-bus subaddress 3 */

#define PRE_SCALE       0xfe    /*!< prescaler for output frequency */
#define CLOCK_FREQ      25000000.0  /*!< 25MHz default osc clock */


class Pca9685
{
private:
    uint8_t address;
    esp_err_t reset();
    esp_err_t setFrequency(uint16_t freq);
    esp_err_t getFrequency(uint16_t* freq);
    esp_err_t getPWMDetail(uint8_t channel, uint8_t* dataReadOn0, uint8_t* dataReadOn1, uint8_t* dataReadOff0, uint8_t* dataReadOff1);
    esp_err_t writeByte(uint8_t regaddr, uint8_t value);
    esp_err_t writeWord(uint8_t regaddr, uint16_t value);
    esp_err_t writeTwoWord(uint8_t regaddr, uint16_t valueOn, uint16_t valueOff);
    esp_err_t readWord(uint8_t regaddr, uint8_t* valueA);
    esp_err_t readTwoWord(uint8_t regaddr, uint8_t* valueA, uint8_t* valueB);
public:
    Pca9685(/* args */);
    ~Pca9685();
    esp_err_t Init(uint8_t address, uint16_t frequency);
    esp_err_t setPwm(uint8_t channel, uint16_t on, uint16_t off);
    esp_err_t getPwm(uint8_t channel, uint16_t* on, uint16_t* off);
    uint8_t getAddress();
};

#endif