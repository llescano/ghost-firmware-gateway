/**
 * @file sntp_sync.h
 * @brief Componente para sincronización de tiempo vía SNTP
 *
 * Este componente gestiona la sincronización de hora del sistema mediante
 * el protocolo SNTP (Simple Network Time Protocol).
 *
 * Características:
 * - Tarea independiente de FreeRTOS
 * - Sincronización periódica automática
 * - Zona horaria configurable (UTC-3 por defecto)
 * - Estado de sincronización consultable
 */

#ifndef SNTP_SYNC_H
#define SNTP_SYNC_H

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

// ============================================================================
// Configuración
// ============================================================================

#define SNTP_SYNC_TZ_DEFAULT      "UTC-3"      // Zona horaria Argentina
#define SNTP_SYNC_SERVER_1        "south-america.pool.ntp.org"
#define SNTP_SYNC_SERVER_2        "pool.ntp.org"
#define SNTP_SYNC_TASK_STACK      3072         // Stack de la tarea (bytes)
#define SNTP_SYNC_TASK_PRIO       5            // Prioridad (media-baja)
#define SNTP_SYNC_RETRY_INTERVAL  30           // Reintento cada 30s (sin sync)
#define SNTP_SYNC_SYNC_INTERVAL   3600         // Resync cada 1 hora

// ============================================================================
// API Pública
// ============================================================================

/**
 * @brief Inicializa el componente SNTP
 *
 * Crea la tarea de sincronización y configura los servidores NTP.
 * Debe llamarse después de tener conexión WiFi.
 *
 * @return ESP_OK si exitoso
 */
esp_err_t sntp_sync_init(void);

/**
 * @brief Verifica si el tiempo está sincronizado
 *
 * @return true si el sistema tiene hora válida de NTP
 */
bool sntp_sync_is_synced(void);

/**
 * @brief Obtiene la hora actual formateada
 *
 * @param buf Buffer para almacenar la string
 * @param size Tamaño del buffer
 * @return ESP_OK si exitoso, ESP_ERR_INVALID_STATE si no está sincronizado
 */
esp_err_t sntp_sync_get_time_str(char *buf, size_t size);

/**
 * @brief Obtiene la hora actual como time_t
 *
 * @param out_time Puntero donde almacenar la hora
 * @return ESP_OK si exitoso, ESP_ERR_INVALID_STATE si no está sincronizado
 */
esp_err_t sntp_sync_get_time(time_t *out_time);

/**
 * @brief Fuerza una re-sincronización inmediata
 *
 * @return ESP_OK si se inició la solicitud
 */
esp_err_t sntp_sync_force_sync(void);

#endif // SNTP_SYNC_H
