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

/**
 * @brief Callback para eventos de Supabase Realtime
 */
static void on_realtime_event(const char *event, const char *payload, void *user_data)
{
    ESP_LOGI(TAG, "📥 Comando WebSocket recibido: %s", event);

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
            if (strcmp(cmd_str, "ARM") == 0) {
                controller_message_t msg = {
                    .header = {
                        .version = 1,
                        .src_id = "RT_CMD",
                        .src_type = DEV_TYPE_GATEWAY
                    },
                    .payload = {
                        .type = MSG_TYPE_ARM_COMMAND
                    }
                };

                if (xQueueSend(gSystemCtx.controller_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    ESP_LOGI(TAG, "✅ Comando ARM enviado al controller");
                }
            } else if (strcmp(cmd_str, "DISARM") == 0) {
                controller_message_t msg = {
                    .header = {
                        .version = 1,
                        .src_id = "RT_CMD",
                        .src_type = DEV_TYPE_GATEWAY
                    },
                    .payload = {
                        .type = MSG_TYPE_DISARM_COMMAND
                    }
                };

                if (xQueueSend(gSystemCtx.controller_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    ESP_LOGI(TAG, "✅ Comando DISARM enviado al controller");
                }
            } else if (strcmp(cmd_str, "TEST") == 0) {
                ESP_LOGI(TAG, "✅ Comando TEST recibido");
            }

            // TODO: Actualizar estado del comando a 'executed' en Supabase
            // Esto requeriría una llamada HTTP o un broadcast por WebSocket
        }
    }

    cJSON_Delete(root);
}

/**
 * @brief Callback para eventos de estado desde otros dispositivos (system_events)
 * Sincroniza el estado local cuando otro gateway o la webapp cambia el estado
 */
static void on_state_event(const char *event, const char *payload, void *user_data)
{
    ESP_LOGI(TAG, "📥 Estado sinc recibido (system_events): %s", event);

    // Solo procesar eventos INSERT
    if (strcmp(event, "INSERT") != 0) {
        return;
    }

    // Parsear payload
    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        ESP_LOGE(TAG, "Error parseando payload de estado");
        return;
    }

    // Extraer el record
    cJSON *record = cJSON_GetObjectItem(root, "record");
    if (!record) {
        record = root;
    }

    if (record) {
        // Extraer device_id para evitar procesar eventos propios
        cJSON *device_id = cJSON_GetObjectItem(record, "device_id");

        // Ignorar eventos propios de este dispositivo
        if (device_id && device_id->valuestring &&
            strcmp(device_id->valuestring, "GATEWAY_001") == 0) {
            ESP_LOGD(TAG, "Ignorando evento propio");
            cJSON_Delete(root);
            return;
        }

        // Extraer energy_data
        cJSON *energy_data = cJSON_GetObjectItem(record, "energy_data");
        if (energy_data) {
            cJSON *new_state_obj = cJSON_GetObjectItem(energy_data, "new_state");
            cJSON *new_state_code_obj = cJSON_GetObjectItem(energy_data, "new_state_code");

            if (new_state_obj && new_state_code_obj) {
                const char *new_state = new_state_obj->valuestring;
                int new_state_code = new_state_code_obj->valueint;

                ESP_LOGI(TAG, "📥 Estado remoto recibido de %s: %s (código: %d)",
                         device_id ? device_id->valuestring : "unknown", new_state, new_state_code);

                // Enviar comando al controller para sincronizar estado
                controller_message_t msg = {0};

                if (strcmp(new_state, "ARMADO") == 0 || new_state_code == 1) {
                    msg.payload.type = MSG_TYPE_ARM_COMMAND;
                    ESP_LOGI(TAG, "🔄 Sincronizando estado a ARMADO");
                } else if (strcmp(new_state, "DESARMADO") == 0 || new_state_code == 0) {
                    msg.payload.type = MSG_TYPE_DISARM_COMMAND;
                    ESP_LOGI(TAG, "🔄 Sincronizando estado a DESARMADO");
                } else {
                    goto cleanup;
                }

                msg.header.version = 1;
                strcpy(msg.header.src_id, "RT_STATE");
                msg.header.src_type = DEV_TYPE_GATEWAY;

                if (xQueueSend(gSystemCtx.controller_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    ESP_LOGI(TAG, "✅ Estado sincronizado desde servidor");
                }
            }
        }
    }

cleanup:
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

    // Suscribirse al canal de system_commands con postgres_changes
    ret = phoenix_subscribe_postgres("public", "system_commands", "INSERT", on_realtime_event, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error suscribiendo a system_commands: %s", esp_err_to_name(ret));
        return ret;
    }

    // Suscribirse a system_events para recibir cambios de estado desde otros dispositivos
    ret = phoenix_subscribe_postgres("public", "system_events", "INSERT", on_state_event, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error suscribiendo a system_events: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "✅ Comandos realtime iniciados");
    ESP_LOGI(TAG, "Escuchando comandos ARM/DISARM en tiempo real...");
    ESP_LOGI(TAG, "Escuchando cambios de estado desde otros dispositivos...");

    return ESP_OK;
}

esp_err_t realtime_commands_stop(void)
{
    ESP_LOGI(TAG, "Deteniendo comandos realtime...");
    return phoenix_disconnect();
}
