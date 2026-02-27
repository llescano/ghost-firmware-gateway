/**
 * @file http_server.c
 * @brief Servidor HTTP con portal de configuración
 *
 * Implementa un servidor HTTP ligero con páginas HTML inline.
 */

#include "http_server.h"
#include "wifi_provisioner.h"
#include "device_identity.h"

#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include <string.h>

static const char *TAG = "http_server";

static httpd_handle_t s_server = NULL;

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

    <!-- Página 1: Scan de redes -->
    "<div id='page-scan'>"
    "<div class='network-list' id='network-list'>"
    "<div class='status connecting'><div class='spinner'></div><p style='margin-top:8px'>Escaneando redes...</p></div>"
    "</div>"
    "</div>"

    <!-- Página 2: Input de contraseña -->
    "<div id='page-password' class='hidden'>"
    "<div class='status' id='selected-network'></div>"
    "<div class='input-group'>"
    "<label>Contraseña de WiFi</label>"
    "<input type='password' id='password' placeholder='Ingresa la contraseña'>"
    "</div>"
    "<button class='btn' id='btn-connect'>Conectar</button>"
    "<button class='btn btn-secondary' style='margin-top:8px' id='btn-back-scan'>Volver</button>"
    "</div>"

    <!-- Página 3: Conectando -->
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

    <!-- Página 4: Éxito + QR -->
    "<div id='page-success' class='hidden'>"
    "<div class='status success'>✅ Conectado exitosamente</div>"
    "<p style='text-align:center;color:#999;margin-bottom:16px'>Escanea el QR para vincular este dispositivo con tu cuenta</p>"
    "<div class='qr-container'>"
    "<div class='qr-placeholder' id='qr-code'>"
    "<!-- QR code se generará aquí -->"
    "</div>"
    "</div>"
    "<p class='pairing-code' id='pairing-url'></p>"
    "<p style='text-align:center;color:#666;font-size:12px;margin-top:16px'>Este código expira en 24 horas</p>"
    "</div>"

    <!-- Página 5: Error -->
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
    "function renderNetworks(networks){const list=document.getElementById('network-list');list.innerHTML='';networks.forEach(n=>{const item=document.createElement('div');item.className='network-item';item.innerHTML=getSignalIcon(n.rssi)+`<div class='network-info'><div class='network-name'>${escapeHtml(n.ssid)}</div><div class='network-strength'>${getStrengthText(n.rssi)}</div></div>`+(n.authmode!==0?'<svg class='network-lock' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'><rect x='3' y='11' width='18' height='11' rx='2' ry='2'></rect><path d='M7 11V7a5 5 0 0 1 10 0v4'></path></svg>':'');item.onclick=()=>selectNetwork(n);list.appendChild(item);});}"
    "function getSignalIcon(rssi){const s=rssi>-60?'excellent':rssi>-70?'good':rssi>-80?'fair':'poor';return `<svg class='wifi-icon' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'><path d='M5 12.55a11 11 0 0 1 14.08 0'></path><path d='M1.42 9a16 16 0 0 1 17.16 0'></path><path d='M8.53 16.11a6 6 0 0 1 6.95 0'></path><line x1='12' y1='20' x2='12.01' y2='20'></line></svg>`;}"
    "function getStrengthText(rssi){return rssi>-60?'Excelente':rssi>-70?'Buena':rssi>-80?'Regular':'Débil';}"
    "function escapeHtml(t){const d=document.createElement('div');d.textContent=t;return d.innerHTML;}"

    // Selección de red
    "function selectNetwork(n){selectedNetwork=n;document.getElementById('selected-network').innerHTML=`<strong>${escapeHtml(n.ssid)}</strong><br><span style='font-size:12px;color:#666'>${getStrengthText(n.rssi)}</span>`;showPage('page-password');document.getElementById('password').focus();}"

    // Conexión
    "async function connect(){const p=document.getElementById('password').value;if(!p){alert('Ingresa la contraseña');return;}"
    "showPage('page-connecting');setStep('step-scan','done');setStep('step-connect','pending');setStep('step-ip','pending');"
    "try{const res=await fetch(apiBase+'/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:selectedNetwork.ssid,password:p})});const data=await res.json();"
    "if(data.success){setStep('step-connect','done');pollStatus();}else{showError(data.error||'No se pudo conectar');}}catch(e){console.error(e);showError('Error de conexión');}}"

    // Poll de estado
    "async function pollStatus(){try{const res=await fetch(apiBase+'/status');const data=await res.json();if(data.connected){setStep('step-ip','done');setTimeout(showSuccess,500);}else{setTimeout(pollStatus,1000);}}catch(e){setTimeout(pollStatus,1000);}}"

    // Éxito con QR
    "async function showSuccess(){showPage('page-success');try{const res=await fetch(apiBase+'/device');const data=await res.json();document.getElementById('pairing-url').textContent=data.pairing_url||data.device_id;generateQR(data.pairing_url||data.device_id);}catch(e){console.error(e);}}"
    "function generateQR(text){const qr=document.getElementById('qr-code');qr.innerHTML=`<img class='qr-code' src='https://api.qrserver.com/v1/create-qr-code/?size=200x200&data=${encodeURIComponent(text)}' alt='QR Code'>`;}"

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

static esp_err_t api_connect_handler(httpd_req_t *req)
{
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

    // Iniciar conexión
    ret = wifi_provisioner_connect(ssid, password, NULL, NULL);

    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (ret == ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":true}");
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Failed to connect\"}");
    }

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

// ============================================================================
// Handlers HTTP
// ============================================================================

static httpd_uri_t handlers[] = {
    { .uri = "/",                .method = HTTP_GET,    .handler = NULL, .user_ctx = (void *)index_html },
    { .uri = "/api/scan",        .method = HTTP_GET,    .handler = api_scan_handler },
    { .uri = "/api/connect",     .method = HTTP_POST,   .handler = api_connect_handler },
    { .uri = "/api/status",      .method = HTTP_GET,    .handler = api_status_handler },
    { .uri = "/api/device",      .method = HTTP_GET,    .handler = api_device_handler },
};

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

    // Registrar API handlers
    const char *uris[] = { "/api/scan", "/api/connect", "/api/status", "/api/device" };
    httpd_req_handler_t funcs[] = { api_scan_handler, api_connect_handler, api_status_handler, api_device_handler };
    httpd_method_t methods[] = { HTTP_GET, HTTP_POST, HTTP_GET, HTTP_GET };

    for (int i = 0; i < 4; i++) {
        uri.uri = uris[i];
        uri.method = methods[i];
        uri.handler = funcs[i];
        httpd_register_uri_handler(s_server, &uri);
    }

    ESP_LOGI(TAG, "✅ HTTP server iniciado (puerto 80)");
    return ESP_OK;
}

esp_err_t http_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    esp_err_t ret = httpd_stop(s_server);
    s_server = NULL;

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "HTTP server detenido");
    }

    return ret;
}
