/**
 * @file sntp_sync.c
 * @brief Implementación del componente de sincronización SNTP
 */

#include "sntp_sync.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <time.h>

static const char *TAG = "SNTP_SYNC";

// ============================================================================
// Estado interno
// ============================================================================

typedef enum {
    SNTP_STATE_NOT_INIT = 0,
    SNTP_STATE_WAITING,     // Esperando primera sincronización
    SNTP_STATE_SYNCED,      // Sincronizado correctamente
    SNTP_STATE_ERROR        // Error de sincronización
} sntp_state_t;

typedef struct {
    sntp_state_t state;
    bool initialized;
    TaskHandle_t task_handle;
    EventGroupHandle_t events;
    time_t last_sync_time;
    uint32_t sync_count;
} sntp_context_t;

// Event bits
#define SNTP_EVENT_SYNC_DONE    (1 << 0)
#define SNTP_EVENT_FORCE_SYNC   (1 << 1)

static sntp_context_t s_ctx = {
    .state = SNTP_STATE_NOT_INIT,
    .initialized = false,
    .task_handle = NULL,
    .events = NULL,
    .last_sync_time = 0,
    .sync_count = 0
};

// ============================================================================
// Callbacks SNTP
// ============================================================================

/**
 * @brief Callback invocado por lwIP cuando se sincroniza la hora
 */
static void sntp_sync_callback(struct timeval *tv)
{
    if (tv == NULL) {
        ESP_LOGW(TAG, "Callback SNTP invocado con tv=NULL");
        return;
    }

    // Actualizar estado
    s_ctx.state = SNTP_STATE_SYNCED;
    s_ctx.last_sync_time = tv->tv_sec;
    s_ctx.sync_count++;

    // Formatear y loguear la hora
    struct tm timeinfo;
    localtime_r(&tv->tv_sec, &timeinfo);

    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);

    ESP_LOGI(TAG, "✅ Hora sincronizada: %s (sync #%u)", time_str, s_ctx.sync_count);

    // Notificar a la tarea
    if (s_ctx.events) {
        xEventGroupSetBits(s_ctx.events, SNTP_EVENT_SYNC_DONE);
    }
}

// ============================================================================
// Tarea de sincronización
// ============================================================================

/**
 * @brief Tarea que maneja la sincronización periódica
 */
static void sntp_sync_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Tarea de sincronización iniciada");

    TickType_t last_check = xTaskGetTickCount();
    bool waiting_first_sync = true;

    while (1) {
        // Esperar eventos o timeout
        TickType_t wait_time;
        if (s_ctx.state == SNTP_STATE_SYNCED) {
            wait_time = pdMS_TO_TICKS(SNTP_SYNC_SYNC_INTERVAL * 1000);
        } else {
            wait_time = pdMS_TO_TICKS(SNTP_SYNC_RETRY_INTERVAL * 1000);
        }

        EventBits_t bits = xEventGroupWaitBits(
            s_ctx.events,
            SNTP_EVENT_SYNC_DONE | SNTP_EVENT_FORCE_SYNC,
            pdTRUE,              // Clear bits on exit
            pdFALSE,             // Wait for ANY bit
            wait_time
        );

        // Chequeo periódico del estado
        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);

        // Verificar si tenemos hora válida (año >= 2024)
        bool is_valid = (timeinfo.tm_year + 1900) >= 2024;

        if (is_valid && s_ctx.state == SNTP_STATE_WAITING) {
            // La hora se actualizó (posiblemente por otro medio)
            s_ctx.state = SNTP_STATE_SYNCED;
            s_ctx.last_sync_time = now;
            s_ctx.sync_count++;
            ESP_LOGI(TAG, "Hora válida detectada");
        }

        // Procesar eventos
        if (bits & SNTP_EVENT_FORCE_SYNC) {
            ESP_LOGI(TAG, "Re-sincronización forzada");
            // Reiniciar SNTP para forzar nueva consulta
            esp_sntp_stop();
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_sntp_init();
        }

        // Si nunca sincronizó y ya pasó el timeout, advertir
        if (s_ctx.state == SNTP_STATE_WAITING && waiting_first_sync) {
            waiting_first_sync = false;
            ESP_LOGW(TAG, "⚠️ Esperando sincronización NTP (puede tomar hasta 60s)");
        }

        // Small delay to prevent tight loop
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ============================================================================
// API Pública
// ============================================================================

esp_err_t sntp_sync_init(void)
{
    if (s_ctx.initialized) {
        ESP_LOGW(TAG, "SNTP ya inicializado");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Inicializando SNTP...");

    // Crear grupo de eventos
    s_ctx.events = xEventGroupCreate();
    if (s_ctx.events == NULL) {
        ESP_LOGE(TAG, "Error creando grupo de eventos");
        return ESP_ERR_NO_MEM;
    }

    // Configurar zona horaria
    setenv("TZ", SNTP_SYNC_TZ_DEFAULT, 1);
    tzset();
    ESP_LOGI(TAG, "Zona horaria: %s", SNTP_SYNC_TZ_DEFAULT);

    // Configurar SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, SNTP_SYNC_SERVER_1);
    esp_sntp_setservername(1, SNTP_SYNC_SERVER_2);
    esp_sntp_set_time_sync_notification_cb(sntp_sync_callback);

    // Iniciar cliente SNTP
    esp_sntp_init();

    ESP_LOGI(TAG, "Servidores NTP: %s, %s", SNTP_SYNC_SERVER_1, SNTP_SYNC_SERVER_2);

    // Crear tarea de sincronización
    BaseType_t ret = xTaskCreate(
        sntp_sync_task,
        "sntp_sync",
        SNTP_SYNC_TASK_STACK,
        NULL,
        SNTP_SYNC_TASK_PRIO,
        &s_ctx.task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Error creando tarea de sincronización");
        esp_sntp_stop();
        vEventGroupDelete(s_ctx.events);
        return ESP_ERR_NO_MEM;
    }

    s_ctx.state = SNTP_STATE_WAITING;
    s_ctx.initialized = true;

    ESP_LOGI(TAG, "✅ SNTP inicializado correctamente");

    return ESP_OK;
}

bool sntp_sync_is_synced(void)
{
    return (s_ctx.state == SNTP_STATE_SYNCED);
}

esp_err_t sntp_sync_get_time_str(char *buf, size_t size)
{
    if (!s_ctx.initialized || !sntp_sync_is_synced()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (buf == NULL || size < 16) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t now;
    time(&now);
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);  // Usar gmtime_r para obtener UTC real

    strftime(buf, size, "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

    return ESP_OK;
}

esp_err_t sntp_sync_get_time(time_t *out_time)
{
    if (!s_ctx.initialized || !sntp_sync_is_synced()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (out_time == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    time(out_time);
    return ESP_OK;
}

esp_err_t sntp_sync_force_sync(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ctx.events) {
        xEventGroupSetBits(s_ctx.events, SNTP_EVENT_FORCE_SYNC);
        return ESP_OK;
    }

    return ESP_FAIL;
}
