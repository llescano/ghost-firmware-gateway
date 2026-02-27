/**
 * @file command_processor.c
 * @brief Implementación del procesador de comandos remotos
 */

#include "command_processor.h"
#include "supabase_client.h"
#include "controller.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

static const char *TAG = "CMD_PROC";

// Configuración
#define COMMAND_CHECK_INTERVAL_SEC  5  // Consultar cada 5 segundos
#define COMMAND_TIMEOUT_SEC         30 // Comandos de más de 30s se consideran expirados

// Handle del timer
static esp_timer_handle_t s_command_timer = NULL;

// ============================================================================
// Funciones privadas
// ============================================================================

/**
 * @brief Procesa un comando individual
 */
static esp_err_t process_command(const char *command_id, const char *command_str)
{
    esp_err_t ret = ESP_OK;
    bool executed = false;

    ESP_LOGI(TAG, "Procesando comando: %s (%s)", command_str, command_id);

    if (strcmp(command_str, "ARM") == 0) {
        // Enviar mensaje de armado al controller
        controller_message_t msg = {
            .header = {
                .version = 1,
                .src_id = "CMD_PROC",
                .src_type = DEV_TYPE_GATEWAY
            },
            .payload = {
                .type = MSG_TYPE_ARM_COMMAND
            }
        };

        if (xQueueSend(gSystemCtx.controller_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            executed = true;
            ESP_LOGI(TAG, "Comando ARM enviado al controller");
        } else {
            ESP_LOGE(TAG, "Error enviando comando ARM a la cola");
            ret = ESP_ERR_TIMEOUT;
        }

    } else if (strcmp(command_str, "DISARM") == 0) {
        // Enviar mensaje de desarmado
        controller_message_t msg = {
            .header = {
                .version = 1,
                .src_id = "CMD_PROC",
                .src_type = DEV_TYPE_GATEWAY
            },
            .payload = {
                .type = MSG_TYPE_DISARM_COMMAND
            }
        };

        if (xQueueSend(gSystemCtx.controller_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            executed = true;
            ESP_LOGI(TAG, "Comando DISARM enviado al controller");
        } else {
            ESP_LOGE(TAG, "Error enviando comando DISARM a la cola");
            ret = ESP_ERR_TIMEOUT;
        }

    } else if (strcmp(command_str, "TEST") == 0) {
        // Comando de prueba - solo log
        executed = true;
        ESP_LOGI(TAG, "Comando TEST recibido");
    } else {
        ESP_LOGW(TAG, "Comando desconocido: %s", command_str);
        ret = ESP_ERR_NOT_SUPPORTED;
    }

    // Actualizar estado del comando en Supabase
    if (supabase_is_initialized()) {
        // Crear JSON para actualizar el comando
        cJSON *update_data = cJSON_CreateObject();
        cJSON_AddStringToObject(update_data, "status", executed ? "executed" : "failed");
        cJSON_AddStringToObject(update_data, "result", executed ? "OK" : "Timeout/Error");

        char *json_str = cJSON_Print(update_data);

        // Llamar a una función de Supabase para actualizar el comando
        // Por ahora solo logueamos
        ESP_LOGD(TAG, "Actualizando comando %s: %s", command_id, json_str);

        free(json_str);
        cJSON_Delete(update_data);
    }

    return ret;
}

/**
 * @brief Consulta y procesa comandos pendientes
 */
static void check_pending_commands(void)
{
    if (!supabase_is_initialized()) {
        ESP_LOGD(TAG, "Supabase no inicializado, omitiendo consulta de comandos");
        return;
    }

    ESP_LOGD(TAG, "Consultando comandos pendientes...");

    // NOTA: Aquí se debería hacer un GET a system_commands?status=eq.pending&order=created_at.asc
    // Por ahora, implementamos un placeholder
    //
    // En producción:
    // 1. Hacer GET a /rest/v1/system_commands?status=eq.pending&order=created_at.asc
    // 2. Para cada comando devuelto, llamar a process_command()
    // 3. Actualizar el status a 'executed' o 'failed'
}

/**
 * @brief Callback del timer para verificación periódica
 */
static void timer_callback(void* arg)
{
    check_pending_commands();
}

// ============================================================================
// Funciones públicas
// ============================================================================

esp_err_t command_processor_init(void)
{
    ESP_LOGI(TAG, "Inicializando procesador de comandos remotos");

    // Crear timer periódico
    const esp_timer_create_args_t timer_args = {
        .callback = &timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "cmd_timer"
    };

    esp_err_t ret = esp_timer_create(&timer_args, &s_command_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error creando timer: %s", esp_err_to_name(ret));
        return ret;
    }

    // Iniciar timer: consulta cada 5 segundos
    ret = esp_timer_start_periodic(s_command_timer, COMMAND_CHECK_INTERVAL_SEC * 1000000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando timer: %s", esp_err_to_name(ret));
        esp_timer_delete(s_command_timer);
        return ret;
    }

    ESP_LOGI(TAG, "Procesador de comandos iniciado (chequeo cada %d segundos)",
             COMMAND_CHECK_INTERVAL_SEC);

    return ESP_OK;
}

esp_err_t command_processor_stop(void)
{
    if (s_command_timer) {
        esp_timer_stop(s_command_timer);
        esp_timer_delete(s_command_timer);
        s_command_timer = NULL;
    }
    return ESP_OK;
}

esp_err_t command_processor_check_now(void)
{
    check_pending_commands();
    return ESP_OK;
}
