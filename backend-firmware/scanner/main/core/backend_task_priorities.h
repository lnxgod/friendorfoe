#pragma once

/* Backend scanner FreeRTOS priorities, stack sizes, and core affinity. */
#define WIFI_SCAN_TASK_PRIORITY     5
#define BLE_SCAN_TASK_PRIORITY      4
#define FUSION_TASK_PRIORITY        3
#define UART_TX_TASK_PRIORITY       2
#define UART_CMD_TASK_PRIORITY      6

#define WIFI_SCAN_TASK_STACK_SIZE   8192
#define BLE_SCAN_TASK_STACK_SIZE    6144
#define FUSION_TASK_STACK_SIZE      4096
#define UART_TX_TASK_STACK_SIZE     8192
#define UART_CMD_TASK_STACK_SIZE    8192

#define CORE_RADIO                  0
#define CORE_PROCESSING             1
#define WIFI_SCAN_TASK_CORE         CORE_RADIO
#define BLE_SCAN_TASK_CORE          CORE_RADIO
#define FUSION_TASK_CORE            CORE_PROCESSING
#define UART_TX_TASK_CORE           CORE_PROCESSING
#define UART_CMD_TASK_CORE          CORE_PROCESSING
