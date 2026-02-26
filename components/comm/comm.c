/**
 * @file comm.c
 * @brief Implementación del componente de comunicación ESP-Now para el Gateway
 * 
 * NOTA: El callback ESP-Now se ejecuta en contexto ISR. Por seguridad:
 * - Solo se copian los datos raw en el ISR
 * - El parsing JSON se hace en una tarea separada
 */

#include "comm.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "freertos/task.h"

static const char *TAG = "COMM";

// ============================================================================
// Variables privadas
// ============================================================================

/** @brief Lista de sensores registrados */
static sensor_info_t s_registered_sensors[10];  // Máximo 10 sensores por ahora
static uint8_t s_sensor_count = 0;

/** @brief Cola para datos raw recibidos (procesados fuera del ISR) */
static QueueHandle_t s_raw_data_queue = NULL;

/** @brief Handle de la tarea de procesamiento */
static TaskHandle_t s_comm_task_handle = NULL;

/** @brief Estructura para pasar datos raw del ISR a la tarea */
typedef struct {
    uint8_t data[ESPNOW_MAX_DATA_LEN];
    int len;
    uint8_t src_mac[6];
} raw_data_t;

// ============================================================================
// Funciones privadas
// ============================================================================

/**
 * @brief Parsea un mensaje JSON recibido y lo convierte a estructura interna
 * @note Esta función NO debe llamarse desde ISR
 */
static esp_err_t parse_json_message(const uint8_t *data, int len, controller_message_t *message)
{
    // Crear string null-terminated
    char *json_str = malloc(len + 1);
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(json_str, data, len);
    json_str[len] = '\0';
    
    // Log del JSON crudo recibido para debug
    ESP_LOGI(TAG, "JSON recibido (%d bytes): %s", len, json_str);

    cJSON *root = cJSON_Parse(json_str);
    free(json_str);
    
    if (!root) {
        ESP_LOGW(TAG, "Error parseando JSON - JSON inválido");
        return ESP_ERR_INVALID_ARG;
    }

    // Parsear header
    cJSON *header = cJSON_GetObjectItem(root, "header");
    if (header) {
        cJSON *ver = cJSON_GetObjectItem(header, "ver");
        cJSON *src_id = cJSON_GetObjectItem(header, "src_id");
        cJSON *src_type = cJSON_GetObjectItem(header, "src_type");
        
        if (ver) message->header.version = ver->valueint;
        if (src_id && cJSON_IsString(src_id)) {
            strncpy(message->header.src_id, src_id->valuestring, DEVICE_ID_MAX_LEN - 1);
            message->header.src_id[DEVICE_ID_MAX_LEN - 1] = '\0';
        }
        if (src_type && cJSON_IsString(src_type)) {
            // Mapear string a enum
            if (strcmp(src_type->valuestring, "SEC_SENSOR") == 0) {
                message->header.src_type = DEV_TYPE_SENSOR_DOOR;
            } else if (strcmp(src_type->valuestring, "PIR_SENSOR") == 0) {
                message->header.src_type = DEV_TYPE_SENSOR_PIR;
            } else if (strcmp(src_type->valuestring, "KEYPAD") == 0) {
                message->header.src_type = DEV_TYPE_KEYPAD;
            }
        }
    }

    // Parsear payload
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    if (payload) {
        cJSON *type = cJSON_GetObjectItem(payload, "type");
        cJSON *action = cJSON_GetObjectItem(payload, "action");
        cJSON *value = cJSON_GetObjectItem(payload, "value");
        cJSON *battery = cJSON_GetObjectItem(payload, "battery");
        
        if (type && cJSON_IsString(type)) {
            if (strcmp(type->valuestring, "EVENT") == 0) {
                message->payload.type = MSG_TYPE_SENSOR_EVENT;
            } else if (strcmp(type->valuestring, "ARM") == 0) {
                message->payload.type = MSG_TYPE_ARM_COMMAND;
            } else if (strcmp(type->valuestring, "DISARM") == 0) {
                message->payload.type = MSG_TYPE_DISARM_COMMAND;
            } else if (strcmp(type->valuestring, "PANIC") == 0) {
                message->payload.type = MSG_TYPE_PANIC;
            } else if (strcmp(type->valuestring, "HEARTBEAT") == 0) {
                message->payload.type = MSG_TYPE_HEARTBEAT;
            }
        }
        
        if (action && cJSON_IsString(action)) {
            if (strcmp(action->valuestring, "STATE_CHANGE") == 0) {
                // El valor está en el campo value
            } else if (strcmp(action->valuestring, "OPEN") == 0) {
                message->payload.action = SENSOR_ACTION_OPEN;
            } else if (strcmp(action->valuestring, "CLOSED") == 0) {
                message->payload.action = SENSOR_ACTION_CLOSED;
            } else if (strcmp(action->valuestring, "TAMPER") == 0) {
                message->payload.action = SENSOR_ACTION_TAMPER;
            }
        }
        
        if (value && cJSON_IsString(value)) {
            if (strcmp(value->valuestring, "OPEN") == 0) {
                message->payload.action = SENSOR_ACTION_OPEN;
            } else if (strcmp(value->valuestring, "CLOSED") == 0) {
                message->payload.action = SENSOR_ACTION_CLOSED;
            }
        }
        
        // Extraer battery si existe
        if (battery && cJSON_IsNumber(battery)) {
            message->payload.value = battery->valueint;
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief Tarea de procesamiento de mensajes ESP-Now
 * 
 * Recibe datos raw de la cola y hace el parsing JSON fuera del contexto ISR
 */
static void comm_processing_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Comm processing task iniciada");
    
    raw_data_t raw_data;
    
    while (1) {
        // Esperar datos raw de la cola
        if (xQueueReceive(s_raw_data_queue, &raw_data, portMAX_DELAY) == pdTRUE) {
            
            // Crear estructura de mensaje
            controller_message_t message;
            memset(&message, 0, sizeof(controller_message_t));
            
            // Parsear el mensaje JSON (fuera del ISR)
            if (parse_json_message(raw_data.data, raw_data.len, &message) == ESP_OK) {
                // RSSI placeholder
                message.rssi = -50;
                
                // Enviar a la cola del controlador
                if (xQueueSend(gSystemCtx.controller_queue, &message, pdMS_TO_TICKS(100)) != pdPASS) {
                    ESP_LOGW(TAG, "Cola del controlador llena");
                }
            } else {
                ESP_LOGW(TAG, "Error parseando mensaje JSON");
            }
        }
    }
}

// ============================================================================
// Callbacks ESP-Now
// ============================================================================

/**
 * @brief Callback de recepción ESP-Now
 * @note Se ejecuta en contexto ISR - SOLO copiar datos, NO hacer parsing
 */
void comm_esp_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    if (len <= 0 || len > ESPNOW_MAX_DATA_LEN) {
        return;
    }
    
    if (s_raw_data_queue == NULL) {
        return;
    }

    // Crear estructura para datos raw
    raw_data_t raw_data;
    raw_data.len = len;
    memcpy(raw_data.data, data, len);
    memcpy(raw_data.src_mac, recv_info->src_addr, 6);
    
    // Enviar a la cola de procesamiento (desde ISR)
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (xQueueSendFromISR(s_raw_data_queue, &raw_data, &xHigherPriorityTaskWoken) != pdPASS) {
        // Cola llena - descartar
    }
    
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

void comm_esp_now_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGD(TAG, "Mensaje enviado exitosamente");
    } else {
        ESP_LOGW(TAG, "Error enviando mensaje");
    }
}

// ============================================================================
// Funciones públicas
// ============================================================================

esp_err_t comm_init(void)
{
    ESP_LOGI(TAG, "Inicializando módulo de comunicación");

    // Inicializar NVS si no está inicializado
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Crear cola para datos raw (antes de inicializar WiFi)
    s_raw_data_queue = xQueueCreate(10, sizeof(raw_data_t));
    if (s_raw_data_queue == NULL) {
        ESP_LOGE(TAG, "Error al crear cola de datos raw");
        return ESP_ERR_NO_MEM;
    }

    // Inicializar WiFi en modo estación (requerido para ESP-Now)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Inicializar ESP-Now
    ESP_ERROR_CHECK(esp_now_init());
    
    // Registrar callbacks
    ESP_ERROR_CHECK(esp_now_register_recv_cb(comm_esp_now_recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(comm_esp_now_send_cb));

    // Agregar peer de broadcast
    esp_now_peer_info_t broadcast_peer = {
        .peer_addr = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
        .channel = 0,  // Usar canal actual
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    ESP_ERROR_CHECK(esp_now_add_peer(&broadcast_peer));

    // Crear tarea de procesamiento
    BaseType_t task_ret = xTaskCreate(
        comm_processing_task,
        "comm_proc",
        4096,
        NULL,
        4,
        &s_comm_task_handle
    );
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Error al crear tarea de procesamiento");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Módulo de comunicación inicializado correctamente");
    
    // Imprimir MAC del gateway
    uint8_t mac[6];
    comm_get_gateway_mac(mac);
    ESP_LOGI(TAG, "Gateway MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    return ESP_OK;
}

esp_err_t comm_deinit(void)
{
    if (s_comm_task_handle) {
        vTaskDelete(s_comm_task_handle);
        s_comm_task_handle = NULL;
    }
    
    if (s_raw_data_queue) {
        vQueueDelete(s_raw_data_queue);
        s_raw_data_queue = NULL;
    }
    
    ESP_ERROR_CHECK(esp_now_deinit());
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_deinit());
    return ESP_OK;
}

esp_err_t comm_send_message(const uint8_t *dest_mac, const controller_message_t *message)
{
    if (!message) {
        return ESP_ERR_INVALID_ARG;
    }

    // Crear JSON para enviar
    cJSON *root = cJSON_CreateObject();
    
    // Header
    cJSON *header = cJSON_CreateObject();
    cJSON_AddNumberToObject(header, "ver", message->header.version);
    cJSON_AddStringToObject(header, "src_id", message->header.src_id);
    cJSON_AddStringToObject(header, "src_type", "GATEWAY");
    cJSON_AddItemToObject(root, "header", header);
    
    // Payload
    cJSON *payload = cJSON_CreateObject();
    const char *type_str = "EVENT";
    switch (message->payload.type) {
        case MSG_TYPE_ARM_COMMAND: type_str = "ARM"; break;
        case MSG_TYPE_DISARM_COMMAND: type_str = "DISARM"; break;
        case MSG_TYPE_PANIC: type_str = "PANIC"; break;
        case MSG_TYPE_HEARTBEAT: type_str = "HEARTBEAT"; break;
        default: break;
    }
    cJSON_AddStringToObject(payload, "type", type_str);
    cJSON_AddItemToObject(root, "payload", payload);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    // Usar broadcast si dest_mac es NULL
    const uint8_t *target_mac = dest_mac ? dest_mac : (uint8_t[]){0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    ESP_LOGI(TAG, "Enviando mensaje: %s", json_str);
    esp_err_t ret = esp_now_send(target_mac, (uint8_t *)json_str, strlen(json_str));
    free(json_str);
    
    return ret;
}

esp_err_t comm_broadcast_message(const controller_message_t *message)
{
    return comm_send_message(NULL, message);
}

esp_err_t comm_register_sensor(const uint8_t *mac_addr, const char *device_id, device_type_t type)
{
    if (s_sensor_count >= 10) {
        return ESP_ERR_NO_MEM;
    }

    // Verificar si ya existe
    for (int i = 0; i < s_sensor_count; i++) {
        if (strcmp(s_registered_sensors[i].device_id, device_id) == 0) {
            // Actualizar información
            s_registered_sensors[i].is_registered = 1;
            s_registered_sensors[i].last_seen = xTaskGetTickCount() * portTICK_PERIOD_MS;
            return ESP_OK;
        }
    }

    // Agregar nuevo sensor
    sensor_info_t *sensor = &s_registered_sensors[s_sensor_count];
    strncpy(sensor->device_id, device_id, DEVICE_ID_MAX_LEN - 1);
    sensor->device_id[DEVICE_ID_MAX_LEN - 1] = '\0';
    sensor->type = type;
    sensor->state = 0;
    sensor->is_registered = 1;
    sensor->last_seen = xTaskGetTickCount() * portTICK_PERIOD_MS;
    sensor->last_rssi = 0;
    
    s_sensor_count++;

    // Agregar como peer ESP-Now
    esp_now_peer_info_t peer = {
        .channel = 0,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, mac_addr, 6);
    esp_now_add_peer(&peer);

    ESP_LOGI(TAG, "Sensor registrado: %s", device_id);
    return ESP_OK;
}

esp_err_t comm_unregister_sensor(const char *device_id)
{
    for (int i = 0; i < s_sensor_count; i++) {
        if (strcmp(s_registered_sensors[i].device_id, device_id) == 0) {
            s_registered_sensors[i].is_registered = 0;
            ESP_LOGI(TAG, "Sensor desregistrado: %s", device_id);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t comm_get_sensor_info(const char *device_id, sensor_info_t *sensor_info)
{
    for (int i = 0; i < s_sensor_count; i++) {
        if (strcmp(s_registered_sensors[i].device_id, device_id) == 0) {
            if (sensor_info) {
                *sensor_info = s_registered_sensors[i];
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

void comm_get_gateway_mac(uint8_t *mac_addr)
{
    esp_wifi_get_mac(WIFI_IF_STA, mac_addr);
}

void comm_print_registered_sensors(void)
{
    ESP_LOGI(TAG, "Sensores registrados: %d", s_sensor_count);
    for (int i = 0; i < s_sensor_count; i++) {
        if (s_registered_sensors[i].is_registered) {
            ESP_LOGI(TAG, "  - %s (tipo: %d, estado: %d, RSSI: %d)",
                     s_registered_sensors[i].device_id,
                     s_registered_sensors[i].type,
                     s_registered_sensors[i].state,
                     s_registered_sensors[i].last_rssi);
        }
    }
}
