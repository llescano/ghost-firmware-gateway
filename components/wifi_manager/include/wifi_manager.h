/**
 * @file wifi_manager.h
 * @brief Gestor de conectividad WiFi para Ghost Gateway
 * 
 * Componente que maneja la conexión WiFi STA del gateway,
 * incluyendo reconexión automática y almacenamiento de credenciales en NVS.
 * 
 * @author Ghost System
 * @version 1.0.0
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Estados de conexión WiFi
 */
typedef enum {
    WIFI_STATE_DISCONNECTED,    /**< WiFi desconectado */
    WIFI_STATE_CONNECTING,      /**< Conectando a AP */
    WIFI_STATE_CONNECTED,       /**< Conectado y con IP asignada */
    WIFI_STATE_ERROR            /**< Error de conexión */
} wifi_state_t;

/**
 * @brief Callback para notificaciones de estado WiFi
 * @param state Nuevo estado de conexión
 * @param ctx Contexto de usuario proporcionado en wifi_manager_set_callback
 */
typedef void (*wifi_event_callback_t)(wifi_state_t state, void *ctx);

/**
 * @brief Configuración de credenciales WiFi
 */
typedef struct {
    char ssid[32];              /**< SSID de la red (máx 32 caracteres) */
    char password[64];          /**< Password de la red (máx 64 caracteres) */
} wifi_credentials_t;

/**
 * @brief Inicializa el gestor WiFi
 * 
 * Inicializa NVS, el stack TCP/IP y configura WiFi en modo estación.
 * Debe llamarse antes de cualquier otra función del módulo.
 * 
 * @return ESP_OK si la inicialización fue exitosa
 * @return ESP_ERR_NO_MEM si no hay memoria disponible
 * @return ESP_ERR_WIFI_BASE si hay error en la inicialización WiFi
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Conecta a una red WiFi
 * 
 * Inicia la conexión a la red especificada. Las credenciales se guardan
 * automáticamente en NVS para reconexión automática.
 * 
 * @param ssid SSID de la red WiFi
 * @param password Password de la red WiFi
 * @return ESP_OK si la conexión se inició correctamente
 * @return ESP_ERR_INVALID_ARG si ssid o password son NULL
 * @return ESP_ERR_WIFI_MODE si WiFi no está en modo STA
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);

/**
 * @brief Conecta usando credenciales almacenadas en NVS
 * 
 * Intenta conectar usando las credenciales guardadas previamente.
 * Útil para reconexión automática al iniciar.
 * 
 * @return ESP_OK si se encontraron credenciales y se inició la conexión
 * @return ESP_ERR_NOT_FOUND si no hay credenciales guardadas
 */
esp_err_t wifi_manager_connect_saved(void);

/**
 * @brief Desconecta de la red WiFi actual
 * 
 * @return ESP_OK si la desconexión fue exitosa
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * @brief Obtiene el estado actual de conexión
 * 
 * @return Estado actual de la conexión WiFi
 */
wifi_state_t wifi_manager_get_state(void);

/**
 * @brief Registra un callback para notificaciones de estado
 * 
 * @param callback Función callback a invocar en cambios de estado
 * @param ctx Contexto de usuario pasado al callback
 */
void wifi_manager_set_callback(wifi_event_callback_t callback, void *ctx);

/**
 * @brief Obtiene las credenciales guardadas
 * 
 * @param[out] creds Estructura donde se copiarán las credenciales
 * @return ESP_OK si se obtuvieron las credenciales
 * @return ESP_ERR_NOT_FOUND si no hay credenciales guardadas
 */
esp_err_t wifi_manager_get_saved_credentials(wifi_credentials_t *creds);

/**
 * @brief Borra las credenciales guardadas en NVS
 * 
 * @return ESP_OK si se borraron correctamente
 */
esp_err_t wifi_manager_clear_credentials(void);

/**
 * @brief Verifica si hay credenciales guardadas
 * 
 * @return true si hay credenciales guardadas
 * @return false si no hay credenciales
 */
bool wifi_manager_has_saved_credentials(void);

/**
 * @brief Obtiene la IP actual del dispositivo
 * 
 * @param[out] ip_str Buffer donde se escribirá la IP (mínimo 16 bytes)
 * @return ESP_OK si se obtuvo la IP
 * @return ESP_ERR_INVALID_STATE si no está conectado
 */
esp_err_t wifi_manager_get_ip(char *ip_str);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
