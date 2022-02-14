
#include <KangarooInterface.h>
#include <AstrOsUtility.h>
#include "esp_system.h"
#include "driver/uart.h"
#include <esp_log.h>
#include "string.h"

#define UART_PORT UART_NUM_1

static const int RX_BUF_SIZE = 1024;

esp_err_t init_kangaroo_interface(int baud_rate, int rx_pin, int tx_pin)
{
    const uart_config_t uart_config = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE};

    esp_err_t err = uart_param_config(UART_PORT, &uart_config);
    if (err != ESP_OK)
    {
        return err;
    }

    err = uart_set_pin(UART_PORT, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    if (err != ESP_OK)
    {
        return err;
    }

    return uart_driver_install(UART_PORT, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
}

static int sendData(const char *logName, const char *data)
{
    const int len = strlen(data);
    const int txBytes = uart_write_bytes(UART_PORT, data, len);
    ESP_LOGI(logName, "Wrote %d bytes", txBytes);
    return txBytes;
}

void kangaroo_tx_task(void *arg)
{
    QueueHandle_t cmd_queue;

    cmd_queue = (QueueHandle_t)arg;

    queue_msg_t msg;
    
    static const char *TX_TASK_TAG = "KANGAROO_TX_TASK";
    esp_log_level_set(TX_TASK_TAG, ESP_LOG_INFO);
    while (1)
    {
        if (xQueueReceive(cmd_queue, &(msg), 0)){
            ESP_LOGI(TX_TASK_TAG, "Got kangaroo cmd");
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void kangaroo_rx_task(void *arg)
{
    QueueHandle_t resp_queue;

    resp_queue = (QueueHandle_t)arg;

    static const char *RX_TASK_TAG = "KANGAROO_RX_TASK";
    esp_log_level_set(RX_TASK_TAG, ESP_LOG_INFO);
    uint8_t *data = (uint8_t *)malloc(RX_BUF_SIZE + 1);
    while (1)
    {

        const int rxBytes = uart_read_bytes(UART_PORT, data, RX_BUF_SIZE, pdMS_TO_TICKS(1000));

        if (rxBytes > 0)
        {
            data[rxBytes] = 0;
            ESP_LOGI(RX_TASK_TAG, "Read %d bytes: '%s'", rxBytes, data);
            ESP_LOG_BUFFER_HEXDUMP(RX_TASK_TAG, data, rxBytes, ESP_LOG_INFO);

            queue_msg_t msg = {1, "test\0"};

            xQueueSend(resp_queue, &msg, 0);
        }
    }
    free(data);
}