/**
 * @file phoenix_client.c
 * @brief Implementación del cliente Phoenix Channels para Supabase Realtime
 *        usando esp_websocket_client
 */

#include "phoenix_client.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "PHOENIX";

// ============================================================================
// Constantes
// ============================================================================

#define PHOENIX_VERSION            "1.0.0"
#define PHOENIX_HEARTBEAT_INTERVAL  30000  // 30 segundos
#define PHOENIX_RECONNECT_DELAY     5000   // 5 segundos
#define PHOENIX_MAX_REFS           1000000  // Counter para ref

#define WS_BUFFER_SIZE             4096

// ============================================================================
// Estructuras
// ============================================================================

/**
 * @brief Estructura de suscripción a un canal
 */
typedef struct phoenix_subscription {
    char *topic;
    char *join_payload;  // Payload para el mensaje phx_join (NULL = empty object)
    phoenix_event_callback_t callback;
    void *user_data;
    bool joined;
    struct phoenix_subscription *next;
} phoenix_subscription_t;

/**
 * @brief Contexto global del cliente Phoenix
 */
typedef struct {
    esp_websocket_client_handle_t ws_client;
    char *supabase_url;
    char *anon_key;
    bool connected;
    uint32_t ref_counter;
    uint32_t heartbeat_interval_ms;
    esp_timer_handle_t heartbeat_timer;
    phoenix_subscription_t *subscriptions;
    bool reconnect_pending;
} phoenix_context_t;

// ============================================================================
// Variables globales
// ============================================================================

static phoenix_context_t s_ctx = {0};

// ============================================================================
// Funciones helper privadas
// ============================================================================

/**
 * @brief Crea un mensaje Phoenix JSON
 */
static char* create_phoenix_message(const char *topic, const char *event, const char *payload, uint64_t ref)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "topic", topic ? topic : "phoenix");
    cJSON_AddStringToObject(msg, "event", event ? event : "phx_reply");

    // El ref en Phoenix es un string (ej: "1", "2")
    if (ref > 0) {
        char ref_str[32];
        snprintf(ref_str, sizeof(ref_str), "%llu", ref);
        cJSON_AddStringToObject(msg, "ref", ref_str);
    } else {
        cJSON_AddNullToObject(msg, "ref");
    }

    if (payload) {
        cJSON *payload_obj = cJSON_Parse(payload);
        if (payload_obj) {
            cJSON_AddItemToObject(msg, "payload", payload_obj);
        } else {
            cJSON_AddStringToObject(msg, "payload", payload);
        }
    } else {
        cJSON_AddItemToObject(msg, "payload", cJSON_CreateObject());
    }

    char *json_str = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    return json_str;
}

/**
 * @brief Procesa un mensaje recibido del WebSocket
 */
static void process_message(const char *message)
{
    ESP_LOGD(TAG, "Mensaje recibido: %s", message);

    cJSON *msg = cJSON_Parse(message);
    if (!msg) {
        ESP_LOGW(TAG, "No se pudo parsear mensaje JSON: %s", message);
        return;
    }

    cJSON *topic = cJSON_GetObjectItem(msg, "topic");
    cJSON *event = cJSON_GetObjectItem(msg, "event");
    cJSON *payload = cJSON_GetObjectItem(msg, "payload");

    if (!topic || !event || !payload) {
        cJSON_Delete(msg);
        return;
    }

    const char *topic_str = topic->valuestring;
    const char *event_str = event->valuestring;

    ESP_LOGD(TAG, "Mensaje: topic=%s, event=%s", topic_str, event_str);

    // Respuesta de join (phx_reply)
    if (strcmp(event_str, "phx_reply") == 0) {
        cJSON *status = cJSON_GetObjectItem(payload, "status");
        if (status && strcmp(status->valuestring, "ok") == 0) {
            // Buscar suscripción y marcar como joined
            phoenix_subscription_t *sub = s_ctx.subscriptions;
            while (sub) {
                if (strcmp(sub->topic, topic_str) == 0) {
                    sub->joined = true;
                    ESP_LOGI(TAG, "✅ Suscrito a %s", topic_str);
                    break;
                }
                sub = sub->next;
            }
        }
        cJSON_Delete(msg);
        return;
    }

    // Buscar suscripción y llamar callback
    phoenix_subscription_t *sub = s_ctx.subscriptions;
    while (sub) {
        if (strcmp(sub->topic, topic_str) == 0 && sub->joined && sub->callback) {
            char *payload_str = cJSON_PrintUnformatted(payload);
            sub->callback(event_str, payload_str, sub->user_data);
            free(payload_str);
            break;
        }
        sub = sub->next;
    }

    cJSON_Delete(msg);
}

/**
 * @brief Handler de eventos WebSocket
 */
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_BEGIN:
            ESP_LOGI(TAG, "WebSocket iniciando...");
            break;

        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "✅ WebSocket conectado");
            s_ctx.connected = true;
            s_ctx.reconnect_pending = false;

            // Re-suscribir a todos los canales después de reconnect
            phoenix_subscription_t *sub = s_ctx.subscriptions;
            while (sub) {
                sub->joined = false; // Resetear estado
                char *msg = create_phoenix_message(sub->topic, "phx_join", sub->join_payload, ++s_ctx.ref_counter);
                ESP_LOGI(TAG, "Enviando JOIN: %s", msg);
                esp_websocket_client_send_text(s_ctx.ws_client, msg, strlen(msg), portMAX_DELAY);
                free(msg);
                sub = sub->next;
            }
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket desconectado");
            s_ctx.connected = false;
            s_ctx.reconnect_pending = true;
            break;

        case WEBSOCKET_EVENT_DATA:
            ESP_LOGD(TAG, "Datos recibidos: %d bytes (op=%d)", data->data_len, data->op_code);
            // Solo procesar mensajes de texto (op=1), ignorar control frames
            if (data->op_code == 1 && data->data_len > 0) {
                if (data->data_ptr[data->data_len - 1] == 0) {
                    // Ya está null-terminated
                    process_message((char *)data->data_ptr);
                } else {
                    // Crear buffer y null-terminate
                    char *buffer = malloc(data->data_len + 1);
                    memcpy(buffer, data->data_ptr, data->data_len);
                    buffer[data->data_len] = '\0';
                    process_message(buffer);
                    free(buffer);
                }
            }
            // op=8 (Close), op=9 (Ping), op=10 (Pong) se ignoran
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            break;

        case WEBSOCKET_EVENT_CLOSED:
            ESP_LOGW(TAG, "WebSocket cerrado");
            s_ctx.connected = false;
            break;

        default:
            break;
    }
}

/**
 * @brief Callback del timer de heartbeat
 */
static void heartbeat_timer_callback(void* arg)
{
    if (s_ctx.connected && s_ctx.ws_client) {
        char *msg = create_phoenix_message("phoenix", "heartbeat", NULL, 0);
        esp_websocket_client_send_text(s_ctx.ws_client, msg, strlen(msg), portMAX_DELAY);
        free(msg);
        ESP_LOGI(TAG, "💓 Heartbeat enviado");
    }
}

// ============================================================================
// Funciones públicas
// ============================================================================

esp_err_t phoenix_init(const char *supabase_url, const char *anon_key)
{
    if (!supabase_url || !anon_key) {
        return ESP_ERR_INVALID_ARG;
    }

    s_ctx.supabase_url = strdup(supabase_url);
    s_ctx.anon_key = strdup(anon_key);
    s_ctx.heartbeat_interval_ms = PHOENIX_HEARTBEAT_INTERVAL;
    s_ctx.ref_counter = 0;
    s_ctx.connected = false;
    s_ctx.subscriptions = NULL;
    s_ctx.reconnect_pending = false;

    ESP_LOGI(TAG, "Cliente Phoenix inicializado para %s", supabase_url);
    return ESP_OK;
}

esp_err_t phoenix_connect(void)
{
    if (s_ctx.connected) {
        ESP_LOGW(TAG, "Ya está conectado");
        return ESP_OK;
    }

    // Construir URL WebSocket de Supabase Realtime
    // Formato v1.0.0: wss://[project-ref].supabase.co/realtime/v1/websocket?apikey=[key]&vsn=1.0.0
    // Nota: Authorization se envía en el payload del JOIN, no en la URL
    char ws_url[512];
    int len = snprintf(ws_url, sizeof(ws_url),
             "wss://%s/realtime/v1/websocket?apikey=%s&vsn=%s",
             s_ctx.supabase_url,
             s_ctx.anon_key,
             PHOENIX_VERSION);

    if (len >= sizeof(ws_url)) {
        ESP_LOGE(TAG, "URL WebSocket muy larga, truncada!");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Conectando a: %s", ws_url);

    // Configurar cliente WebSocket
    const esp_websocket_client_config_t ws_cfg = {
        .uri = ws_url,
        .reconnect_timeout_ms = PHOENIX_RECONNECT_DELAY,
        .network_timeout_ms = 10000,           // Timeout de red 10s
        .buffer_size = 8192,                   // Buffer más grande para mensajes grandes
        .user_agent = "ESP32-Ghost-Gateway/1.0",
        .cert_pem = NULL,                      // Sin cert PEM específico
        .crt_bundle_attach = esp_crt_bundle_attach,  // Usar cert bundle del sistema
        .keep_alive_enable = true,             // Mantener conexión activa
        .keep_alive_idle = 30,                 // 30 segundos idle
        .keep_alive_interval = 5,              // Keep-alive cada 5 segundos
        .keep_alive_count = 3,                 // 3 reintentos
    };

    s_ctx.ws_client = esp_websocket_client_init(&ws_cfg);

    // Registrar handler de eventos
    esp_websocket_register_events(s_ctx.ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);

    // Iniciar conexión
    esp_err_t err = esp_websocket_client_start(s_ctx.ws_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando WebSocket: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_ctx.ws_client);
        s_ctx.ws_client = NULL;
        return err;
    }

    ESP_LOGI(TAG, "WebSocket iniciado, esperando conexión...");

    // Crear timer de heartbeat
    const esp_timer_create_args_t timer_args = {
        .callback = &heartbeat_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "phoenix_heartbeat"
    };

    err = esp_timer_create(&timer_args, &s_ctx.heartbeat_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error creando timer de heartbeat: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_timer_start_periodic(s_ctx.heartbeat_timer, s_ctx.heartbeat_interval_ms * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando timer de heartbeat: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t phoenix_disconnect(void)
{
    if (s_ctx.heartbeat_timer) {
        esp_timer_stop(s_ctx.heartbeat_timer);
        esp_timer_delete(s_ctx.heartbeat_timer);
        s_ctx.heartbeat_timer = NULL;
    }

    if (s_ctx.ws_client) {
        esp_websocket_client_stop(s_ctx.ws_client);
        esp_websocket_client_destroy(s_ctx.ws_client);
        s_ctx.ws_client = NULL;
    }

    // Limpiar suscripciones
    phoenix_subscription_t *sub = s_ctx.subscriptions;
    while (sub) {
        phoenix_subscription_t *next = sub->next;
        free(sub->topic);
        if (sub->join_payload) {
            free(sub->join_payload);
        }
        free(sub);
        sub = next;
    }
    s_ctx.subscriptions = NULL;

    s_ctx.connected = false;
    return ESP_OK;
}

esp_err_t phoenix_subscribe(const char *topic, phoenix_event_callback_t callback, void *user_data)
{
    if (!topic || !callback) {
        return ESP_ERR_INVALID_ARG;
    }

    // Crear nueva suscripción
    phoenix_subscription_t *sub = malloc(sizeof(phoenix_subscription_t));
    sub->topic = strdup(topic);
    sub->join_payload = NULL;  // Sin payload personalizado
    sub->callback = callback;
    sub->user_data = user_data;
    sub->joined = false;
    sub->next = s_ctx.subscriptions;
    s_ctx.subscriptions = sub;

    ESP_LOGI(TAG, "Suscripción agregada a %s", topic);

    // Si ya está conectado, enviar join inmediatamente
    if (s_ctx.connected && s_ctx.ws_client) {
        char *msg = create_phoenix_message(topic, "phx_join", NULL, ++s_ctx.ref_counter);
        esp_websocket_client_send_text(s_ctx.ws_client, msg, strlen(msg), portMAX_DELAY);
        free(msg);
        ESP_LOGI(TAG, "Enviando phx_join a %s", topic);
    }

    return ESP_OK;
}

esp_err_t phoenix_subscribe_postgres(const char *schema, const char *table, const char *event,
                                      phoenix_event_callback_t callback, void *user_data)
{
    if (!schema || !table || !callback) {
        return ESP_ERR_INVALID_ARG;
    }

    // Crear topic en formato: realtime:schema:table
    char topic[128];
    snprintf(topic, sizeof(topic), "realtime:%s:%s", schema, table);

    // Crear payload de configuración para Supabase Realtime
    cJSON *config = cJSON_CreateObject();
    cJSON *postgres_changes = cJSON_CreateArray();
    cJSON *change = cJSON_CreateObject();
    cJSON_AddStringToObject(change, "event", event ? event : "*");
    cJSON_AddStringToObject(change, "schema", schema);
    cJSON_AddStringToObject(change, "table", table);
    cJSON_AddItemToArray(postgres_changes, change);
    cJSON_AddItemToObject(config, "postgres_changes", postgres_changes);

    char *payload_str = cJSON_PrintUnformatted(config);

    // Crear suscripción
    phoenix_subscription_t *sub = malloc(sizeof(phoenix_subscription_t));
    sub->topic = strdup(topic);
    sub->join_payload = strdup(payload_str);  // Guardar payload para JOINs posteriores
    sub->callback = callback;
    sub->user_data = user_data;
    sub->joined = false;
    sub->next = s_ctx.subscriptions;
    s_ctx.subscriptions = sub;

    ESP_LOGI(TAG, "Suscripción postgres agregada a %s (event=%s)", topic, event ? event : "*");

    // Si ya está conectado, enviar join con payload
    if (s_ctx.connected && s_ctx.ws_client) {
        char *msg = create_phoenix_message(topic, "phx_join", payload_str, ++s_ctx.ref_counter);
        ESP_LOGI(TAG, "Enviando JOIN: %s", msg);
        esp_websocket_client_send_text(s_ctx.ws_client, msg, strlen(msg), portMAX_DELAY);
        free(msg);
    }

    free(payload_str);
    cJSON_Delete(config);
    return ESP_OK;
}

esp_err_t phoenix_send(const char *topic, const char *event, const char *payload)
{
    if (!s_ctx.connected || !s_ctx.ws_client) {
        ESP_LOGW(TAG, "No conectado, no se puede enviar mensaje");
        return ESP_ERR_INVALID_STATE;
    }

    char *msg = create_phoenix_message(topic, event, payload, ++s_ctx.ref_counter);
    esp_err_t err = esp_websocket_client_send_text(s_ctx.ws_client, msg, strlen(msg), portMAX_DELAY);
    free(msg);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error enviando mensaje: %s", esp_err_to_name(err));
    }

    return err;
}

bool phoenix_is_connected(void)
{
    return s_ctx.connected;
}

esp_err_t phoenix_set_heartbeat_interval(uint32_t interval_ms)
{
    s_ctx.heartbeat_interval_ms = interval_ms;

    if (s_ctx.heartbeat_timer) {
        esp_timer_stop(s_ctx.heartbeat_timer);
        esp_timer_start_periodic(s_ctx.heartbeat_timer, interval_ms * 1000);
    }

    return ESP_OK;
}
