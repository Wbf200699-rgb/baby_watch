#ifndef APP_ERROR_H
#define APP_ERROR_H

#include "common_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum e_app_err
{
    APP_OK = 0,
    APP_ERR_GENERIC = -1,
    APP_ERR_INVALID_ARG = -2,
    APP_ERR_NULL_PTR = -3,
    APP_ERR_TIMEOUT = -4,
    APP_ERR_BUSY = -5,
    APP_ERR_NO_MEMORY = -6,
    APP_ERR_OVERFLOW = -7,
    APP_ERR_UNDERFLOW = -8,
    APP_ERR_CRC = -9,
    APP_ERR_FORMAT = -10,
    APP_ERR_IO = -11,
    APP_ERR_NOT_READY = -12,
    APP_ERR_NOT_FOUND = -13,
    APP_ERR_UNSUPPORTED = -14
} app_err_t;

#ifdef __cplusplus
}
#endif

#endif /* APP_ERROR_H */
