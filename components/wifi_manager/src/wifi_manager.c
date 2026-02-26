/**
 * @file wifi_manager.c
 * @brief Implementación del gestor de conectividad WiFi para Ghost Gateway
 * 
 * Maneja conexión WiFi STA con reconexión automática y almacenamiento
 * de credenciales en NVS. Compatible con ESP-IDF v5.5.1
 */

#include "wifi_manager.h"
#include <string.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Tag para logging
static const char *TAG = "wifi_manager";

// Namespace y claves NVS para almacenamiento
#define NVS_NAMESPACE       "wifi_cfg"
#define NVS_KEY_SSID        "ssid"
#define NVS_KEY_PASSWORD    "pass"

// Bits del event group para sincronización
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

// Timeout de conexión (10 segundos)
#define WIFI_CONNECT_TIMEOUT_MS  10000

// Variables estáticas del módulo
static wifi_state_t s_wifi_state = WIFI_STATE_DISCONNECTED;
static wifi_event_callback_t s_callback = NULL;
static void *s_callback_ctx = NULL;
static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;
static bool s_initialized = false;
static int s_retry_count = 0;
static int s_max_retries = 5;

// Declaración de handlers de eventos
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data);

/**
 * @brief Notifica cambio de estado via callback
 */
static void notify_state_change(wifi_state_t new_state)
{
    s_wifi_state = new_state;
    if (s_callback != NULL) {
        s_callback(new_state, s_callback_ctx);
    }
}

/**
 * @brief Handler para eventos WiFi
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi STA iniciado");
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "Conectado al AP, esperando IP...");
                s_retry_count = 0;
                notify_state_change(WIFI_STATE_CONNECTING);
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
                ESP_LOGW(TAG, "Desconectado del AP, razón: %d", event->reason);
                
                if (s_retry_count < s_max_retries) {
                    ESP_LOGI(TAG, "Reintentando conexión (%d/%d)...", 
                             s_retry_count + 1, s_max_retries);
                    esp_wifi_connect();
                    s_retry_count++;
                    notify_state_change(WIFI_STATE_CONNECTING);
                } else {
                    ESP_LOGE(TAG, "Máximo de reintentos alcanzado");
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                    notify_state_change(WIFI_STATE_ERROR);
                }
                break;
            }

            default:
                break;
        }
    }
}

/**
 * @brief Handler para eventos IP
 */
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "IP obtenida: " IPSTR, IP2STR(&event->ip_info.ip));
        
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        s_retry_count = 0;
        notify_state_change(WIFI_STATE_CONNECTED);
    }
}

esp_err_t wifi_manager_init(void)
{
    esp_err_t ret;

    if (s_initialized) {
        ESP_LOGW(TAG, "WiFi manager ya inicializado");
        return ESP_OK;
    }

    // Crear event group para sincronización
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Error creando event group");
        return ESP_ERR_NO_MEM;
    }

    // Inicializar NVS si no está inicializado
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS necesita ser borrado, borrando...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Crear el event loop default si no existe
    // Esto es necesario antes de cualquier otra inicialización
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Error creando event loop: %s", esp_err_to_name(ret));
        return ret;
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "Event loop ya existe");
    }

    // Inicializar TCP/IP stack
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Error inicializando netif: %s", esp_err_to_name(ret));
        return ret;
    }

    // Crear interfaz STA usando el método estándar
    // esp_netif_create_default_wifi_sta() crea el netif Y lo conecta al driver
    ESP_LOGI(TAG, "Creando netif STA...");
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "Error creando interfaz STA");
        return ESP_ERR_NO_MEM;
    }

    // Configuración WiFi por defecto
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Error inicializando WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    // Registrar handlers de eventos
    ret = esp_event_handler_instance_register(WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &wifi_event_handler,
                                               NULL,
                                               NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error registrando handler WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_instance_register(IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               &ip_event_handler,
                                               NULL,
                                               NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error registrando handler IP: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configurar modo estación
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando modo STA: %s", esp_err_to_name(ret));
        return ret;
    }

    // Iniciar WiFi
    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Error iniciando WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    s_wifi_state = WIFI_STATE_DISCONNECTED;
    ESP_LOGI(TAG, "WiFi manager inicializado correctamente");
    
    return ESP_OK;
}

/**
 * @brief Guarda credenciales en NVS
 */
static esp_err_t save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error abriendo NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error guardando SSID: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    ret = nvs_set_str(handle, NVS_KEY_PASSWORD, password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error guardando password: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Credenciales guardadas correctamente");
    }
    
    return ret;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "WiFi manager no inicializado");
        return ESP_ERR_INVALID_STATE;
    }

    if (ssid == NULL || password == NULL) {
        ESP_LOGE(TAG, "SSID o password NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Limpiar bits previos
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    // Configurar conexión
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    // Guardar credenciales en NVS
    save_credentials(ssid, password);

    // Iniciar conexión
    s_retry_count = 0;
    notify_state_change(WIFI_STATE_CONNECTING);
    
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando conexión: %s", esp_err_to_name(ret));
        notify_state_change(WIFI_STATE_ERROR);
        return ret;
    }

    // Esperar resultado de conexión
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE,
                                            pdFALSE,
                                            pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Conectado a SSID: %s", ssid);
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Falló la conexión a SSID: %s", ssid);
        return ESP_ERR_WIFI_CONN;
    } else {
        ESP_LOGW(TAG, "Timeout conectando a SSID: %s", ssid);
        notify_state_change(WIFI_STATE_ERROR);
        return ESP_ERR_TIMEOUT;
    }
}

esp_err_t wifi_manager_connect_saved(void)
{
    wifi_credentials_t creds;
    esp_err_t ret = wifi_manager_get_saved_credentials(&creds);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No hay credenciales guardadas");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Conectando con credenciales guardadas: %s", creds.ssid);
    return wifi_manager_connect(creds.ssid, creds.password);
}

esp_err_t wifi_manager_disconnect(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_wifi_disconnect();
    if (ret == ESP_OK) {
        notify_state_change(WIFI_STATE_DISCONNECTED);
        ESP_LOGI(TAG, "Desconectado de WiFi");
    }
    
    return ret;
}

wifi_state_t wifi_manager_get_state(void)
{
    return s_wifi_state;
}

void wifi_manager_set_callback(wifi_event_callback_t callback, void *ctx)
{
    s_callback = callback;
    s_callback_ctx = ctx;
}

esp_err_t wifi_manager_get_saved_credentials(wifi_credentials_t *creds)
{
    if (creds == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t ssid_len = sizeof(creds->ssid);
    size_t pass_len = sizeof(creds->password);

    ret = nvs_get_str(handle, NVS_KEY_SSID, creds->ssid, &ssid_len);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }

    ret = nvs_get_str(handle, NVS_KEY_PASSWORD, creds->password, &pass_len);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t wifi_manager_clear_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_erase_key(handle, NVS_KEY_SSID);
    nvs_erase_key(handle, NVS_KEY_PASSWORD);
    ret = nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Credenciales WiFi borradas");
    return ret;
}

bool wifi_manager_has_saved_credentials(void)
{
    wifi_credentials_t creds;
    return (wifi_manager_get_saved_credentials(&creds) == ESP_OK);
}

esp_err_t wifi_manager_get_ip(char *ip_str)
{
    if (ip_str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_wifi_state != WIFI_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(s_sta_netif, &ip_info);
    if (ret != ESP_OK) {
        return ret;
    }

    snprintf(ip_str, 16, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}