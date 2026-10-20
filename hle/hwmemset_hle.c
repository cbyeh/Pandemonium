/**
 * libdragon's sys_hw_memset* use MI repeat mode, which HLE emulators don't
 * emulate (libdragon asserts). In the HLE build they are linked with
 * --wrap to these plain CPU fills.
 */
#include <libdragon.h>
#include <string.h>

void* __wrap_sys_hw_memset(void *ptr, uint8_t value, size_t len)
{
    return memset(ptr, value, len);
}

void* __wrap_sys_hw_memset16(void *ptr, uint16_t value, size_t len)
{
    uint16_t *p = ptr;
    for (size_t i = 0; i < len / 2; i++) p[i] = value;
    return ptr;
}

void* __wrap_sys_hw_memset32(void *ptr, uint32_t value, size_t len)
{
    uint32_t *p = ptr;
    for (size_t i = 0; i < len / 4; i++) p[i] = value;
    return ptr;
}

void* __wrap_sys_hw_memset64(void *ptr, uint64_t value, size_t len)
{
    uint64_t *p = ptr;
    for (size_t i = 0; i < len / 8; i++) p[i] = value;
    return ptr;
}
