/**
 * @file controller.c
 * @brief Implementación del controlador principal del Gateway
 */

#include "controller.h"
#include "ui.h"
#include "supabase_client.h"
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "CTRL";

// ============================================================================
// Funciones helper
// ============================================================================

/**
 * @brief Obtiene el nombre del estado como string
 */
static const char* get_state_name(system_state_t state)
{
    switch (state) {
        case SYS_STATE_DISARMED: return "DESARMADO";
        case SYS_STATE_ARMED: return "ARMADO";
        case SYS_STATE_ALARM: return "ALARMA";
        case SYS_STATE_TAMPER: return "TAMPER";
        default: return "DESCONOCIDO";
    }
}

/**
 * @brief Envía un evento de cambio de estado a Supabase
 */
static void send_state_change_event(system_state_t new_state, system_state_t old_state)
{
    if (!supabase_is_initialized()) {
        ESP_LOGD(TAG, "Supabase no inicializado, no se enviará evento de estado");
        return;
    }

    // Crear JSON con información del cambio de estado
    cJSON *state_info = cJSON_CreateObject();
    cJSON_AddStringToObject(state_info, "old_state", get_state_name(old_state));
    cJSON_AddStringToObject(state_info, "new_state", get_state_name(new_state));
    cJSON_AddNumberToObject(state_info, "old_state_code", (int)old_state);
    cJSON_AddNumberToObject(state_info, "new_state_code", (int)new_state);

    char *energy_data = cJSON_Print(state_info);

    // Crear evento
    device_event_t event = {
        .event_type = "state_change",
        .event_timestamp = NULL,  // Se generará automáticamente
        .device_id = "GATEWAY_001",
        .device_type = "GATEWAY",
        .presence = false,
        .distance_cm = 0.0f,
        .direction = -1,
        .behavior = -1,
        .active_zone = -1,
        .energy_data = energy_data
    };

    esp_err_t ret = supabase_send_event(&event);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ Evento de estado enviado: %s -> %s",
                 get_state_name(old_state), get_state_name(new_state));
    } else {
        ESP_LOGW(TAG, "⚠️ Error enviando evento de estado: %s", esp_err_to_name(ret));
    }

    free(energy_data);
    cJSON_Delete(state_info);
}

// ==============================================================================
// Variables globales (definición)
// ==============================================================================

/** @brief Contexto global del sistema - definición */
system_context_t gSystemCtx;

/** @brief Handle de la tarea del controlador */
static TaskHandle_t s_controller_task_handle = NULL;

// ==============================================================================
// Funciones privadas
// ==============================================================================

/**
 * @brief Carga el estado guardado desde NVS
 */
static esp_err_t load_state_from_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_SYSTEM, NVS_READONLY, &nvs_handle);
    
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // No hay datos guardados, usar valores por defecto
        gSystemCtx.boot_mode = BOOT_MODE_LAST_STATE;
        gSystemCtx.current_state = SYS_STATE_DISARMED;
        gSystemCtx.sensor_count = 0;
        return ESP_OK;
    }
    
    if (err != ESP_OK) {
        return err;
    }
    
    // Cargar modo de boot
    uint8_t boot_mode = 0;
    nvs_get_u8(nvs_handle, NVS_KEY_BOOT_MODE, &boot_mode);
    gSystemCtx.boot_mode = (boot_mode_t)boot_mode;
    
    // Cargar último estado
    uint8_t last_state = 0;
    nvs_get_u8(nvs_handle, NVS_KEY_LAST_STATE, &last_state);
    gSystemCtx.current_state = (system_state_t)last_state;
    
    nvs_close(nvs_handle);
    return ESP_OK;
}

/**
 * @brief Guarda el estado actual en NVS
 */
static esp_err_t save_state_to_nvs(void){
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_SYSTEM, NVS_READWRITE, &nvs_handle);
    
    if (err != ESP_OK) {
        return err;
    }
    
    nvs_set_u8(nvs_handle, NVS_KEY_BOOT_MODE, (uint8_t)gSystemCtx.boot_mode);
    nvs_set_u8(nvs_handle, NVS_KEY_LAST_STATE, (uint8_t)gSystemCtx.current_state);
    
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return ESP_OK;
}

/**
 * @brief Determina el estado inicial del sistema según el modo de boot
 */
static void determine_initial_state(void)
{
    switch (gSystemCtx.boot_mode) {
        case BOOT_MODE_FORCE_DISARMED:
            gSystemCtx.current_state = SYS_STATE_DISARMED;
            ESP_LOGI(TAG, "Boot mode: FORCE_DISARMED");
            break;
        case BOOT_MODE_FORCE_ARMED:
            gSystemCtx.current_state = SYS_STATE_ARMED;
            ESP_LOGI(TAG, "Boot mode: FORCE_ARMED");
            break;
        case BOOT_MODE_LAST_STATE:
        default:
            // Ya se cargó el estado desde NVS
            ESP_LOGI(TAG, "Boot mode: LAST_STATE (restaurando %d)", gSystemCtx.current_state);
            break;
    }
}

/**
 * @brief Procesa un mensaje recibido en la cola
 */
static void process_message(controller_message_t *message)
{
    if (!message) {
        return;
    }
    
    ESP_LOGI(TAG, "Mensaje recibido de: %s (tipo: %d)", 
             message->header.src_id, message->payload.type);
    
    switch (message->payload.type) {
        case MSG_TYPE_SENSOR_EVENT:
            controller_process_sensor_event(message);
            break;
            
        case MSG_TYPE_ARM_COMMAND:
            controller_arm();
            break;
            
        case MSG_TYPE_DISARM_COMMAND:
            controller_disarm();
            break;
            
        case MSG_TYPE_PANIC:
            controller_trigger_alarm();
            break;
            
        case MSG_TYPE_HEARTBEAT:
            // Actualizar last_seen del sensor
            ESP_LOGD(TAG, "Heartbeat de %s", message->header.src_id);
            break;
    }
}

// ==============================================================================
// Funciones públicas
// ==============================================================================

esp_err_t controller_init(void)
{
    ESP_LOGI(TAG, "Inicializando controlador");
    
    // Inicializar mutex
    gSystemCtx.mutex = xSemaphoreCreateMutex();
    if (!gSystemCtx.mutex) {
        ESP_LOGE(TAG, "Error creando mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // Cargar estado desde NVS
    load_state_from_nvs();
    
    // Determinar estado inicial
    determine_initial_state();
    
    // Crear cola de mensajes y asignarla al contexto global
    // La cola almacena estructuras completas, no punteros
    gSystemCtx.controller_queue = xQueueCreate(CONTROLLER_QUEUE_SIZE, sizeof(controller_message_t));
    if (!gSystemCtx.controller_queue) {
        ESP_LOGE(TAG, "Error creando cola de mensajes");
        return ESP_ERR_NO_MEM;
    }
    
    // Crear tarea del controlador
    // Stack aumentado a 8KB para evitar overflow durante HTTP/TLS
    BaseType_t ret = xTaskCreate(
        controller_task,
        "controller",
        8192,
        NULL,
        5,
        &s_controller_task_handle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Error creando tarea del controlador");
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "Controlador inicializado - Estado: %d", gSystemCtx.current_state);
    return ESP_OK;
}

esp_err_t controller_deinit(void)
{
    if (s_controller_task_handle) {
        vTaskDelete(s_controller_task_handle);
        s_controller_task_handle = NULL;
    }
    
    if (gSystemCtx.controller_queue) {
        vQueueDelete(gSystemCtx.controller_queue);
        gSystemCtx.controller_queue = NULL;
    }
    
    if (gSystemCtx.mutex) {
        vSemaphoreDelete(gSystemCtx.mutex);
        gSystemCtx.mutex = NULL;
    }
    
    return ESP_OK;
}

system_state_t controller_get_state(void)
{
    system_state_t state;
    xSemaphoreTake(gSystemCtx.mutex, portMAX_DELAY);
    state = gSystemCtx.current_state;
    xSemaphoreGive(gSystemCtx.mutex);
    return state;
}

esp_err_t controller_set_state(system_state_t state)
{
    system_state_t old_state = controller_get_state();

    xSemaphoreTake(gSystemCtx.mutex, portMAX_DELAY);
    gSystemCtx.previous_state = gSystemCtx.current_state;
    gSystemCtx.current_state = state;
    xSemaphoreGive(gSystemCtx.mutex);

    save_state_to_nvs();
    ESP_LOGI(TAG, "Estado cambiado: %s -> %s", get_state_name(old_state), get_state_name(state));

    // Actualizar UI inmediatamente para feedback instantáneo al usuario
    ui_set_system_state(state);

    // Enviar evento a Supabase (en background, no bloquea la UI)
    send_state_change_event(state, old_state);

    return ESP_OK;
}

esp_err_t controller_arm(void)
{
    system_state_t current = controller_get_state();
    if (current == SYS_STATE_ARMED) {
        ESP_LOGW(TAG, "Sistema ya está ARMADO");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "🔒 ARMANDO sistema (de %s)", get_state_name(current));
    controller_set_state(SYS_STATE_ARMED);
    return ESP_OK;
}

esp_err_t controller_disarm(void)
{
    system_state_t current = controller_get_state();
    ESP_LOGI(TAG, "🔓 DESARMANDO sistema (de %s)", get_state_name(current));
    controller_set_state(SYS_STATE_DISARMED);
    return ESP_OK;
}

esp_err_t controller_trigger_alarm(void)
{
    system_state_t current = controller_get_state();
    ESP_LOGW(TAG, "🚨 ALARMA ACTIVADA! (desde %s)", get_state_name(current));
    controller_set_state(SYS_STATE_ALARM);
    return ESP_OK;
}

esp_err_t controller_clear_alarm(void)
{
    ESP_LOGI(TAG, "Alarma desactivada");
    controller_set_state(SYS_STATE_DISARMED);
    return ESP_OK;
}

esp_err_t controller_process_sensor_event(const controller_message_t *message)
{
    if (!message) {
        return ESP_ERR_INVALID_ARG;
    }
    
    system_state_t current_state = controller_get_state();
    
    // Actualizar estado del sensor
    uint8_t sensor_state = (message->payload.action == SENSOR_ACTION_OPEN) ? 1 : 0;
    controller_update_sensor_state(message->header.src_id, sensor_state);
    
    ESP_LOGI(TAG, "Evento de sensor %s: %s", 
             message->header.src_id,
             sensor_state ? "ABIERTO" : "CERRADO");
    
    // Si el sistema está armado y el sensor se abre, disparar alarma
    if (current_state == SYS_STATE_ARMED && sensor_state == 1) {
        ESP_LOGW(TAG, "¡Intrusión detectada por sensor %s!", message->header.src_id);
        controller_trigger_alarm();
    }
    
    // Si hay tamper, siempre disparar alarma
    if (message->payload.action == SENSOR_ACTION_TAMPER) {
        ESP_LOGW(TAG, "¡Tamper detectado en sensor %s!", message->header.src_id);
        controller_set_state(SYS_STATE_TAMPER);
    }
    
    return ESP_OK;
}

esp_err_t controller_update_sensor_state(const char *device_id, uint8_t state)
{
    // Buscar sensor en la lista
    for (int i = 0; i < gSystemCtx.sensor_count; i++) {
        if (strcmp(gSystemCtx.sensors[i].device_id, device_id) == 0) {
            gSystemCtx.sensors[i].state = state;
            gSystemCtx.sensors[i].last_seen = xTaskGetTickCount() * portTICK_PERIOD_MS;
            return ESP_OK;
        }
    }
    
    // Si no existe, agregarlo
    if (gSystemCtx.sensor_count < MAX_SENSORS) {
        strncpy(gSystemCtx.sensors[gSystemCtx.sensor_count].device_id, device_id, DEVICE_ID_MAX_LEN - 1);
        gSystemCtx.sensors[gSystemCtx.sensor_count].state = state;
        gSystemCtx.sensors[gSystemCtx.sensor_count].last_seen = xTaskGetTickCount() * portTICK_PERIOD_MS;
        gSystemCtx.sensor_count++;
        ESP_LOGI(TAG, "Nuevo sensor registrado: %s", device_id);
    }
    
    return ESP_OK;
}

boot_mode_t controller_get_boot_mode(void)
{
    return gSystemCtx.boot_mode;
}

esp_err_t controller_set_boot_mode(boot_mode_t mode)
{
    gSystemCtx.boot_mode = mode;
    save_state_to_nvs();
    ESP_LOGI(TAG, "Boot mode cambiado a: %d", mode);
    return ESP_OK;
}

void controller_task(void *pvParameters)
{
    controller_message_t message;  // Variable local para recibir la estructura completa
    
    ESP_LOGI(TAG, "Tarea del controlador iniciada");
    
    while (1) {
        // Esperar mensaje en la cola (bloqueante)
        // La cola pasa estructuras completas, no punteros
        if (xQueueReceive(gSystemCtx.controller_queue, &message, portMAX_DELAY) == pdPASS) {
            process_message(&message);
            // No se necesita free() porque message es una variable local del stack
        }
    }
}

void controller_print_state(void)
{
    xSemaphoreTake(gSystemCtx.mutex, portMAX_DELAY);
    
    ESP_LOGI(TAG, "=== Estado del Sistema ===");
    ESP_LOGI(TAG, "Estado: %d", gSystemCtx.current_state);
    ESP_LOGI(TAG, "Boot mode: %d", gSystemCtx.boot_mode);
    ESP_LOGI(TAG, "Sensores: %d", gSystemCtx.sensor_count);
    
    for (int i = 0; i < gSystemCtx.sensor_count; i++) {
        ESP_LOGI(TAG, "  - %s: %s", 
                 gSystemCtx.sensors[i].device_id,
                 gSystemCtx.sensors[i].state ? "ABIERTO" : "CERRADO");
    }
    
    xSemaphoreGive(gSystemCtx.mutex);
}

system_context_t* controller_get_context(void)
{
    return &gSystemCtx;
}
