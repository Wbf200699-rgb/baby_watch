#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARRAY_SIZE(x)              (sizeof(x) / sizeof((x)[0]))
#define UNUSED(x)                  ((void) (x))

#define APP_VERSION_MAJOR          (0U)
#define APP_VERSION_MINOR          (1U)
#define APP_VERSION_PATCH          (0U)

typedef uint8_t core_id_t;

enum
{
    CORE_ID_M33 = 0U,
    CORE_ID_M85 = 1U,
    CORE_ID_ESP32 = 2U,
};

#ifdef __cplusplus
}
#endif

#endif /* COMMON_TYPES_H */
