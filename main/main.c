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
#include "supabase_client.h"
#include "sntp_sync.h"

// ============================================================================
// Constantes y configuración
// ============================================================================

static const char *TAG = "GATEWAY_MAIN";

// Credenciales WiFi para prueba de integración
#define WIFI_SSID "Lescano-WiFi"
#define WIFI_PASSWORD "20259738866"

// ============================================================================
// Variables estáticas
// ============================================================================

static bool s_wifi_connected = false;

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
        
        // Inicializar SNTP para sincronización de hora
        ESP_LOGI(TAG, "Inicializando SNTP...");
        sntp_sync_init();

        // Inicializar cliente Supabase después de conectar WiFi
        ESP_LOGI(TAG, "Inicializando cliente Supabase...");
        esp_err_t ret = supabase_client_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Error inicializando Supabase client: %s", esp_err_to_name(ret));
            return;
        }
        
        // Enviar evento de prueba
        ESP_LOGI(TAG, "Enviando evento de prueba a Supabase...");
        
        // Crear evento de prueba usando la estructura device_event_t
        device_event_t test_event = {
            .event_type = "TEST",
            .event_timestamp = NULL,  // Se generará automáticamente
            .device_id = "GATEWAY_001",
            .device_type = "GATEWAY",
            .presence = false,
            .distance_cm = 0.0f,
            .direction = -1,  // No válido
            .behavior = -1,  // No válido
            .active_zone = -1,  // No válido
            .energy_data = NULL
        };
        
        ret = supabase_send_event(&test_event);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Evento de prueba enviado correctamente!");
        } else {
            ESP_LOGE(TAG, "Error enviando evento: %s", esp_err_to_name(ret));
        }
        
    } else if (state == WIFI_STATE_DISCONNECTED) {
        s_wifi_connected = false;
        ESP_LOGW(TAG, "WiFi desconectado");
    } else if (state == WIFI_STATE_ERROR) {
        ESP_LOGE(TAG, "Error de conexión WiFi");
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
    
    // 2. Inicializar UI (LED indicador)
    //    Se inicializa primero para dar feedback visual durante el boot
    ESP_LOGI(TAG, "Inicializando UI...");
    ESP_ERROR_CHECK(ui_init());
    
    // 3. Inicializar controlador
    //    Crea la cola de mensajes, carga estado desde NVS, crea tarea
    ESP_LOGI(TAG, "Inicializando controlador...");
    ESP_ERROR_CHECK(controller_init());
    
    // 4. Configurar callbacks del botón BOOT
    ui_set_button_click_callback(on_boot_button_click);
    ui_set_button_long_press_callback(on_boot_button_long_press);
    
    // 5. Inicializar WiFi Manager PRIMERO
    //    Esto asegura que los handlers de eventos WiFi se registren correctamente
    //    antes de que comm/ESP-Now inicialice WiFi
    ESP_LOGI(TAG, "Inicializando WiFi Manager...");
    ESP_ERROR_CHECK(wifi_manager_init());
    
    // 6. Registrar callback de WiFi
    wifi_manager_set_callback(on_wifi_state_change, NULL);
    
    // 7. Conectar a WiFi
    ESP_LOGI(TAG, "Conectando a WiFi: %s", WIFI_SSID);
    esp_err_t ret = wifi_manager_connect(WIFI_SSID, WIFI_PASSWORD);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo conectar a WiFi. Continuando en modo offline...");
        // No crashear - el sistema puede funcionar sin backend
        // ESP-Now seguirá funcionando para comunicación local
    }
    
    // 8. Inicializar comunicación ESP-Now
    //    Requiere que controller_queue esté creado y WiFi esté inicializado
    ESP_LOGI(TAG, "Inicializando comunicación ESP-Now...");
    ESP_ERROR_CHECK(comm_init());
    
    // 9. Actualizar LED según estado inicial
    system_state_t initial_state = controller_get_state();
    ESP_LOGI(TAG, "Estado inicial: %d", initial_state);
    ui_set_system_state(initial_state);
    
    ESP_LOGI(TAG, "Sistema iniciado correctamente");
    ESP_LOGI(TAG, "Esperando eventos de sensores...");
    ESP_LOGI(TAG, "Botón BOOT: click=toggle arm/desarm, long press=desarmar");
    
    // app_main retorna y el scheduler de FreeRTOS toma el control
    // Las tareas de controller y comm se ejecutan en sus propios componentes
}
