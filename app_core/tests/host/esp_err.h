/** @file Minimal ESP-IDF error compatibility types for host tests. */
#ifndef __HOST_ESP_ERR_H__
#define __HOST_ESP_ERR_H__

/** @brief Host representation of an ESP-IDF error code. */
typedef int esp_err_t;

#define ESP_OK               0
#define ESP_FAIL            -1
#define ESP_ERR_NO_MEM      -2
#define ESP_ERR_INVALID_ARG -3
#define ESP_ERR_NOT_SUPPORTED -4
#define ESP_ERR_INVALID_STATE -5
#define ESP_ERR_NOT_FOUND     -6
#define ESP_ERR_TIMEOUT       -7
#define ESP_ERR_INVALID_SIZE  -8

/** @brief Minimal host stand-in for the IDF error-name helper. */
static inline const char *esp_err_to_name(esp_err_t error)
{
    switch (error)
    {
    case ESP_OK:
        return "ESP_OK";
    case ESP_FAIL:
        return "ESP_FAIL";
    case ESP_ERR_NO_MEM:
        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:
        return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:
        return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NOT_FOUND:
        return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_TIMEOUT:
        return "ESP_ERR_TIMEOUT";
    default:
        return "ESP_ERR";
    }
}

#endif /* __HOST_ESP_ERR_H__ */
