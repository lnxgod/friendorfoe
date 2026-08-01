#pragma once

#ifndef portMUX_TYPE
#define portMUX_TYPE int
#endif

#ifndef portMUX_INITIALIZER_UNLOCKED
#define portMUX_INITIALIZER_UNLOCKED 0
#endif

#ifndef portENTER_CRITICAL
static inline void fof_test_port_enter_critical(portMUX_TYPE *lock)
{
    while (__sync_lock_test_and_set(lock, 1) != 0) {
    }
}
#define portENTER_CRITICAL(lock) fof_test_port_enter_critical(lock)
#endif

#ifndef portEXIT_CRITICAL
static inline void fof_test_port_exit_critical(portMUX_TYPE *lock)
{
    __sync_lock_release(lock);
}
#define portEXIT_CRITICAL(lock) fof_test_port_exit_critical(lock)
#endif
