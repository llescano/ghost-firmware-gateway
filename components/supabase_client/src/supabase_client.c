/**
 * @file supabase_client.c
 * @brief Implementación del cliente HTTP para Supabase Edge Functions
 * 
 * Este módulo envía eventos al backend de Supabase mediante HTTP POST
 * a la Edge Function ghost-event-public.
 * 
 * Usa esp_tls directamente para control total sobre ALPN, SNI y
 * Certificate Bundle, requeridos por Cloudflare/Supabase.
 */

#include "supabase_client.h"
#include "sntp_sync.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "esp_netif.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>

static const char *TAG = "SUPABASE_CLIENT";

// === FUNCIÓN PRIVADA: Generar timestamp ISO 8601 ===
/**
 * @brief Generar timestamp en formato ISO 8601 usando SNTP
 * @return String con el timestamp (debe ser liberada por el llamador)
 */
static char *generate_timestamp(void)
{
    char *timestamp = malloc(32);
    if (timestamp == NULL) {
        ESP_LOGE(TAG, "Failed to allocate timestamp buffer");
        return NULL;
    }

    // Usar el componente sntp_sync si está sincronizado
    if (sntp_sync_is_synced()) {
        if (sntp_sync_get_time_str(timestamp, 32) == ESP_OK) {
            return timestamp;
        }
    }

    // Fallback: usar tiempo del sistema (puede no estar sincronizado)
    time_t now = time(NULL);
    struct tm timeinfo = {0};
    localtime_r(&now, &timeinfo);
    strftime(timestamp, 32, "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

    ESP_LOGW(TAG, "SNTP no sincronizado, usando tiempo del sistema");
    return timestamp;
}

// === FUNCIÓN PÚBLICA: Crear JSON del evento ===
char *create_event_json(const device_event_t *event)
{
    if (event == NULL || event->event_type == NULL) {
        ESP_LOGE(TAG, "Invalid event parameters");
        return NULL;
    }
    
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return NULL;
    }
    
    // Campo obligatorio: event_type (fuera del payload)
    cJSON_AddStringToObject(json, "event_type", event->event_type);
    
    // Crear objeto payload con todos los campos del evento
    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) {
        ESP_LOGE(TAG, "Failed to create payload object");
        cJSON_Delete(json);
        return NULL;
    }
    
    // Campo obligatorio: event_timestamp (dentro del payload)
    // Nota: La edge function ghost-event-public espera este campo dentro del payload
    if (event->event_timestamp != NULL) {
        cJSON_AddStringToObject(payload, "event_timestamp", event->event_timestamp);
    } else {
        char *timestamp = generate_timestamp();
        if (timestamp != NULL) {
            cJSON_AddStringToObject(payload, "event_timestamp", timestamp);
            free(timestamp);
        }
    }
    
    // Campos opcionales dentro del payload
    // device_id y device_type son opcionales
    if (event->device_id != NULL) {
        cJSON_AddStringToObject(payload, "device_id", event->device_id);
    }
    
    if (event->device_type != NULL) {
        cJSON_AddStringToObject(payload, "device_type", event->device_type);
    }
    
    // presence (bool) - agregar solo si es true
    if (event->presence) {
        cJSON_AddBoolToObject(payload, "presence", event->presence);
    }
    
    // distance_cm (float) - agregar solo si tiene valor positivo
    if (event->distance_cm > 0) {
        cJSON_AddNumberToObject(payload, "distance_cm", event->distance_cm);
    }
    
    // direction (int) - agregar solo si es válido
    if (event->direction >= 0) {
        cJSON_AddNumberToObject(payload, "direction", event->direction);
    }
    
    // behavior (int) - agregar solo si es válido
    if (event->behavior >= 0) {
        cJSON_AddNumberToObject(payload, "behavior", event->behavior);
    }
    
    // active_zone (int) - agregar solo si es válido
    if (event->active_zone >= 0) {
        cJSON_AddNumberToObject(payload, "active_zone", event->active_zone);
    }
    
    // energy_data (string JSON) - agregar solo si se proporciona
    if (event->energy_data != NULL) {
        // Intentar parsear como JSON para validar
        cJSON *energy_json = cJSON_Parse(event->energy_data);
        if (energy_json != NULL) {
            cJSON_AddItemToObject(payload, "energy_data", energy_json);
            // Nota: No hacemos Delete aquí porque cJSON_AddItemToObject toma ownership
        } else {
            ESP_LOGW(TAG, "Invalid energy_data JSON string, skipping");
        }
    }
    
    // Agregar payload al objeto principal
    cJSON_AddItemToObject(json, "payload", payload);
    
    // Convertir a string
    char *json_str = cJSON_Print(json);
    cJSON_Delete(json);
    
    return json_str;
}

// === CONFIGURACIÓN TLS ===
#define SUPABASE_PORT 443
#define SUPABASE_PATH "/functions/v1/ghost-event-public"
#define SUPABASE_RESPONSE_BUF_SIZE 1024

// === FUNCIONES PRIVADAS ===

/**
 * @brief Construye y envía petición HTTP sobre conexión TLS
 */
static esp_err_t send_http_request(esp_tls_t *tls, const char *host, const char *path,
                                   const char *device_key, const char *json_body)
{
    // Construir petición HTTP
    char request[1024];
    int request_len = snprintf(request, sizeof(request),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "X-Device-Key: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host, device_key, (int)strlen(json_body), json_body);
    
    if (request_len >= sizeof(request)) {
        ESP_LOGE(TAG, "Petición HTTP demasiado grande");
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "Enviando petición HTTP (%d bytes)", request_len);
    ESP_LOGD(TAG, "Request:\n%s", request);
    
    // Enviar petición
    int written = esp_tls_conn_write(tls, request, request_len);
    if (written < 0) {
        ESP_LOGE(TAG, "Error al escribir en conexión TLS: %d", written);
        return ESP_FAIL;
    }
    
    if (written != request_len) {
        ESP_LOGW(TAG, "Escritos %d de %d bytes", written, request_len);
    }
    
    return ESP_OK;
}

/**
 * @brief Lee la respuesta HTTP del servidor
 */
static esp_err_t read_http_response(esp_tls_t *tls, int *http_status, char *body, size_t body_size)
{
    char buffer[SUPABASE_RESPONSE_BUF_SIZE];
    int total_read = 0;
    int ret;
    
    // Leer respuesta con timeout
    while ((ret = esp_tls_conn_read(tls, buffer + total_read, sizeof(buffer) - total_read - 1)) > 0) {
        total_read += ret;
        if (total_read >= sizeof(buffer) - 1) {
            break;
        }
    }
    
    // Manejar errores TLS específicos que son normales en operaciones no bloqueantes
    if (ret < 0 && ret != ESP_TLS_ERR_SSL_WANT_READ && ret != ESP_TLS_ERR_SSL_WANT_WRITE) {
        ESP_LOGE(TAG, "Error al leer respuesta TLS: %d", ret);
        return ESP_FAIL;
    }
    
    if (total_read == 0) {
        ESP_LOGE(TAG, "No se recibió respuesta del servidor");
        return ESP_FAIL;
    }
    
    buffer[total_read] = '\0';
    ESP_LOGI(TAG, "Respuesta recibida (%d bytes)", total_read);
    ESP_LOGD(TAG, "Response:\n%s", buffer);
    
    // Parsear status code HTTP
    if (sscanf(buffer, "HTTP/1.%*d %d", http_status) != 1) {
        ESP_LOGE(TAG, "No se pudo parsear status code HTTP");
        return ESP_FAIL;
    }
    
    // Buscar el body (después de \r\n\r\n)
    char *body_start = strstr(buffer, "\r\n\r\n");
    if (body_start && body) {
        body_start += 4;
        strncpy(body, body_start, body_size - 1);
        body[body_size - 1] = '\0';
    }
    
    return ESP_OK;
}

// Contexto global del cliente
static supabase_context_t s_ctx = {
    .initialized = false,
    .host = SUPABASE_HOST,
};

// === FUNCIÓN PÚBLICA: Inicializar cliente ===
esp_err_t supabase_client_init(void)
{
    if (s_ctx.initialized) {
        ESP_LOGW(TAG, "Cliente ya inicializado");
        return ESP_OK;
    }

    // Marcar como inicializado
    s_ctx.initialized = true;

    ESP_LOGI(TAG, "Cliente Supabase inicializado");
    ESP_LOGI(TAG, "  Host: %s:%d", s_ctx.host, SUPABASE_PORT);
    ESP_LOGI(TAG, "  Path: %s", SUPABASE_PATH);
    ESP_LOGI(TAG, "  Device Key: %s", DEVICE_KEY);

    return ESP_OK;
}

// === FUNCIÓN PÚBLICA: Enviar evento ===
esp_err_t supabase_send_event(const device_event_t *event)
{
    if (!s_ctx.initialized) {
        ESP_LOGE(TAG, "Client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (event == NULL || event->event_type == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Enviando evento: %s", event->event_type);

    // Verificar estado de la red
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "Estado de red:");
        ESP_LOGI(TAG, "  IP: " IPSTR, IP2STR(&ip_info.ip));
        ESP_LOGI(TAG, "  Netmask: " IPSTR, IP2STR(&ip_info.netmask));
        ESP_LOGI(TAG, "  Gateway: " IPSTR, IP2STR(&ip_info.gw));
    } else {
        ESP_LOGW(TAG, "No se pudo obtener información de la red");
    }

    // Crear JSON usando la función auxiliar
    char *json_str = create_event_json(event);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to create event JSON");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "JSON body: %s", json_str);

    // Configurar TLS con ALPN (requerido por Cloudflare)
    // ALPN protocolos: "http/1.1" para HTTP/1.1
    const char *alpn_protos[] = { "http/1.1", NULL };
    
    esp_tls_cfg_t tls_cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .common_name = SUPABASE_HOST,  // SNI - Server Name Indication
        .alpn_protos = alpn_protos,    // ALPN para Cloudflare
        .timeout_ms = SUPABASE_TIMEOUT_MS,
    };
    
    ESP_LOGI(TAG, "Conectando a %s:%d (SNI=%s, ALPN=http/1.1)...", 
             s_ctx.host, SUPABASE_PORT, tls_cfg.common_name);
    
    // Crear conexión TLS
    esp_tls_t *tls = esp_tls_init();
    if (tls == NULL) {
        ESP_LOGE(TAG, "Error al inicializar TLS");
        free(json_str);
        return ESP_ERR_NO_MEM;
    }
    
    // Conectar al servidor
    int ret = esp_tls_conn_new_sync(s_ctx.host, strlen(s_ctx.host),
                                     SUPABASE_PORT, &tls_cfg, tls);
    if (ret != 1) {
        ESP_LOGE(TAG, "Error en conexión TLS: %d", ret);
        esp_tls_conn_destroy(tls);
        free(json_str);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "✅ Conexión TLS establecida");
    
    // Enviar petición HTTP
    esp_err_t err = send_http_request(tls, s_ctx.host, SUPABASE_PATH,
                                       DEVICE_KEY, json_str);
    free(json_str);
    
    if (err != ESP_OK) {
        esp_tls_conn_destroy(tls);
        return err;
    }
    
    // Leer respuesta
    int http_status = 0;
    char response_body[SUPABASE_RESPONSE_BUF_SIZE] = {0};
    err = read_http_response(tls, &http_status, response_body, sizeof(response_body));
    esp_tls_conn_destroy(tls);
    
    if (err != ESP_OK) {
        return err;
    }
    
    ESP_LOGI(TAG, "HTTP Status: %d", http_status);
    if (strlen(response_body) > 0) {
        ESP_LOGI(TAG, "Respuesta: %s", response_body);
    }
    
    // Considerar exitoso si el status es 2xx
    if (http_status >= 200 && http_status < 300) {
        ESP_LOGI(TAG, "✅ Evento enviado correctamente");
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "⚠️ Error del servidor: HTTP %d", http_status);
        return ESP_FAIL;
    }
}

// === FUNCIÓN PÚBLICA: Verificar inicialización ===
bool supabase_is_initialized(void)
{
    return s_ctx.initialized;
}
