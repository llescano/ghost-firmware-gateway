/**
 * @file main.c
 * @brief Punto de entrada principal del Gateway de Seguridad
 *
 * Este archivo contiene la inicialización del sistema y delega
 * la lógica a los componentes controller, comm y ui.
 *
 * @target ESP32-S3-Zero
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "system_globals.h"
#include "controller.h"
#include "comm.h"
#include "ui.h"
#include "wifi_manager.h"
#include "wifi_provisioner.h"
#include "device_identity.h"
#include "supabase_client.h"
#include "sntp_sync.h"
#include "realtime_commands.h"

// ============================================================================
// Constantes y configuración
// ============================================================================

static const char *TAG = "GATEWAY_MAIN";

// ============================================================================
// Variables estáticas
// ============================================================================

static bool s_wifi_connected = false;
static bool s_provisioning_mode = false;

// ============================================================================
// Callbacks del botón BOOT
// ============================================================================

/**
 * @brief Callback para click simple del botón BOOT
 * 
 * Toggle entre ARMED y DISARMED
 */
static void on_boot_button_click(void)
{
    system_state_t current = controller_get_state();
    
    if (current == SYS_STATE_DISARMED) {
        ESP_LOGI(TAG, "Botón: Armando sistema");
        controller_arm();
    } else if (current == SYS_STATE_ARMED) {
        ESP_LOGI(TAG, "Botón: Desarmando sistema");
        controller_disarm();
    } else if (current == SYS_STATE_ALARM || current == SYS_STATE_TAMPER) {
        // Desde ALARM o TAMPER, un click también desarma
        ESP_LOGI(TAG, "Botón: Desarmando sistema desde ALARM/TAMPER");
        controller_disarm();
    }
}

/**
 * @brief Callback para long press del botón BOOT
 * 
 * Siempre desarma el sistema (útil para emergencias)
 */
static void on_boot_button_long_press(void)
{
    ESP_LOGI(TAG, "Botón: Long press - Desarmando sistema");
    controller_disarm();
}

// ============================================================================
// Callbacks de WiFi
// ============================================================================

/**
 * @brief Callback para cambios de estado WiFi
 *
 * @param state Nuevo estado de conexión WiFi
 * @param ctx Contexto de usuario (no usado)
 */
static void on_wifi_state_change(wifi_state_t state, void *ctx)
{
    ESP_LOGI(TAG, "WiFi state changed: %d", state);

    if (state == WIFI_STATE_CONNECTED) {
        s_wifi_connected = true;

        char ip_str[16] = {0};
        if (wifi_manager_get_ip(ip_str) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi conectado! IP: %s", ip_str);
        }

        // Si estábamos en modo provisionamiento, detenerlo
        if (s_provisioning_mode) {
            ESP_LOGI(TAG, "Deteniendo modo provisionamiento...");
            wifi_provisioner_stop();
            s_provisioning_mode = false;
        }

        // Inicializar servicios que requieren WiFi
        init_wifi_services();

    } else if (state == WIFI_STATE_DISCONNECTED) {
        s_wifi_connected = false;
        ESP_LOGW(TAG, "WiFi desconectado");
    } else if (state == WIFI_STATE_ERROR) {
        ESP_LOGE(TAG, "Error de conexión WiFi");
    }
}

/**
 * @brief Callback para eventos del provisionador
 *
 * @param state Nuevo estado del provisionador
 * @param ctx Contexto de usuario (no usado)
 */
static void on_provisioner_event(prov_state_t state, void *ctx)
{
    ESP_LOGI(TAG, "Provisioner state: %d", state);

    if (state == PROV_STATE_CONNECTED) {
        ESP_LOGI(TAG, "✅ WiFi configurado exitosamente!");
        // El provisionador se detendrá automáticamente en on_wifi_state_change
    } else if (state == PROV_STATE_FAILED) {
        ESP_LOGW(TAG, "Falló la conexión WiFi");
        // El portal sigue disponible para reintentar
    }
}

/**
 * @brief Inicializa servicios que dependen de WiFi
 *
 * Se llama después de conectar exitosamente (con o sin provisionamiento)
 */
static void init_wifi_services(void)
{
    esp_err_t ret;

    // Inicializar SNTP para sincronización de hora
    ESP_LOGI(TAG, "Inicializando SNTP...");
    sntp_sync_init();

    // Inicializar cliente Supabase
    ESP_LOGI(TAG, "Inicializando cliente Supabase...");
    ret = supabase_client_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error inicializando Supabase client: %s", esp_err_to_name(ret));
        return;
    }

    // Enviar evento de dispositivo conectado
    ESP_LOGI(TAG, "Enviando evento de conexión a Supabase...");

    char device_id[DEVICE_ID_LEN];
    device_identity_get_id(device_id);

    device_event_t connect_event = {
        .event_type = "DEVICE_ONLINE",
        .event_timestamp = NULL,
        .device_id = device_id,
        .device_type = "GATEWAY",
        .presence = false,
        .distance_cm = 0.0f,
        .direction = -1,
        .behavior = -1,
        .active_zone = -1,
        .energy_data = NULL
    };

    ret = supabase_send_event(&connect_event);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Evento de conexión enviado");
    } else {
        ESP_LOGE(TAG, "Error enviando evento: %s", esp_err_to_name(ret));
    }

    // Inicializar comandos en tiempo real (WebSocket)
    ESP_LOGI(TAG, "Inicializando comandos realtime (WebSocket)...");
    ret = realtime_commands_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error inicializando realtime commands: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "✅ Comandos realtime iniciados - WebSocket activo");
    }
}

// ============================================================================
// Funciones de inicialización
// ============================================================================

/**
 * @brief Inicializa NVS (Non-Volatile Storage)
 * 
 * NVS se usa para almacenar:
 * - Modo de arranque (boot_mode)
 * - Último estado del sistema
 * - Configuración de sensores
 * 
 * @return ESP_OK si exitoso
 */
static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS corrupto o versión diferente - borrar y reinicializar
        ESP_LOGW(TAG, "NVS necesita ser borrado, reinicializando...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    
    ESP_LOGI(TAG, "NVS inicializado correctamente");
    return ret;
}

// ============================================================================
// Punto de entrada principal
// ============================================================================

/**
 * @brief Función principal del programa
 * 
 * Inicializa el sistema y delega a los componentes
 */
void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  GHOST - Gateway de Seguridad");
    ESP_LOGI(TAG, "  Target: ESP32-S3-Zero");
    ESP_LOGI(TAG, "========================================");

    // 1. Inicializar NVS
    ESP_ERROR_CHECK(init_nvs());

    // 2. Inicializar identidad del dispositivo
    ESP_LOGI(TAG, "Inicializando identidad del dispositivo...");
    ESP_ERROR_CHECK(device_identity_init());

    char device_id[DEVICE_ID_LEN];
    device_identity_get_id(device_id);
    ESP_LOGI(TAG, "Device ID: %s", device_id);

    // 3. Inicializar UI (LED indicador)
    ESP_LOGI(TAG, "Inicializando UI...");
    ESP_ERROR_CHECK(ui_init());

    // 4. Inicializar controlador
    ESP_LOGI(TAG, "Inicializando controlador...");
    ESP_ERROR_CHECK(controller_init());

    // 5. Configurar callbacks del botón BOOT
    ui_set_button_click_callback(on_boot_button_click);
    ui_set_button_long_press_callback(on_boot_button_long_press);

    // 6. Inicializar WiFi Manager
    ESP_LOGI(TAG, "Inicializando WiFi Manager...");
    ESP_ERROR_CHECK(wifi_manager_init());

    // 7. Registrar callback de WiFi
    wifi_manager_set_callback(on_wifi_state_change, NULL);

    // 8. Verificar si el dispositivo está provisionado
    if (!device_identity_is_provisioned()) {
        // Modo PROVISIONAMIENTO: iniciar SoftAP con portal cautivo
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "📱 Dispositivo no provisionado - Iniciando modo setup");
        ESP_LOGI(TAG, "");

        ESP_ERROR_CHECK(wifi_provisioner_init());
        ESP_ERROR_CHECK(wifi_provisioner_start(on_provisioner_event, NULL));

        s_provisioning_mode = true;

        char ap_ssid[32];
        wifi_provisioner_get_ap_ssid(ap_ssid);
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "  MODO PROVISIONAMIENTO ACTIVO");
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "  SSID: %s", ap_ssid);
        ESP_LOGI(TAG, "  IP: %s", wifi_provisioner_get_ap_ip());
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "1. Conéctate a la red WiFi arriba indicada");
        ESP_LOGI(TAG, "2. Se abrirá automáticamente el portal de configuración");
        ESP_LOGI(TAG, "3. Selecciona tu red WiFi e ingresa la contraseña");
        ESP_LOGI(TAG, "4. Escanea el QR para vincular el dispositivo");
        ESP_LOGI(TAG, "");

    } else {
        // MODO NORMAL: conectar con credenciales guardadas
        ESP_LOGI(TAG, "Dispositivo provisionado - Conectando con credenciales guardadas...");
        esp_err_t ret = wifi_manager_connect_saved();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo conectar con credenciales guardadas: %s", esp_err_to_name(ret));
            ESP_LOGI(TAG, "Podría ser necesario resetear la configuración...");
            // No iniciar modo AP automáticamente - podría ser un problema temporal
        }
    }

    // 9. Inicializar comunicación ESP-Now
    ESP_LOGI(TAG, "Inicializando comunicación ESP-Now...");
    ESP_ERROR_CHECK(comm_init());

    // 10. Actualizar LED según estado inicial
    system_state_t initial_state = controller_get_state();
    ESP_LOGI(TAG, "Estado inicial: %d", initial_state);
    ui_set_system_state(initial_state);

    ESP_LOGI(TAG, "Sistema iniciado correctamente");
    ESP_LOGI(TAG, "Botón BOOT: click=toggle arm/desarm, long press=desarmar");
}
