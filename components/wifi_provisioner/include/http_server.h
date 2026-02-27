/**
 * @file http_server.h
 * @brief Servidor HTTP para portal de configuración
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicia el servidor HTTP
 *
 * @return ESP_OK si se inició correctamente
 */
esp_err_t http_server_start(void);

/**
 * @brief Detiene el servidor HTTP
 *
 * @return ESP_OK si se detuvo correctamente
 */
esp_err_t http_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif // HTTP_SERVER_H
