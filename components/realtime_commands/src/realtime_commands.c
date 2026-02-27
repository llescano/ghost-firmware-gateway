/**
 * @file realtime_commands.c
 * @brief Implementación del procesador de comandos via Supabase Realtime
 */

#include "realtime_commands.h"
#include "phoenix_client.h"
#include "controller.h"
#include "esp_log.h"
#include "string.h"
#include "cJSON.h"

static const char *TAG = "RT_CMD";

// Credenciales de Supabase (desde menuconfig o hardcodeadas)
#define SUPABASE_PROJECT_URL    "ekwdgsgjtmhlvaiwfhuo.supabase.co"
#define SUPABASE_ANON_KEY       "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImVrd2Rnc2dqdG1obHZhaXdmaHVvIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzEzMjk3OTQsImV4cCI6MjA4NjkwNTc5NH0.JF11ajRjbmVUcapOb08WTGo6rx5lGai0Dx5UVwN074E"

// Topic de Phoenix para system_commands
#define REALTIME_TOPIC  "realtime:system_commands"

/**
 * @brief Callback para eventos de Supabase Realtime
 */
static void on_realtime_event(const char *event, const char *payload, void *user_data)
{
    ESP_LOGI(TAG, "Evento recibido: %s", event);

    // Solo procesar eventos INSERT
    if (strcmp(event, "INSERT") != 0) {
        return;
    }

    // Parsear payload
    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        ESP_LOGE(TAG, "Error parseando payload JSON");
        return;
    }

    // Extraer el record (payload puede estar nested en diferentes formatos)
    cJSON *record = cJSON_GetObjectItem(root, "record");
    if (!record) {
        // Intentar formato alternativo
        record = root;
    }

    if (record) {
        cJSON *command = cJSON_GetObjectItem(record, "command");
        cJSON *status = cJSON_GetObjectItem(record, "status");
        cJSON *id = cJSON_GetObjectItem(record, "id");

        if (command && status && strcmp(status->valuestring, "pending") == 0) {
            const char *cmd_str = command->valuestring;
            ESP_LOGI(TAG, "🎯 Comando recibido: %s (id: %s)", cmd_str, id ? id->valuestring : "unknown");

            // Procesar comando
            bool executed = false;

            if (strcmp(cmd_str, "ARM") == 0) {
                controller_message_t msg = {
                    .header = {
                        .src_id = "RT_CMD",
                        .dst_id = "CTRL",
                        .timestamp = esp_timer_get_time() / 1000
                    },
                    .payload = {
                        .type = MSG_TYPE_ARM_COMMAND
                    }
                };

                if (xQueueSend(gSystemCtx.controller_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    executed = true;
                    ESP_LOGI(TAG, "✅ Comando ARM enviado al controller");
                }
            } else if (strcmp(cmd_str, "DISARM") == 0) {
                controller_message_t msg = {
                    .header = {
                        .src_id = "RT_CMD",
                        .dst_id = "CTRL",
                        .timestamp = esp_timer_get_time() / 1000
                    },
                    .payload = {
                        .type = MSG_TYPE_DISARM_COMMAND
                    }
                };

                if (xQueueSend(gSystemCtx.controller_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    executed = true;
                    ESP_LOGI(TAG, "✅ Comando DISARM enviado al controller");
                }
            } else if (strcmp(cmd_str, "TEST") == 0) {
                executed = true;
                ESP_LOGI(TAG, "✅ Comando TEST recibido");
            }

            // TODO: Actualizar estado del comando a 'executed' en Supabase
            // Esto requeriría una llamada HTTP o un broadcast por WebSocket
        }
    }

    cJSON_Delete(root);
}

// ============================================================================
// Funciones públicas
// ============================================================================

esp_err_t realtime_commands_init(void)
{
    ESP_LOGI(TAG, "Inicializando comandos realtime...");

    // Inicializar cliente Phoenix
    esp_err_t ret = phoenix_init(SUPABASE_PROJECT_URL, SUPABASE_ANON_KEY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error inicializando Phoenix client: %s", esp_err_to_name(ret));
        return ret;
    }

    // Conectar a Supabase Realtime
    ret = phoenix_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error conectando a Supabase Realtime: %s", esp_err_to_name(ret));
        return ret;
    }

    // Suscribirse al canal de system_commands
    ret = phoenix_subscribe(REALTIME_TOPIC, on_realtime_event, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error suscribiendo a %s: %s", REALTIME_TOPIC, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "✅ Comandos realtime iniciados");
    ESP_LOGI(TAG, "Escuchando comandos ARM/DISARM en tiempo real...");

    return ESP_OK;
}

esp_err_t realtime_commands_stop(void)
{
    ESP_LOGI(TAG, "Deteniendo comandos realtime...");
    return phoenix_disconnect();
}
