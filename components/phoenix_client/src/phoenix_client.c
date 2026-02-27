/**
 * @file phoenix_client.c
 * @brief Implementación del cliente Phoenix Channels para Supabase Realtime
 */

#include "phoenix_client.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "PHOENIX";

// ============================================================================
// Constantes
// ============================================================================

#define PHOENIX_TRANSPORT          "websocket"
#define PHOENIX_VERSION            "2.0.0"
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
    cJSON_AddStringToObject(msg, "ref", ref ? cJSON_CreateNumber(ref)->valuestring : "");

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
 * @brief Extrae el join_ref del mensaje Phoenix
 */
static char* extract_join_ref(const char *message)
{
    cJSON *msg = cJSON_Parse(message);
    if (!msg) return NULL;

    cJSON *payload = cJSON_GetObjectItem(msg, "payload");
    if (payload) {
        cJSON *join_ref = cJSON_GetObjectItem(payload, "join_ref");
        if (join_ref && cJSON_IsString(join_ref)) {
            char *ref = strdup(join_ref->valuestring);
            cJSON_Delete(msg);
            return ref;
        }
    }

    cJSON_Delete(msg);
    return NULL;
}

/**
 * @brief Procesa un mensaje recibido del WebSocket
 */
static void process_message(const char *message)
{
    cJSON *msg = cJSON_Parse(message);
    if (!msg) {
        ESP_LOGW(TAG, "No se pudo parsear mensaje JSON");
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

    // Heartbeat (phx_reply)
    if (strcmp(event_str, "phx_reply") == 0) {
        cJSON *resp = cJSON_GetObjectItem(payload, "response");
        if (resp && cJSON_IsObject(resp) && cJSON_GetArraySize(resp) == 0) {
            ESP_LOGD(TAG, "Heartbeat OK");
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
 * @brief Callback del evento WebSocket
 */
static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                     int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "✅ WebSocket conectado");
            s_ctx.connected = true;

            // Enviar heartbeat inicial
            char *heartbeat = create_phoenix_message("phoenix", "heartbeat", NULL, ++s_ctx.ref_counter);
            esp_websocket_client_send_text(s_ctx.ws_client, heartbeat, strlen(heartbeat), portMAX_DELAY);
            free(heartbeat);

            // Re-suscribir a todos los canales
            phoenix_subscription_t *sub = s_ctx.subscriptions;
            while (sub) {
                sub->joined = false;
                char *join_msg = create_phoenix_message(sub->topic, "phx_join", "{}", ++s_ctx.ref_counter);
                esp_websocket_client_send_text(s_ctx.ws_client, join_msg, strlen(join_msg), portMAX_DELAY);
                free(join_msg);
                sub = sub->next;
            }
            break;

        case WEBSOCKET_EVENT_DATA:
            ESP_LOGD(TAG, "WebSocket data recibido (%d bytes)", data->data_len);
            if (data->data_len > 0 && data->data_ptr[data->data_len - 1] == 0) {
                process_message((char *)data->data_ptr);
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            break;

        case WEBSOCKET_EVENT_CLOSED:
            ESP_LOGW(TAG, "WebSocket cerrado - reconectando en %d ms", PHOENIX_RECONNECT_DELAY);
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
        char *heartbeat = create_phoenix_message("phoenix", "heartbeat", NULL, ++s_ctx.ref_counter);
        esp_websocket_client_send_text(s_ctx.ws_client, heartbeat, strlen(heartbeat), portMAX_DELAY);
        free(heartbeat);
        ESP_LOGD(TAG, "Heartbeat enviado");
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
    // Formato: wss://[project-ref].supabase.co/realtime/v1/websocket?apikey=[key]&vsn=2.0.0
    char ws_url[256];
    snprintf(ws_url, sizeof(ws_url),
             "wss://%s/realtime/v1/websocket?apikey=%s&log_level=error&vsn=%s",
             s_ctx.supabase_url,
             s_ctx.anon_key,
             PHOENIX_VERSION);

    ESP_LOGI(TAG, "Conectando a: %s", ws_url);

    // Configurar cliente WebSocket
    esp_websocket_client_config_t ws_cfg = {
        .uri = ws_url,
        .reconnect_timeout_ms = PHOENIX_RECONNECT_DELAY,
        .tx_buffer_size = WS_BUFFER_SIZE,
        .user_agent = "ESP32-Ghost-Gateway/1.0",
    };

    s_ctx.ws_client = esp_websocket_client_init(&ws_cfg);

    // Registrar eventos
    esp_websocket_register_events(s_ctx.ws_client, WEBSOCKET_EVENT_ANY,
                                    websocket_event_handler, NULL);

    // Iniciar conexión
    esp_err_t ret = esp_websocket_client_start(s_ctx.ws_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando WebSocket: %s", esp_err_to_name(ret));
        return ret;
    }

    // Crear timer de heartbeat
    const esp_timer_create_args_t timer_args = {
        .callback = &heartbeat_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "phoenix_heartbeat"
    };

    ret = esp_timer_create(&timer_args, &s_ctx.heartbeat_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error creando timer de heartbeat: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_timer_start_periodic(s_ctx.heartbeat_timer, s_ctx.heartbeat_interval_ms * 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando timer de heartbeat: %s", esp_err_to_name(ret));
        return ret;
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
        esp_websocket_client_close(s_ctx.ws_client, portMAX_DELAY);
        esp_websocket_client_destroy(s_ctx.ws_client);
        s_ctx.ws_client = NULL;
    }

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
    sub->callback = callback;
    sub->user_data = user_data;
    sub->joined = false;
    sub->next = s_ctx.subscriptions;
    s_ctx.subscriptions = sub;

    // Si ya está conectado, enviar join inmediatamente
    if (s_ctx.connected && s_ctx.ws_client) {
        char *join_msg = create_phoenix_message(topic, "phx_join", "{}", ++s_ctx.ref_counter);
        esp_websocket_client_send_text(s_ctx.ws_client, join_msg, strlen(join_msg), portMAX_DELAY);
        free(join_msg);
        ESP_LOGI(TAG, "Enviando join a %s", topic);
    }

    return ESP_OK;
}

esp_err_t phoenix_send(const char *topic, const char *event, const char *payload)
{
    if (!topic || !event) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_ctx.connected || !s_ctx.ws_client) {
        ESP_LOGW(TAG, "No conectado, no se puede enviar mensaje");
        return ESP_ERR_INVALID_STATE;
    }

    char *msg = create_phoenix_message(topic, event, payload, ++s_ctx.ref_counter);
    esp_websocket_client_send_text(s_ctx.ws_client, msg, strlen(msg), portMAX_DELAY);
    free(msg);

    return ESP_OK;
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
