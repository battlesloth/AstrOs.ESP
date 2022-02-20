#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_system.h"

esp_err_t init_kangaroo_interface(int baud_rate, int rx_pin, int tx_pin);

void kangaroo_tx_task(void *arg);

void kangaroo_rx_task(void *arg);

#ifdef __cplusplus
}
#endif