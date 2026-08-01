#pragma once

/*
 * Keep release stack allocations byte-for-byte stable. The DEF CON 34 game
 * canary exercises deeper USB update and scanner proof paths, so only that
 * explicitly selected build receives additional task-stack headroom.
 */
#if defined(FOF_BADGE_VARIANT) && defined(FOF_DC34_GAME_CANARY)
#define BADGE_USB_TASK_STACK_BYTES 20480
#define BADGE_UART_RX_TASK_STACK_BYTES 9216
#else
#define BADGE_USB_TASK_STACK_BYTES 16384
#define BADGE_UART_RX_TASK_STACK_BYTES 8192
#endif
