/**
 * @file http_server.c
 * @brief Servidor HTTP con portal de configuración
 *
 * Implementa un servidor HTTP ligero con páginas HTML inline.
 */

#include "http_server.h"
#include "wifi_provisioner.h"
#include "device_identity.h"
#include "supabase_client.h"
#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_timer.h"

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "cJSON.h"

#include <string.h>

static const char *TAG = "http_server";

static httpd_handle_t s_server = NULL;
static esp_timer_handle_t s_link_code_timer = NULL;

// Variables para link_code asíncrono
static char s_link_code[8] = {0};
static bool s_link_code_ready = false;
static bool s_link_code_pending = false;

/**
 * @brief Callback del timer para obtener link_code en segundo plano
 *
 * Esta función se ejecuta en el contexto del timer, no del handler HTTP,
 * evitando el crash de lwip/mbedtls.
 */
static void link_code_timer_callback(void* arg)
{
    if (s_link_code_pending && !s_link_code_ready) {
        ESP_LOGI(TAG, "Intentando obtener link_code...");

        esp_err_t ret = supabase_get_link_code(s_link_code);
        if (ret == ESP_OK) {
            s_link_code_ready = true;
            s_link_code_pending = false;
            ESP_LOGI(TAG, "✅ Link_code obtenido: %s", s_link_code);
        } else {
            ESP_LOGW(TAG, "Error obteniendo link_code, reintentando en 2s...");
        }
    }
}

// ============================================================================
// HTML/CSS/JS Inline
// ============================================================================

static const char *const index_html =
    "<!DOCTYPE html>"
    "<html lang='es'>"
    "<head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Ghost Setup</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0a0a0a;color:#fff;min-height:100vh;display:flex;flex-direction:column;align-items:center;justify-content:center;padding:20px}"
    ".container{max-width:400px;width:100%;background:#111;border-radius:16px;padding:24px;box-shadow:0 8px 32px rgba(0,0,0,.5)}"
    ".logo{width:64px;height:64px;margin:0 auto 24px;background:linear-gradient(135deg,#6366f1,#8b5cf6);border-radius:16px;display:flex;align-items:center;justify-content:center;font-size:32px;font-weight:bold}"
    "h1{text-align:center;font-size:24px;margin-bottom:8px;font-weight:600}"
    ".subtitle{text-align:center;color:#666;font-size:14px;margin-bottom:24px}"
    ".network-list{display:flex;flex-direction:column;gap:8px;margin-bottom:16px}"
    ".network-item{background:#1a1a1a;border:1px solid #222;border-radius:12px;padding:16px;cursor:pointer;transition:all .2s;display:flex;align-items:center;gap:12px}"
    ".network-item:hover{border-color:#6366f1;background:#1f1f1f}"
    ".network-item.selected{border-color:#6366f1;background:rgba(99,102,241,.1)}"
    ".wifi-icon{width:24px;height:24px;flex-shrink:0}"
    ".network-info{flex:1;min-width:0}"
    ".network-name{font-size:16px;font-weight:500;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
    ".network-strength{font-size:12px;color:#666}"
    ".network-lock{width:16px;height:16px;opacity:.5}"
    ".input-group{margin-bottom:16px}"
    "label{display:block;font-size:14px;color:#999;margin-bottom:8px}"
    "input[type='password']{width:100%;background:#1a1a1a;border:1px solid #222;border-radius:12px;padding:16px;color:#fff;font-size:16px;outline:none;transition:border-color .2s}"
    "input[type='password']:focus{border-color:#6366f1}"
    ".btn{width:100%;background:#6366f1;border:none;border-radius:12px;padding:16px;color:#fff;font-size:16px;font-weight:600;cursor:pointer;transition:background .2s}"
    ".btn:hover{background:#7c3aed}"
    ".btn:disabled{opacity:.5;cursor:not-allowed}"
    ".btn-secondary{background:#222;color:#999}"
    ".btn-secondary:hover{background:#2a2a2a}"
    ".hidden{display:none}"
    ".spinner{width:24px;height:24px;border:3px solid #333;border-top-color:#6366f1;border-radius:50%;animation:spin 1s linear infinite;margin:0 auto}"
    "@keyframes spin{to{transform:rotate(360deg)}}"
    ".status{text-align:center;padding:16px;background:#1a1a1a;border-radius:12px;margin-bottom:16px}"
    ".status.success{background:rgba(34,197,94,.1);color:#22c55e}"
    ".status.error{background:rgba(239,68,68,.1);color:#ef4444}"
    ".status.connecting{background:rgba(99,102,241,.1);color:#6366f1}"
    ".progress{display:flex;flex-direction:column;gap:8px;text-align:left}"
    ".progress-item{display:flex;align-items:center;gap:8px;font-size:14px;color:#999}"
    ".progress-item.done{color:#22c55e}"
    ".progress-item.pending{color:#6366f1}"
    ".progress-item .icon{width:16px;height:16px}"
    ".qr-container{text-align:center;margin:16px 0}"
    ".qr-placeholder{width:200px;height:200px;margin:0 auto;background:#fff;border-radius:16px;padding:16px;display:flex;align-items:center;justify-content:center}"
    ".qr-code{width:100%;height:100%}"
    ".pairing-code{font-family:monospace;font-size:14px;color:#999;margin-top:16px;word-break:break-all}"
    "</style>"
    "</head>"
    "<body>"
    "<div class='container'>"
    "<div class='logo'>👻</div>"
    "<h1>Ghost Setup</h1>"
    "<p class='subtitle' id='subtitle'>Configura tu dispositivo Ghost</p>"
    "<!-- Pagina 1: Scan de redes -->"
    "<div id='page-scan'>"
    "<div class='network-list' id='network-list'>"
    "<div class='status connecting'><div class='spinner'></div><p style='margin-top:8px'>Escaneando redes...</p></div>"
    "</div>"
    "</div>"
    "<!-- Pagina 2: Input de contrasena -->"
    "<div id='page-password' class='hidden'>"
    "<div class='status' id='selected-network'></div>"
    "<div class='input-group'>"
    "<label>Contrasena de WiFi</label>"
    "<input type='password' id='password' placeholder='Ingresa la contraseña'>"
    "</div>"
    "<button class='btn' id='btn-connect'>Conectar</button>"
    "<button class='btn btn-secondary' style='margin-top:8px' id='btn-back-scan'>Volver</button>"
    "</div>"
    "<!-- Pagina 3: Conectando -->"
    "<div id='page-connecting' class='hidden'>"
    "<div class='status connecting'>"
    "<div class='spinner'></div>"
    "<p style='margin-top:8px' id='connecting-text'>Conectando...</p>"
    "</div>"
    "<div class='progress'>"
    "<div class='progress-item pending' id='step-scan'><span class='icon'>○</span> Escaneando redes</div>"
    "<div class='progress-item pending' id='step-connect'><span class='icon'>○</span> Conectando a WiFi</div>"
    "<div class='progress-item pending' id='step-ip'><span class='icon'>○</span> Obteniendo IP</div>"
    "</div>"
    "</div>"
    "<!-- Pagina 4: Exito + Telegram -->"
    "<div id='page-success' class='hidden'>"
    "<div class='status success'>✅ Conectado exitosamente</div>"
    "<p style='text-align:center;color:#999;margin-bottom:16px'>Tu codigo de vinculación es:</p>"
    "<div style='background:#1a1a1a;border:2px solid #6366f1;border-radius:16px;padding:24px;margin:16px 0;text-align:center'>"
    "<span id='link-code' style='font-family:monospace;font-size:36px;font-weight:bold;letter-spacing:4px;color:#6366f1'>----</span>"
    "</div>"
    "<p style='text-align:center;color:#999;margin-bottom:16px'>Para vincular el dispositivo:</p>"
    "<ol style='color:#999;padding-left:24px;line-height:1.8'>"
    "<li>Abre Telegram</li>"
    "<li>Busca <strong>@GhostSecurityBot</strong></li>"
    "<li>Escribe el comando: <code style='background:#222;padding:4px 8px;border-radius:4px;color:#6366f1'>/vincular <span id='code-cmd'>----</span></code></li>"
    "</ol>"
    "<p style='text-align:center;color:#666;font-size:12px;margin-top:16px'>Este codigo expira en 24 horas</p>"
    "</div>"
    "<!-- Pagina 5: Error -->"
    "<div id='page-error' class='hidden'>"
    "<div class='status error' id='error-message'>Error de conexión</div>"
    "<button class='btn' id='btn-retry'>Reintentar</button>"
    "<button class='btn btn-secondary' style='margin-top:8px' id='btn-back-error'>Volver</button>"
    "</div>"
    "</div>"

    "<script>"
    "let selectedNetwork=null;"
    "const apiBase='/api';"

    // Funciones de navegación
    "function showPage(id){document.querySelectorAll('[id^=page-]').forEach(p=>p.classList.add('hidden'));document.getElementById(id).classList.remove('hidden');}"
    "function setStep(id,status){const el=document.getElementById(id);el.classList.remove('pending','done');el.classList.add(status);el.querySelector('.icon').textContent=status==='done'?'✓':'○';}"

    // Scan de redes
    "async function scanNetworks(){try{const res=await fetch(apiBase+'/scan');const data=await res.json();if(data.networks){renderNetworks(data.networks);}else{showError('No se encontraron redes');}}catch(e){console.error(e);showError('Error escaneando redes');}}"
    "function renderNetworks(networks){const list=document.getElementById('network-list');list.innerHTML='';networks.forEach(n=>{const item=document.createElement('div');item.className='network-item';item.innerHTML=''+getSignalIcon(n.rssi)+'<div class=\"network-info\"><div class=\"network-name\">'+escapeHtml(n.ssid)+'</div><div class=\"network-strength\">'+getStrengthText(n.rssi)+'</div></div>'+(n.authmode!==0?'<svg class=\"network-lock\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"><rect x=\"3\" y=\"11\" width=\"18\" height=\"11\" rx=\"2\" ry=\"2\"></rect><path d=\"M7 11V7a5 5 0 0 1 10 0v4\"></path></svg>':'');item.onclick=function(){selectNetwork(n);};list.appendChild(item);});}"
    "function getSignalIcon(rssi){return '<svg class=\"wifi-icon\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"><path d=\"M5 12.55a11 11 0 0 1 14.08 0\"></path><path d=\"M1.42 9a16 16 0 0 1 17.16 0\"></path><path d=\"M8.53 16.11a6 6 0 0 1 6.95 0\"></path><line x1=\"12\" y1=\"20\" x2=\"12.01\" y2=\"20\"></line></svg>';}"
    "function getStrengthText(rssi){return rssi>-60?'Excelente':rssi>-70?'Buena':rssi>-80?'Regular':'Débil';}"
    "function escapeHtml(t){const d=document.createElement('div');d.textContent=t;return d.innerHTML;}"

    // Selección de red
    "function selectNetwork(n){selectedNetwork=n;document.getElementById('selected-network').innerHTML='<strong>'+escapeHtml(n.ssid)+'</strong><br><span style=\\'font-size:12px;color:#666\\'>'+getStrengthText(n.rssi)+'</span>';showPage('page-password');document.getElementById('password').focus();}"

    // Conexión
    "async function connect(){const p=document.getElementById('password').value;if(!p){alert('Ingresa la contraseña');return;}"
    "showPage('page-connecting');setStep('step-scan','done');setStep('step-connect','pending');setStep('step-ip','pending');"
    "try{const res=await fetch(apiBase+'/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:selectedNetwork.ssid,password:p})});const data=await res.json();"
    "if(data.success){setStep('step-connect','done');pollStatus();}else{showError(data.error||'No se pudo conectar');}}catch(e){console.error(e);showError('Error de conexión');}}"

    // Poll de estado
    "async function pollStatus(){try{const res=await fetch(apiBase+'/status');const data=await res.json();if(data.connected){setStep('step-ip','done');pollLinkCode();}else{setTimeout(pollStatus,1000);}}catch(e){setTimeout(pollStatus,1000);}}"

    // Poll de link_code
    "async function pollLinkCode(){try{const res=await fetch(apiBase+'/link-code');const data=await res.json();if(data.ready){showSuccess(data.code);}else{setTimeout(pollLinkCode,1000);}}catch(e){setTimeout(pollLinkCode,1000);}}"

    // Éxito con codigo
    "function showSuccess(code){showPage('page-success');if(code){document.getElementById('link-code').textContent=document.getElementById('code-cmd').textContent=code;}else{document.getElementById('link-code').textContent='ERROR';}}"

    // Error
    "function showError(msg){document.getElementById('error-message').textContent=msg;showPage('page-error');}"

    // Event listeners
    "document.getElementById('btn-connect').onclick=connect;"
    "document.getElementById('btn-back-scan').onclick=()=>showPage('page-scan');"
    "document.getElementById('btn-retry').onclick=scanNetworks;"
    "document.getElementById('btn-back-error').onclick=()=>showPage('page-scan');"
    "document.getElementById('password').addEventListener('keypress',e=>{if(e.key==='Enter')connect();});"

    // Iniciar scan
    "scanNetworks();"
    "</script>"
    "</body>"
    "</html>";

// ============================================================================
// API Endpoints
// ============================================================================

static esp_err_t api_scan_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "API scan handler llamado");
    httpd_resp_set_type(req, "application/json");

    wifi_scan_result_t results[20];
    size_t found = 0;

    esp_err_t ret = wifi_provisioner_scan(results, 20, &found);

    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_CreateArray();

    if (ret == ESP_OK) {
        for (size_t i = 0; i < found; i++) {
            cJSON *net = cJSON_CreateObject();
            cJSON_AddStringToObject(net, "ssid", results[i].ssid);
            cJSON_AddNumberToObject(net, "rssi", results[i].rssi);
            cJSON_AddNumberToObject(net, "channel", results[i].channel);
            cJSON_AddNumberToObject(net, "authmode", results[i].authmode);
            cJSON_AddItemToArray(networks, net);
        }
    }

    cJSON_AddItemToObject(root, "networks", networks);

    char *response = cJSON_Print(root);
    httpd_resp_sendstr(req, response);

    free(response);
    cJSON_Delete(root);

    return ESP_OK;
}

/**
 * @brief Callback para obtener IP del STA después de conectar
 */
static bool wait_for_sta_ip(char *sta_ip, size_t max_len, int timeout_ms)
{
    int64_t start_time = esp_timer_get_time() / 1000;

    while (true) {
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            if (ip_info.ip.addr != 0) {
                // Tenemos IP
                esp_ip4_addr_t ip = ip_info.ip;
                snprintf(sta_ip, max_len, IPSTR, IP2STR(&ip));
                ESP_LOGI(TAG, "STA IP obtenida: %s", sta_ip);
                return true;
            }
        }

        // Verificar timeout
        int64_t elapsed = (esp_timer_get_time() / 1000) - start_time;
        if (elapsed >= timeout_ms) {
            ESP_LOGW(TAG, "Timeout esperando IP STA");
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static esp_err_t api_connect_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "API connect handler iniciado");

    // Leer body
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // Parsear JSON
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid_json = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_json = cJSON_GetObjectItem(root, "password");

    if (ssid_json == NULL || pass_json == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid or password");
        return ESP_FAIL;
    }

    const char *ssid = cJSON_GetStringValue(ssid_json);
    const char *password = cJSON_GetStringValue(pass_json);

    ESP_LOGI(TAG, "Conectando a WiFi: %s", ssid);

    // Iniciar conexión WiFi (STA)
    ret = wifi_provisioner_connect(ssid, password, NULL, NULL);
    cJSON_Delete(root);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando conexión WiFi");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Failed to start connection\"}");
        return ESP_OK;
    }

    // Esperar a obtener IP en STA (máx 30s)
    char sta_ip[16];
    if (!wait_for_sta_ip(sta_ip, sizeof(sta_ip), 30000)) {
        ESP_LOGE(TAG, "Timeout esperando IP STA");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Timeout getting IP\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "WiFi conectado, IP: %s", sta_ip);

    // Marcar que se debe obtener el link_code en segundo plano
    s_link_code_pending = true;
    s_link_code_ready = false;

    // Construir respuesta JSON en un solo buffer
    char response[128];
    snprintf(response, sizeof(response), "{\"success\":true,\"ip\":\"%s\"}", sta_ip);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response);

    ESP_LOGI(TAG, "Conexión WiFi exitosa, obteniendo link_code en segundo plano...");

    return ESP_OK;
}

static esp_err_t api_status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    wifi_state_t wifi_state = wifi_manager_get_state();
    bool connected = (wifi_state == WIFI_STATE_CONNECTED);

    char ip[16] = "";
    if (connected) {
        wifi_manager_get_ip(ip);
    }

    char response[128];
    snprintf(response, sizeof(response),
             "{\"connected\":%s,\"ip\":\"%s\"}",
             connected ? "true" : "false", ip);

    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

static esp_err_t api_device_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    char device_id[DEVICE_ID_LEN];
    char pairing_token[PAIRING_TOKEN_LEN];
    char pairing_url[256];

    device_identity_get_id(device_id);
    device_identity_get_pairing_token(pairing_token);
    device_identity_get_pairing_url(pairing_url, sizeof(pairing_url));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "pairing_token", pairing_token);
    cJSON_AddStringToObject(root, "pairing_url", pairing_url);

    char *response = cJSON_Print(root);
    httpd_resp_sendstr(req, response);

    free(response);
    cJSON_Delete(root);

    return ESP_OK;
}

/**
 * @brief Handler para obtener el link_code (polling)
 */
static esp_err_t api_link_code_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    if (s_link_code_ready) {
        char response[64];
        snprintf(response, sizeof(response), "{\"ready\":true,\"code\":\"%s\"}", s_link_code);
        httpd_resp_sendstr(req, response);
    } else if (s_link_code_pending) {
        httpd_resp_sendstr(req, "{\"ready\":false}");
    } else {
        httpd_resp_sendstr(req, "{\"ready\":false,\"error\":\"No pending request\"}");
    }

    return ESP_OK;
}

// ============================================================================
// Handlers HTTP
// ============================================================================

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, index_html);
    return ESP_OK;
}

// ============================================================================
// Funciones públicas
// ============================================================================

esp_err_t http_server_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    // Registrar handler raíz
    httpd_uri_t uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri);

    // Registrar handler /continue (mismo HTML que /)
    uri.uri = "/continue";
    uri.method = HTTP_GET;
    uri.handler = index_handler;
    uri.user_ctx = NULL;
    httpd_register_uri_handler(s_server, &uri);

    // Crear timer para obtener link_code en segundo plano
    if (s_link_code_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = &link_code_timer_callback,
            .name = "link_code_timer"
        };
        esp_timer_create(&timer_args, &s_link_code_timer);
    }

    // Iniciar timer periódico (cada 2 segundos)
    esp_timer_start_periodic(s_link_code_timer, 2000000); // 2 segundos en microsegundos

    // Registrar API handlers
    uri.uri = "/api/scan";
    uri.method = HTTP_GET;
    uri.handler = api_scan_handler;
    uri.user_ctx = NULL;
    httpd_register_uri_handler(s_server, &uri);

    uri.uri = "/api/connect";
    uri.method = HTTP_POST;
    uri.handler = api_connect_handler;
    uri.user_ctx = NULL;
    httpd_register_uri_handler(s_server, &uri);

    uri.uri = "/api/status";
    uri.method = HTTP_GET;
    uri.handler = api_status_handler;
    uri.user_ctx = NULL;
    httpd_register_uri_handler(s_server, &uri);

    uri.uri = "/api/device";
    uri.method = HTTP_GET;
    uri.handler = api_device_handler;
    uri.user_ctx = NULL;
    httpd_register_uri_handler(s_server, &uri);

    uri.uri = "/api/link-code";
    uri.method = HTTP_GET;
    uri.handler = api_link_code_handler;
    uri.user_ctx = NULL;
    httpd_register_uri_handler(s_server, &uri);

    ESP_LOGI(TAG, "✅ HTTP server iniciado (puerto 80)");
    return ESP_OK;
}

esp_err_t http_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    // Detener timer
    if (s_link_code_timer != NULL) {
        esp_timer_stop(s_link_code_timer);
    }

    esp_err_t ret = httpd_stop(s_server);
    s_server = NULL;

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "HTTP server detenido");
    }

    return ret;
}
