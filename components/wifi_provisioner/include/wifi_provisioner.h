/**
 * @file wifi_provisioner.h
 * @brief Modo de provisionamiento WiFi con SoftAP y portal cautivo
 *
 * Este componente gestiona el modo AP para configuración inicial:
 * - SoftAP con SSID: Ghost-Setup-<device_id>
 * - Servidor DNS para captive portal (redirige todo a 192.168.4.1)
 * - Servidor HTTP con páginas de configuración
 * - Scan de redes WiFi
 * - Conexión a red seleccionada
 */

#ifndef WIFI_PROVISIONER_H
#define WIFI_PROVISIONER_H

#include "esp_err.h"
#include "esp_wifi.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Estados del provisionador
 */
typedef enum {
    PROV_STATE_IDLE,           /**< Inactivo */
    PROV_STATE_STARTING,       /**< Iniciando SoftAP */
    PROV_STATE_RUNNING,        /**< SoftAP activo, portal disponible */
    PROV_STATE_CONNECTING,     /**< Conectando al WiFi del usuario */
    PROV_STATE_CONNECTED,      /**< Conectado exitosamente */
    PROV_STATE_FAILED,         /**< Falló la conexión */
    PROV_STATE_STOPPING        /**< Deteniendo */
} prov_state_t;

/**
 * @brief Información de una red WiFi escaneada
 */
typedef struct {
    char ssid[32];             /**< SSID de la red */
    uint8_t bssid[6];          /**< BSSID de la red */
    uint8_t channel;           /**< Canal (1-14) */
    int8_t rssi;               /**< Intensidad de señal en dBm */
    wifi_auth_mode_t authmode; /**< Tipo de autenticación */
} wifi_scan_result_t;

/**
 * @brief Callback para notificaciones de estado
 *
 * @param state Nuevo estado del provisionador
 * @param ctx Contexto de usuario
 */
typedef void (*prov_event_callback_t)(prov_state_t state, void *ctx);

/**
 * @brief Callback del resultado de conexión
 *
 * @param success true si la conexión fue exitosa
 * @param ip_addr IP asignada (si success=true)
 * @param ctx Contexto de usuario
 */
typedef void (*prov_connect_callback_t)(bool success, const char *ip_addr, void *ctx);

/**
 * @brief Inicializa el provisionador WiFi
 *
 * @return ESP_OK si la inicialización fue exitosa
 */
esp_err_t wifi_provisioner_init(void);

/**
 * @brief Inicia el modo de provisionamiento
 *
 * Configura el ESP32 en modo AP+STA:
 * - SoftAP en 192.168.4.1
 * - SSID: Ghost-Setup-<device_id>
 * - Servidor DNS y HTTP activos
 *
 * @param event_cb Callback para eventos de estado (opcional)
 * @param ctx Contexto para el callback (opcional)
 * @return ESP_OK si se inició correctamente
 */
esp_err_t wifi_provisioner_start(prov_event_callback_t event_cb, void *ctx);

/**
 * @brief Detiene el modo de provisionamiento
 *
 * Detiene SoftAP, DNS y HTTP, y vuelve a modo STA puro.
 *
 * @return ESP_OK si se detuvo correctamente
 */
esp_err_t wifi_provisioner_stop(void);

/**
 * @brief Obtiene el estado actual del provisionador
 *
 * @return Estado actual
 */
prov_state_t wifi_provisioner_get_state(void);

/**
 * @brief Escanea redes WiFi disponibles
 *
 * @param[out] results Array de resultados
 * @param max_results Máximo número de resultados
 * @param[out] found_results Número de redes encontradas
 * @return ESP_OK si el scan fue exitoso
 */
esp_err_t wifi_provisioner_scan(wifi_scan_result_t *results,
                                 size_t max_results,
                                 size_t *found_results);

/**
 * @brief Conecta a una red WiFi
 *
 * Intenta conectar a la red especificada. El resultado se notifica
 * vía callback o puede ser consultado con get_state().
 *
 * @param ssid SSID de la red
 * @param password Password de la red
 * @param connect_cb Callback para resultado de conexión (opcional)
 * @param ctx Contexto para el callback (opcional)
 * @return ESP_OK si se inició la conexión
 */
esp_err_t wifi_provisioner_connect(const char *ssid, const char *password,
                                    prov_connect_callback_t connect_cb, void *ctx);

/**
 * @brief Registra callback para eventos de estado
 *
 * @param callback Función callback
 * @param ctx Contexto de usuario
 */
void wifi_provisioner_set_callback(prov_event_callback_t callback, void *ctx);

/**
 * @brief Obtiene el SSID del SoftAP
 *
 * @param[out] ssid Buffer donde se escribirá el SSID (mínimo 32 bytes)
 * @return ESP_OK si se obtuvo correctamente
 */
esp_err_t wifi_provisioner_get_ap_ssid(char *ssid);

/**
 * @brief Obtiene la IP del SoftAP
 *
 * @return Siempre devuelve "192.168.4.1"
 */
const char *wifi_provisioner_get_ap_ip(void);

/**
 * @brief Verifica si el provisionador está activo
 *
 * @return true si está en modo AP
 * @return false si está inactivo
 */
bool wifi_provisioner_is_running(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_PROVISIONER_H
