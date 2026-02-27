 #ifndef SUPABASE_CLIENT_H
#define SUPABASE_CLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// === CONFIGURACIÓN ===
#define SUPABASE_HOST "ekwdgsgjtmhlvaiwfhuo.supabase.co"
#define SUPABASE_TIMEOUT_MS 15000

// === DEVICE KEY ===
// Key única para autenticación del dispositivo (se puede rotar fácilmente)
#define DEVICE_KEY "ghost-gateway-001"

// === ESTRUCTURAS ===

// Contexto del cliente Supabase
typedef struct {
    bool initialized;         /**< Cliente inicializado */
    char host[128];           /**< Host de Supabase */
} supabase_context_t;

// Estructura de evento de dispositivo (compatible con edge function ghost-event-public)
typedef struct {
    char *event_type;        /**< Tipo de evento (ej. "presence_start", "alarm", etc.) */
    char *event_timestamp;   /**< Timestamp del evento (ISO 8601) - opcional (se genera si es NULL) */
    char *device_id;         /**< ID del dispositivo - opcional */
    char *device_type;       /**< Tipo de dispositivo (ej. "SEC_SENSOR") - opcional */
    bool presence;            /**< Presencia detectada - opcional */
    float distance_cm;        /**< Distancia en cm - opcional */
    int direction;            /**< Dirección (0-3) - opcional */
    int behavior;            /**< Comportamiento - opcional */
    int active_zone;         /**< Zona activa - opcional */
    char *energy_data;        /**< Datos de energía (JSON string) - opcional */
} device_event_t;

// === FUNCIONES PÚBLICAS ===

/**
 * @brief Inicializar el cliente Supabase
 * @return ESP_OK si éxito, error code si falla
 */
esp_err_t supabase_client_init(void);

/**
 * @brief Crear JSON string a partir de estructura device_event_t
 * @param event Puntero a la estructura del evento
 * @return String JSON (debe ser liberada por el llamador) o NULL si error
 */
char *create_event_json(const device_event_t *event);

/**
 * @brief Enviar evento a Supabase via Edge Function
 * @param event Puntero a la estructura del evento
 * @return ESP_OK si éxito, error code si falla
 */
esp_err_t supabase_send_event(const device_event_t *event);

/**
 * @brief Verificar si el cliente está inicializado
 * @return true si inicializado, false si no
 */
bool supabase_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif // SUPABASE_CLIENT_H
