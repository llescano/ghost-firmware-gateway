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
#include "device_identity.h"
#include "sntp_sync.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>

static const char *TAG = "SUPABASE_CLIENT";

// === CONFIGURACIÓN ===
#define SUPABASE_CONNECT_TIMEOUT_MS 10000  // 10s timeout de conexión

// Mutex para proteger las conexiones TLS
static SemaphoreHandle_t s_tls_mutex = NULL;

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
    gmtime_r(&now, &timeinfo);  // Usar gmtime_r para obtener UTC real
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
#define SUPABASE_TOKEN_PATH "/functions/v1/ghost-token-create"
#define SUPABASE_RESPONSE_BUF_SIZE 1024

// === FUNCIONES PRIVADAS ===

/**
 * @brief Decodifica Transfer-Encoding: chunked
 * @param chunked_data Datos codificados con chunked encoding
 * @param decoded Buffer donde guardar el resultado decodificado
 * @param decoded_size Tamaño del buffer decoded
 * @return Longitud de datos decodificados o 0 si error
 *
 * Formato chunked:
 *   [chunk_size_hex]\r\n
 *   [chunk_data]\r\n
 *   ...
 *   0\r\n
 *   \r\n
 */
static size_t decode_chunked(const char *chunked_data, char *decoded, size_t decoded_size)
{
    const char *p = chunked_data;
    size_t decoded_len = 0;

    while (*p != '\0') {
        // Leer tamaño del chunk (hexadecimal hasta \r\n)
        char chunk_size_str[16];
        const char *hex_end = strstr(p, "\r\n");
        if (!hex_end) break;

        size_t hex_len = hex_end - p;
        if (hex_len >= sizeof(chunk_size_str) - 1) break;
        memcpy(chunk_size_str, p, hex_len);
        chunk_size_str[hex_len] = '\0';

        // Parsear hex a entero
        char *endptr;
        long chunk_size = strtol(chunk_size_str, &endptr, 16);
        if (*endptr != '\0' || chunk_size < 0) break;

        // Chunk size 0 = fin
        if (chunk_size == 0) {
            break;
        }

        // Mover al inicio del data (saltar \r\n)
        p = hex_end + 2;

        // Verificar espacio
        if (decoded_len + (size_t)chunk_size > decoded_size - 1) {
            chunk_size = decoded_size - decoded_len - 1;
        }

        // Copiar data del chunk
        memcpy(decoded + decoded_len, p, chunk_size);
        decoded_len += chunk_size;
        p += chunk_size;

        // Saltar \r\n después del data
        if (p[0] == '\r' && p[1] == '\n') {
            p += 2;
        }
    }

    decoded[decoded_len] = '\0';
    return decoded_len;
}

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
 * @brief Lee la respuesta HTTP del servidor con Content-Length
 */
static esp_err_t read_http_response(esp_tls_t *tls, int *http_status, char *body, size_t body_size)
{
    char buffer[SUPABASE_RESPONSE_BUF_SIZE];
    int total_read = 0;
    int ret;
    int content_length = -1;
    bool headers_complete = false;
    int empty_reads = 0;
    const int MAX_EMPTY_READS = 10;  // Límite de reads vacíos para evitar loop infinito

    // Fase 1: Leer hasta tener headers completos (\r\n\r\n)
    while (!headers_complete && total_read < sizeof(buffer) - 1) {
        ret = esp_tls_conn_read(tls, buffer + total_read, sizeof(buffer) - total_read - 1);

        if (ret > 0) {
            total_read += ret;
            buffer[total_read] = '\0';
            empty_reads = 0;  // Reset contador

            // Buscar fin de headers
            char *headers_end = strstr(buffer, "\r\n\r\n");
            if (headers_end != NULL) {
                headers_complete = true;

                // Parsear Content-Length si existe
                char *content_len_str = strstr(buffer, "\r\nContent-Length:");
                if (content_len_str) {
                    sscanf(content_len_str + 18, "%d", &content_length);
                    ESP_LOGD(TAG, "Content-Length: %d", content_length);
                }
            }
        } else if (ret == 0) {
            // Conexión cerrada por servidor
            ESP_LOGD(TAG, "Conexión cerrada por servidor");
            break;
        } else if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            // No hay datos disponibles todavía, continuar
            empty_reads++;
            if (empty_reads > MAX_EMPTY_READS) {
                ESP_LOGW(TAG, "Timeout esperando headers");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));  // Pequeña pausa
            continue;
        } else {
            ESP_LOGE(TAG, "Error al leer respuesta TLS: %d", ret);
            return ESP_FAIL;
        }
    }

    if (total_read == 0) {
        ESP_LOGE(TAG, "No se recibió respuesta del servidor");
        return ESP_FAIL;
    }

    if (!headers_complete) {
        ESP_LOGE(TAG, "Headers incompletos recibidos (%d bytes)", total_read);
        ESP_LOGD(TAG, "Response parcial:\n%.*s", total_read, buffer);
    }

    // Fase 2: Si tenemos Content-Length, leer el body completo
    if (headers_complete && content_length > 0) {
        char *body_start = strstr(buffer, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            int headers_len = body_start - buffer;
            int body_received = total_read - headers_len;

            if (body_received < content_length) {
                // Leer el resto del body
                int remaining = content_length - body_received;
                empty_reads = 0;

                while (remaining > 0 && total_read < sizeof(buffer) - 1) {
                    ret = esp_tls_conn_read(tls, buffer + total_read,
                                           sizeof(buffer) - total_read - 1);
                    if (ret > 0) {
                        total_read += ret;
                        remaining -= ret;
                        empty_reads = 0;
                    } else if (ret == 0) {
                        break;  // Conexión cerrada
                    } else if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
                        empty_reads++;
                        if (empty_reads > MAX_EMPTY_READS) {
                            ESP_LOGW(TAG, "Timeout esperando body");
                            break;
                        }
                        vTaskDelay(pdMS_TO_TICKS(10));
                        continue;
                    } else {
                        ESP_LOGE(TAG, "Error leyendo body: %d", ret);
                        break;
                    }
                }
            }
        }
    }

    buffer[total_read] = '\0';
    ESP_LOGI(TAG, "Respuesta recibida (%d bytes)", total_read);
    ESP_LOGD(TAG, "Response:\n%s", buffer);

    // Parsear status code HTTP
    if (sscanf(buffer, "HTTP/1.%*d %d", http_status) != 1) {
        ESP_LOGE(TAG, "No se pudo parsear status code HTTP. Buffer: %.*s", total_read, buffer);
        return ESP_FAIL;
    }

    // Buscar el body (después de \r\n\r\n)
    char *headers_end = strstr(buffer, "\r\n\r\n");
    if (headers_end && body) {
        char *body_start = headers_end + 4;
        size_t body_len = total_read - (body_start - buffer);

        // Verificar si usa Transfer-Encoding: chunked
        bool is_chunked = (strstr(buffer, "Transfer-Encoding:") != NULL &&
                          strstr(buffer, "chunked") != NULL);

        if (is_chunked) {
            ESP_LOGI(TAG, "Detectado Transfer-Encoding: chunked, decodificando...");
            body_len = decode_chunked(body_start, body, body_size);
            ESP_LOGI(TAG, "Body decodificado: %zu bytes", body_len);
        } else {
            // Copiar body directamente
            if (body_len >= body_size) body_len = body_size - 1;
            memcpy(body, body_start, body_len);
            body[body_len] = '\0';
            ESP_LOGI(TAG, "Body copiado: %zu bytes", body_len);
        }
    } else {
        body[0] = '\0';
    }

    return ESP_OK;
}

// Contexto global del cliente
static supabase_context_t s_ctx = {
    .initialized = false,
    .host = SUPABASE_HOST,
};

// === FUNCIÓN PRIVADA: Crear conexión TLS ===
/**
 * @brief Crear nueva conexión TLS para cada request
 * @return Puntero a la conexión TLS o NULL si error
 *
 * NOTA: No usamos keep-alive porque Supabase envía datos residuales
 * con ~10s de delay que causan problemas al reusar la conexión.
 */
static esp_tls_t *create_connection(void)
{
    const char *alpn_protos[] = { "http/1.1", NULL };

    esp_tls_cfg_t tls_cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .common_name = SUPABASE_HOST,
        .alpn_protos = alpn_protos,
        .timeout_ms = SUPABASE_CONNECT_TIMEOUT_MS,
    };

    esp_tls_t *tls = esp_tls_init();
    if (tls == NULL) {
        ESP_LOGE(TAG, "Error al inicializar TLS");
        return NULL;
    }

    int ret = esp_tls_conn_new_sync(s_ctx.host, strlen(s_ctx.host),
                                     SUPABASE_PORT, &tls_cfg, tls);
    if (ret != 1) {
        ESP_LOGE(TAG, "Error en conexión TLS: %d", ret);
        esp_tls_conn_destroy(tls);
        return NULL;
    }

    ESP_LOGI(TAG, "✅ Conexión TLS establecida");
    return tls;
}

// === FUNCIÓN PÚBLICA: Inicializar cliente ===
esp_err_t supabase_client_init(void)
{
    if (s_ctx.initialized) {
        ESP_LOGW(TAG, "Cliente ya inicializado");
        return ESP_OK;
    }

    // Crear mutex para proteger las conexiones TLS
    s_tls_mutex = xSemaphoreCreateMutex();
    if (s_tls_mutex == NULL) {
        ESP_LOGE(TAG, "Error al crear mutex TLS");
        return ESP_ERR_NO_MEM;
    }

    // Marcar como inicializado
    s_ctx.initialized = true;

    ESP_LOGI(TAG, "Cliente Supabase inicializado");
    ESP_LOGI(TAG, "  Host: %s:%d", s_ctx.host, SUPABASE_PORT);
    ESP_LOGI(TAG, "  Path: %s", SUPABASE_PATH);
    ESP_LOGI(TAG, "  Device Key: %s", DEVICE_KEY);
    ESP_LOGI(TAG, "  Mode: Connection close (no keep-alive)");

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

    // Tomar mutex para acceso exclusivo
    if (xSemaphoreTake(s_tls_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout esperando mutex TLS");
        free(json_str);
        return ESP_ERR_TIMEOUT;
    }

    // Crear nueva conexión TLS para cada request
    esp_tls_t *tls = create_connection();
    if (tls == NULL) {
        xSemaphoreGive(s_tls_mutex);
        free(json_str);
        return ESP_FAIL;
    }

    // Enviar petición HTTP
    esp_err_t err = send_http_request(tls, s_ctx.host, SUPABASE_PATH,
                                       DEVICE_KEY, json_str);
    free(json_str);

    if (err != ESP_OK) {
        esp_tls_conn_destroy(tls);
        xSemaphoreGive(s_tls_mutex);
        return err;
    }

    // Leer respuesta
    int http_status = 0;
    char response_body[SUPABASE_RESPONSE_BUF_SIZE] = {0};
    err = read_http_response(tls, &http_status, response_body, sizeof(response_body));

    // Cerrar conexión
    esp_tls_conn_destroy(tls);
    xSemaphoreGive(s_tls_mutex);

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

// === FUNCIÓN PÚBLICA: Obtener link_code ===
esp_err_t supabase_get_link_code(char *link_code)
{
    if (!s_ctx.initialized) {
        ESP_LOGE(TAG, "Client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (link_code == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Solicitando link_code a Supabase...");

    // Obtener device_id
    char device_id[DEVICE_ID_LEN];
    if (device_identity_get_id(device_id) != ESP_OK) {
        ESP_LOGE(TAG, "Error obteniendo device_id");
        return ESP_FAIL;
    }

    // Crear JSON body con device_id
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(json, "device_id", device_id);

    char *json_str = cJSON_Print(json);
    cJSON_Delete(json);

    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON string");
        return ESP_ERR_NO_MEM;
    }

    // Tomar mutex para acceso exclusivo
    if (xSemaphoreTake(s_tls_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout esperando mutex TLS");
        free(json_str);
        return ESP_ERR_TIMEOUT;
    }

    // Crear nueva conexión TLS
    esp_tls_t *tls = create_connection();
    if (tls == NULL) {
        xSemaphoreGive(s_tls_mutex);
        free(json_str);
        return ESP_FAIL;
    }

    // Enviar petición HTTP a ghost-token-create
    esp_err_t err = send_http_request(tls, s_ctx.host, SUPABASE_TOKEN_PATH,
                                       DEVICE_KEY, json_str);
    free(json_str);

    if (err != ESP_OK) {
        esp_tls_conn_destroy(tls);
        xSemaphoreGive(s_tls_mutex);
        return err;
    }

    // Leer respuesta
    int http_status = 0;
    char response_body[SUPABASE_RESPONSE_BUF_SIZE] = {0};
    err = read_http_response(tls, &http_status, response_body, sizeof(response_body));

    // Cerrar conexión
    esp_tls_conn_destroy(tls);
    xSemaphoreGive(s_tls_mutex);

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "HTTP Status: %d", http_status);

    // Parsear JSON para extraer link_code
    if (http_status >= 200 && http_status < 300) {
        cJSON *response = cJSON_Parse(response_body);
        if (response == NULL) {
            ESP_LOGE(TAG, "Error parseando respuesta JSON: %s", response_body);
            return ESP_FAIL;
        }

        cJSON *code = cJSON_GetObjectItem(response, "link_code");
        if (code == NULL || !cJSON_IsString(code)) {
            ESP_LOGE(TAG, "No se encontró link_code en respuesta: %s", response_body);
            cJSON_Delete(response);
            return ESP_FAIL;
        }

        strncpy(link_code, code->valuestring, 7);
        link_code[7] = '\0';
        cJSON_Delete(response);

        ESP_LOGI(TAG, "✅ Link code obtenido: %s", link_code);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "⚠️ Error del servidor: HTTP %d", http_status);
        return ESP_FAIL;
    }
}
