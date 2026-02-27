/**
 * @file phoenix_client.h
 * @brief Cliente Phoenix Channels para Supabase Realtime
 *
 * Implementación simplificada del protocolo Phoenix Channels
 * para conectarse a Supabase Realtime desde ESP32.
 *
 * Protocolo: https://hexdocs.pm/phoenix/Phoenix.Channels.html
 */

#ifndef PHOENIX_CLIENT_H
#define PHOENIX_CLIENT_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback para eventos recibidos del canal
 *
 * @param event Nombre del evento (ej: "INSERT", "UPDATE")
 * @param payload Payload JSON del evento
 * @param user_data Datos de usuario pasados en phoenix_subscribe
 */
typedef void (*phoenix_event_callback_t)(const char *event, const char *payload, void *user_data);

/**
 * @brief Inicializa el cliente Phoenix
 *
 * @param supabase_url URL del proyecto Supabase (ej: "ekwdgsgjtmhlvaiwfhuo.supabase.co")
 * @param anon_key API key anónima de Supabase
 * @return ESP_OK si se inicializó correctamente
 */
esp_err_t phoenix_init(const char *supabase_url, const char *anon_key);

/**
 * @brief Conecta a Supabase Realtime via WebSocket
 *
 * @return ESP_OK si la conexión se inició correctamente
 */
esp_err_t phoenix_connect(void);

/**
 * @brief Desconecta de Supabase Realtime
 *
 * @return ESP_OK si se desconectó correctamente
 */
esp_err_t phoenix_disconnect(void);

/**
 * @brief Suscribe a un canal Phoenix
 *
 * @param topic Nombre del topic (ej: "realtime:system_commands")
 * @param callback Callback para eventos del canal
 * @param user_data Datos de usuario para el callback
 * @return ESP_OK si la suscripción se inició correctamente
 */
esp_err_t phoenix_subscribe(const char *topic, phoenix_event_callback_t callback, void *user_data);

/**
 * @brief Suscribe a un canal Supabase Postgres Changes
 *
 * @param schema Nombre del schema (ej: "public")
 * @param table Nombre de la tabla (ej: "system_commands")
 * @param event Tipo de evento (ej: "INSERT", "UPDATE", "*")
 * @param callback Callback para eventos del canal
 * @param user_data Datos de usuario para el callback
 * @return ESP_OK si la suscripción se inició correctamente
 */
esp_err_t phoenix_subscribe_postgres(const char *schema, const char *table, const char *event,
                                      phoenix_event_callback_t callback, void *user_data);

/**
 * @brief Envia un evento al canal
 *
 * @param topic Topic del canal
 * @param event Nombre del evento
 * @param payload Payload JSON (puede ser NULL)
 * @return ESP_OK si se envió correctamente
 */
esp_err_t phoenix_send(const char *topic, const char *event, const char *payload);

/**
 * @brief Verifica si está conectado
 *
 * @return true si está conectado
 */
bool phoenix_is_connected(void);

/**
 * @brief Configura intervalo de heartbeat (ping)
 *
 * @param interval_ms Intervalo en milisegundos (default: 30000)
 * @return ESP_OK
 */
esp_err_t phoenix_set_heartbeat_interval(uint32_t interval_ms);

#ifdef __cplusplus
}
#endif

#endif // PHOENIX_CLIENT_H
