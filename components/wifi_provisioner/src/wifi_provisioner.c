/**
 * @file wifi_provisioner.c
 * @brief Implementación del provisionador WiFi con SoftAP y portal cautivo
 */

#include "wifi_provisioner.h"
#include "device_identity.h"
#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "dns_server.h"
#include "http_server.h"

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

static const char *TAG = "wifi_prov";

// Configuración SoftAP
#define SOFTAP_S_PREFIX     "Ghost-Setup-"
#define SOFTAP_IP           "192.168.4.1"
#define SOFTAP_NETMASK      "255.255.255.0"
#define SOFTAP_CHANNEL      1
#define SOFTAP_MAX_CONN     4

// Timeout para escanear redes
#define SCAN_TIMEOUT_MS     5000

// Event bits
#define PROV_STOP_BIT       BIT0

// Variables estáticas
static struct {
    bool initialized;
    bool running;
    prov_state_t state;
    char ap_ssid[32];
    esp_netif_t *ap_netif;
    EventGroupHandle_t event_group;
    prov_event_callback_t event_cb;
    void *event_ctx;
    prov_connect_callback_t connect_cb;
    void *connect_ctx;
} s_prov = {
    .initialized = false,
    .running = false,
    .state = PROV_STATE_IDLE,
    .ap_netif = NULL,
    .event_group = NULL,
};

// Handlers de eventos
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);

// ============================================================================
// Inicialización
// ============================================================================

esp_err_t wifi_provisioner_init(void)
{
    if (s_prov.initialized) {
        ESP_LOGW(TAG, "Provisionador ya inicializado");
        return ESP_OK;
    }

    // Crear event group
    s_prov.event_group = xEventGroupCreate();
    if (s_prov.event_group == NULL) {
        ESP_LOGE(TAG, "Error creando event group");
        return ESP_ERR_NO_MEM;
    }

    // Obtener device_id para el SSID
    char device_id[DEVICE_ID_LEN];
    esp_err_t ret = device_identity_get_id(device_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error obteniendo device_id: %s", esp_err_to_name(ret));
        return ret;
    }

    // Generar SSID del SoftAP
    snprintf(s_prov.ap_ssid, sizeof(s_prov.ap_ssid),
             "%s%s", SOFTAP_S_PREFIX, device_id);

    ESP_LOGI(TAG, "Provisionador inicializado");
    ESP_LOGI(TAG, "  SSID: %s", s_prov.ap_ssid);
    ESP_LOGI(TAG, "  IP: %s", SOFTAP_IP);

    s_prov.initialized = true;
    return ESP_OK;
}

// ============================================================================
// Control del provisionador
// ============================================================================

esp_err_t wifi_provisioner_start(prov_event_callback_t event_cb, void *ctx)
{
    if (!s_prov.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_prov.running) {
        ESP_LOGW(TAG, "Provisionador ya está corriendo");
        return ESP_OK;
    }

    s_prov.event_cb = event_cb;
    s_prov.event_ctx = ctx;

    notify_state(PROV_STATE_STARTING);

    // Registrar handler de eventos WiFi
    esp_err_t ret = esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL,
        NULL
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error registrando handler WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    // Cambiar a modo AP+STA
    ESP_LOGI(TAG, "Configurando WiFi en modo AP+STA...");
    ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando modo APSTA: %s", esp_err_to_name(ret));
        return ret;
    }

    // Crear interfaz AP
    s_prov.ap_netif = esp_netif_create_default_wifi_ap();

    // Configurar SoftAP
    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = strlen(s_prov.ap_ssid),
            .channel = SOFTAP_CHANNEL,
            .max_connection = SOFTAP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,  // Portal abierto, protección por token
        },
    };
    strncpy((char *)ap_config.ap.ssid, s_prov.ap_ssid, sizeof(ap_config.ap.ssid));

    ret = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando SoftAP: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configurar IP estática del SoftAP
    esp_netif_ip_info_t ip_info;
    ip_info.ip.addr = esp_ip4addr_aton(SOFTAP_IP);
    ip_info.netmask.addr = esp_ip4addr_aton(SOFTAP_NETMASK);
    ip_info.gw.addr = esp_ip4addr_aton(SOFTAP_IP);
    esp_netif_dhcps_stop(s_prov.ap_netif);
    esp_netif_set_ip_info(s_prov.ap_netif, &ip_info);
    esp_netif_dhcps_start(s_prov.ap_netif);

    // Iniciar servidor DNS (captive portal)
    ret = dns_server_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando DNS server: %s", esp_err_to_name(ret));
        return ret;
    }

    // Iniciar servidor HTTP
    ret = http_server_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando HTTP server: %s", esp_err_to_name(ret));
        dns_server_stop();
        return ret;
    }

    s_prov.running = true;
    notify_state(PROV_STATE_RUNNING);

    ESP_LOGI(TAG, "✅ Provisionador iniciado");
    ESP_LOGI(TAG, "   SoftAP: %s", s_prov.ap_ssid);
    ESP_LOGI(TAG, "   IP: %s", SOFTAP_IP);

    return ESP_OK;
}

esp_err_t wifi_provisioner_stop(void)
{
    if (!s_prov.running) {
        return ESP_OK;
    }

    notify_state(PROV_STATE_STOPPING);

    // Detener servidores
    http_server_stop();
    dns_server_stop();

    // Destruir interfaz AP
    if (s_prov.ap_netif) {
        esp_netif_destroy(s_prov.ap_netif);
        s_prov.ap_netif = NULL;
    }

    // Volver a modo STA
    esp_wifi_set_mode(WIFI_MODE_STA);

    s_prov.running = false;
    notify_state(PROV_STATE_IDLE);

    ESP_LOGI(TAG, "Provisionador detenido");

    return ESP_OK;
}

// ============================================================================
// Scan de redes
// ============================================================================

esp_err_t wifi_provisioner_scan(wifi_scan_result_t *results,
                                 size_t max_results,
                                 size_t *found_results)
{
    if (!s_prov.running) {
        return ESP_ERR_INVALID_STATE;
    }

    if (results == NULL || found_results == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Escaneando redes WiFi...");

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando scan: %s", esp_err_to_name(ret));
        return ret;
    }

    // Esperar resultados
    uint16_t ap_count = 0;
    ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error obteniendo número de APs: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ap_count == 0) {
        ESP_LOGW(TAG, "No se encontraron redes");
        *found_results = 0;
        return ESP_OK;
    }

    // Limitar a max_results
    if (ap_count > max_results) {
        ap_count = max_results;
    }

    wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (ap_records == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error obteniendo records: %s", esp_err_to_name(ret));
        free(ap_records);
        return ret;
    }

    // Copiar resultados
    for (uint16_t i = 0; i < ap_count; i++) {
        strncpy(results[i].ssid, (char *)ap_records[i].ssid, sizeof(results[i].ssid) - 1);
        results[i].ssid[sizeof(results[i].ssid) - 1] = '\0';
        memcpy(results[i].bssid, ap_records[i].bssid, 6);
        results[i].channel = ap_records[i].primary;
        results[i].rssi = ap_records[i].rssi;
        results[i].authmode = ap_records[i].authmode;
    }

    free(ap_records);

    *found_results = ap_count;
    ESP_LOGI(TAG, "✅ Scan completo: %d redes encontradas", ap_count);

    return ESP_OK;
}

// ============================================================================
// Conexión a WiFi
// ============================================================================

esp_err_t wifi_provisioner_connect(const char *ssid, const char *password,
                                    prov_connect_callback_t connect_cb, void *ctx)
{
    if (!s_prov.running) {
        return ESP_ERR_INVALID_STATE;
    }

    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_prov.connect_cb = connect_cb;
    s_prov.connect_ctx = ctx;

    notify_state(PROV_STATE_CONNECTING);

    ESP_LOGI(TAG, "Conectando a: %s", ssid);

    // Usar wifi_manager para conectar
    esp_err_t ret = wifi_manager_connect(ssid, password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando conexión: %s", esp_err_to_name(ret));
        notify_state(PROV_STATE_FAILED);
        if (connect_cb) {
            connect_cb(false, NULL, ctx);
        }
        return ret;
    }

    // La conexión es asíncrona, el resultado vendrá por eventos
    return ESP_OK;
}

// ============================================================================
// Getters y callbacks
// ============================================================================

prov_state_t wifi_provisioner_get_state(void)
{
    return s_prov.state;
}

void wifi_provisioner_set_callback(prov_event_callback_t callback, void *ctx)
{
    s_prov.event_cb = callback;
    s_prov.event_ctx = ctx;
}

esp_err_t wifi_provisioner_get_ap_ssid(char *ssid)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(ssid, s_prov.ap_ssid, 32);
    return ESP_OK;
}

const char *wifi_provisioner_get_ap_ip(void)
{
    return SOFTAP_IP;
}

bool wifi_provisioner_is_running(void)
{
    return s_prov.running;
}

// ============================================================================
// Funciones internas
// ============================================================================

static void notify_state(prov_state_t new_state)
{
    s_prov.state = new_state;

    if (s_prov.event_cb) {
        s_prov.event_cb(new_state, s_prov.event_ctx);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGD(TAG, "STA iniciado");
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "✅ Conectado al AP, esperando IP...");
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *event = event_data;
                ESP_LOGW(TAG, "Desconectado, razón: %d", event->reason);

                if (s_prov.state == PROV_STATE_CONNECTING) {
                    notify_state(PROV_STATE_FAILED);
                    if (s_prov.connect_cb) {
                        s_prov.connect_cb(false, NULL, s_prov.connect_ctx);
                    }
                }
                break;
            }

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *event = event_data;
                ESP_LOGI(TAG, "Station conectada al SoftAP: " MACSTR,
                         MAC2STR(event->mac));
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *event = event_data;
                ESP_LOGI(TAG, "Station desconectada del SoftAP: " MACSTR,
                         MAC2STR(event->mac));
                break;
            }

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = event_data;
            char ip_str[16];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));

            ESP_LOGI(TAG, "✅ IP obtenida: %s", ip_str);

            if (s_prov.state == PROV_STATE_CONNECTING) {
                notify_state(PROV_STATE_CONNECTED);

                // Marcar como provisionado
                device_identity_set_provisioned();

                if (s_prov.connect_cb) {
                    s_prov.connect_cb(true, ip_str, s_prov.connect_ctx);
                }
            }
        }
    }
}
