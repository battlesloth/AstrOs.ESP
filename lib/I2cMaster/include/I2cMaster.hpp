#ifndef __I2CMASTER_HPP__
#define __I2CMASTER_HPP__

#include <driver/i2c.h>

#define I2C_PORT I2C_NUM_0
#define I2C_FREQ_HZ 100 * 1000

#define ACK_CHECK_EN 0x1  /*!< I2C master will check ack from slave */
#define ACK_CHECK_DIS 0x0 /*!< I2C master will not check ack from slave */
#define ACK_VAL 0x0       /*!< I2C ack value */
#define NACK_VAL 0x1      /*!< I2C nack value */

class I2cMaster
{
private:

public:
    I2cMaster();
    ~I2cMaster();
    esp_err_t Init(gpio_num_t sda, gpio_num_t scl);

    bool DeviceExists(uint8_t addr);

    bool WriteByte(uint8_t addr, uint8_t registerAddr, uint8_t data);
    bool WriteWord(uint8_t addr, uint8_t registerAddr, uint16_t data);
    bool WriteTwoWords(uint8_t addr, uint8_t registerAddr, uint16_t data1, uint16_t data2);

    bool ReadByte(uint8_t addr, uint8_t registerAddr, uint8_t *data);
    bool ReadWord(uint8_t addr, uint8_t registerAddr, uint16_t *data);
    bool ReadTwoWords(uint8_t addr, uint8_t registerAddr, uint16_t *data1, uint16_t *data2);
};



extern I2cMaster i2cMaster;

#endif