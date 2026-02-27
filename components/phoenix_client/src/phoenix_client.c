/**
 * @file phoenix_client.c
 * @brief Implementación del cliente Phoenix Channels para Supabase Realtime
 *        usando esp_http_client (WebSocket)
 */

#include "phoenix_client.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "PHOENIX";

// ============================================================================
// Constantes
// ============================================================================

#define PHOENIX_VERSION            "2.0.0"
#define PHOENIX_HEARTBEAT_INTERVAL  30000  // 30 segundos
#define PHOENIX_RECONNECT_DELAY     5000   // 5 segundos
#define PHOENIX_MAX_REFS           1000000  // Counter para ref

#define HTTP_BUFFER_SIZE           8192
#define HTTP_TIMEOUT_MS            5000

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
    esp_http_client_handle_t http_client;
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
 * @brief Handler de eventos HTTP (WebSocket)
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA: %d bytes", evt->data_len);
            if (evt->data_len > 0) {
                // Asegurar null-termination para logs
                char *data = malloc(evt->data_len + 1);
                memcpy(data, evt->data, evt->data_len);
                data[evt->data_len] = '\0';
                ESP_LOGD(TAG, "Data recibida: %s", data);
                free(data);
            }
            break;

        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            break;

        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "HTTP_EVENT_DISCONNECTED");
            s_ctx.connected = false;
            break;

        default:
            break;
    }
    return ESP_OK;
}

/**
 * @brief Callback del timer de heartbeat
 */
static void heartbeat_timer_callback(void* arg)
{
    if (s_ctx.connected && s_ctx.http_client) {
        // TODO: Enviar heartbeat por WebSocket
        ESP_LOGD(TAG, "Heartbeat");
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

    // Configurar cliente HTTP con WebSocket
    esp_http_client_config_t config = {
        .url = ws_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = HTTP_BUFFER_SIZE,
        .user_agent = "ESP32-Ghost-Gateway/1.0",
        .event_handler = http_event_handler,
    };

    s_ctx.http_client = esp_http_client_init(&config);

    // Realizar la "conexión" WebSocket (upgrade request)
    esp_err_t err = esp_http_client_perform(s_ctx.http_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error en conexión WebSocket: %s", esp_err_to_name(err));
        esp_http_client_cleanup(s_ctx.http_client);
        s_ctx.http_client = NULL;
        return err;
    }

    s_ctx.connected = true;
    ESP_LOGI(TAG, "✅ WebSocket conectado");

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

    if (s_ctx.http_client) {
        esp_http_client_cleanup(s_ctx.http_client);
        s_ctx.http_client = NULL;
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

    ESP_LOGI(TAG, "Suscripción agregada a %s", topic);
    return ESP_OK;
}

esp_err_t phoenix_send(const char *topic, const char *event, const char *payload)
{
    // TODO: Implementar envío de mensajes por WebSocket
    ESP_LOGW(TAG, "phoenix_send no implementado aún");
    return ESP_ERR_NOT_SUPPORTED;
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
