/**
 * @file dns_server.h
 * @brief Servidor DNS para captive portal
 */

#ifndef DNS_SERVER_H
#define DNS_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicia el servidor DNS
 *
 * Redirige todas las peticiones a 192.168.4.1
 *
 * @return ESP_OK si se inició correctamente
 */
esp_err_t dns_server_start(void);

/**
 * @brief Detiene el servidor DNS
 *
 * @return ESP_OK si se detuvo correctamente
 */
esp_err_t dns_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif // DNS_SERVER_H
