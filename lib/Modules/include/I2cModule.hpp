#ifndef I2CMODULE_HPP
#define I2CMODULE_HPP

#include <AnimationCommand.hpp>

#include <esp_system.h>
#include <string>

#define WRITE_BIT I2C_MASTER_WRITE /*!< I2C master write */
#define READ_BIT I2C_MASTER_READ   /*!< I2C master read */

#define ACK_CHECK_EN 0x1  /*!< I2C master will check ack from slave */
#define ACK_CHECK_DIS 0x0 /*!< I2C master will not check ack from slave */
#define ACK_VAL 0x0       /*!< I2C ack value */
#define NACK_VAL 0x1      /*!< I2C nack value */

class I2cModule
{
private:
    esp_err_t write(uint8_t addr, uint8_t *data_wr, size_t size);
    void writeSsd1306(int line, std::string value);
    void clearSsd1306(int line);

public:
    I2cModule(/* args */);
    ~I2cModule();
    esp_err_t Init();
    void SendCommand(uint8_t *cmd);
    void WriteDisplay(uint8_t *cmd);
};

extern I2cModule I2cMod;

#endif