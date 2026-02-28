/**
 * @file device_identity.c
 * @brief Implementación de identidad y tokens de vinculación
 */

#include "device_identity.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <time.h>

static const char *TAG = "device_identity";

// Namespace NVS para configuración del dispositivo
#define NVS_NAMESPACE        "ghost_cfg"

// Claves NVS
#define NVS_KEY_DEVICE_ID    "device_id"
#define NVS_KEY_PAIRING_TK   "pairing_token"
#define NVS_KEY_USER_ID      "user_id"
#define NVS_KEY_CREATED_AT   "created_at"
#define NVS_KEY_EXPIRES_AT   "expires_at"
#define NVS_KEY_PROVISIONED  "provisioned"
#define NVS_KEY_LINKED       "linked"

// Validez del pairing_token: 24 horas
#define PAIRING_TOKEN_VALIDITY_SEC  (24 * 60 * 60)

// Estructura interna caché
typedef struct {
    device_identity_t identity;
    bool loaded;
} identity_cache_t;

static identity_cache_t s_cache = {
    .loaded = false
};

/**
 * @brief Genera device_id desde MAC address
 *
 * Formato: GHOST-XXXXXX donde XXXXXX son los últimos 3 bytes de la MAC
 */
static esp_err_t generate_device_id(char *device_id, size_t len)
{
    if (len < DEVICE_ID_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac[6];
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error leyendo MAC address: %s", esp_err_to_name(ret));
        return ret;
    }

    // Formato: GHOST-XXXXXX
    snprintf(device_id, len, "GHOST-%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "Device ID generado: %s", device_id);
    return ESP_OK;
}

/**
 * @brief Genera un pairing_token aleatorio
 *
 * Formato: Base64 URL-safe de 32 bytes aleatorios
 */
static esp_err_t generate_pairing_token(char *token, size_t len)
{
    if (len < PAIRING_TOKEN_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    // Caracteres Base64 URL-safe
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    // Generar 48 bytes aleatorios → 64 caracteres en Base64
    uint8_t random_bytes[48];
    esp_fill_random(random_bytes, sizeof(random_bytes));

    for (int i = 0; i < PAIRING_TOKEN_LEN - 1; i++) {
        token[i] = charset[random_bytes[i] % sizeof(charset)];
    }
    token[PAIRING_TOKEN_LEN - 1] = '\0';

    ESP_LOGI(TAG, "Pairing token generado: %s", token);
    return ESP_OK;
}

/**
 * @brief Carga identidad desde NVS al caché
 */
static esp_err_t load_identity_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        // Namespace no existe, aún no inicializado
        ESP_LOGW(TAG, "Namespace NVS no encontrado, dispositivo nuevo");
        return ESP_ERR_NOT_FOUND;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error abriendo NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    // Leer device_id
    size_t len = DEVICE_ID_LEN;
    ret = nvs_get_str(handle, NVS_KEY_DEVICE_ID, s_cache.identity.device_id, &len);
    if (ret != ESP_OK) {
        nvs_close(handle);
        ESP_LOGW(TAG, "Device ID no encontrado en NVS");
        return ret;
    }

    // Leer pairing_token
    len = PAIRING_TOKEN_LEN;
    ret = nvs_get_str(handle, NVS_KEY_PAIRING_TK, s_cache.identity.pairing_token, &len);
    if (ret != ESP_OK) {
        nvs_close(handle);
        ESP_LOGW(TAG, "Pairing token no encontrado en NVS");
        return ret;
    }

    // Leer user_id (opcional)
    len = USER_ID_LEN;
    if (nvs_get_str(handle, NVS_KEY_USER_ID, s_cache.identity.user_id, &len) == ESP_OK) {
        s_cache.identity.linked = true;
    } else {
        s_cache.identity.linked = false;
        s_cache.identity.user_id[0] = '\0';
    }

    // Leer timestamps
    s_cache.identity.created_at = 0;
    s_cache.identity.expires_at = 0;
    nvs_get_i64(handle, NVS_KEY_CREATED_AT, &s_cache.identity.created_at);
    nvs_get_i64(handle, NVS_KEY_EXPIRES_AT, &s_cache.identity.expires_at);

    // Leer flags
    uint8_t provisioned = 0;
    nvs_get_u8(handle, NVS_KEY_PROVISIONED, &provisioned);
    s_cache.identity.provisioned = (provisioned != 0);

    uint8_t linked = 0;
    nvs_get_u8(handle, NVS_KEY_LINKED, &linked);
    s_cache.identity.linked = (linked != 0);

    nvs_close(handle);
    s_cache.loaded = true;

    ESP_LOGI(TAG, "Identidad cargada: %s (provisioned=%d, linked=%d)",
             s_cache.identity.device_id,
             s_cache.identity.provisioned,
             s_cache.identity.linked);

    return ESP_OK;
}

/**
 * @brief Guarda identidad al NVS
 */
static esp_err_t save_identity_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error abriendo NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    // Guardar device_id
    ret = nvs_set_str(handle, NVS_KEY_DEVICE_ID, s_cache.identity.device_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error guardando device_id: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    // Guardar pairing_token
    ret = nvs_set_str(handle, NVS_KEY_PAIRING_TK, s_cache.identity.pairing_token);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error guardando pairing_token: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    // Guardar user_id si existe
    if (s_cache.identity.linked && strlen(s_cache.identity.user_id) > 0) {
        nvs_set_str(handle, NVS_KEY_USER_ID, s_cache.identity.user_id);
    }

    // Guardar timestamps
    nvs_set_i64(handle, NVS_KEY_CREATED_AT, s_cache.identity.created_at);
    nvs_set_i64(handle, NVS_KEY_EXPIRES_AT, s_cache.identity.expires_at);

    // Guardar flags
    nvs_set_u8(handle, NVS_KEY_PROVISIONED, s_cache.identity.provisioned ? 1 : 0);
    nvs_set_u8(handle, NVS_KEY_LINKED, s_cache.identity.linked ? 1 : 0);

    ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

// ============================================================================
// Funciones Públicas
// ============================================================================

esp_err_t device_identity_init(void)
{
    esp_err_t ret;

    // Intentar cargar desde NVS
    ret = load_identity_from_nvs();

    if (ret == ESP_OK) {
        // Verificar si el pairing_token expiró
        time_t now = 0;
        // TODO: Obtener timestamp real desde SNTP
        // Por ahora, asumimos que no expira si no tenemos tiempo
        if (s_cache.identity.expires_at > 0) {
            ESP_LOGI(TAG, "Identidad cargada desde NVS");
        } else {
            // Sin tiempo, generar nuevo token
            device_identity_refresh_pairing_token();
        }
    } else {
        // Dispositivo nuevo, generar identidad
        ESP_LOGI(TAG, "Dispositivo nuevo, generando identidad...");

        // Generar device_id
        ret = generate_device_id(s_cache.identity.device_id, DEVICE_ID_LEN);
        if (ret != ESP_OK) {
            return ret;
        }

        // Generar pairing_token
        ret = generate_pairing_token(s_cache.identity.pairing_token, PAIRING_TOKEN_LEN);
        if (ret != ESP_OK) {
            return ret;
        }

        // Inicializar campos
        s_cache.identity.created_at = 0; // TODO: SNTP
        s_cache.identity.expires_at = 0; // TODO: +24h
        s_cache.identity.provisioned = false;
        s_cache.identity.linked = false;
        s_cache.identity.user_id[0] = '\0';

        // Guardar en NVS
        ret = save_identity_to_nvs();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Error guardando identidad en NVS");
            return ret;
        }

        s_cache.loaded = true;
        ESP_LOGI(TAG, "✅ Identidad nueva creada: %s", s_cache.identity.device_id);
    }

    return ESP_OK;
}

esp_err_t device_identity_get(device_identity_t *identity)
{
    if (!s_cache.loaded) {
        return ESP_ERR_INVALID_STATE;
    }

    if (identity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(identity, &s_cache.identity, sizeof(device_identity_t));
    return ESP_OK;
}

esp_err_t device_identity_get_id(char *device_id)
{
    if (!s_cache.loaded) {
        return ESP_ERR_INVALID_STATE;
    }

    if (device_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(device_id, s_cache.identity.device_id, DEVICE_ID_LEN);
    return ESP_OK;
}

esp_err_t device_identity_get_pairing_token(char *token)
{
    if (!s_cache.loaded) {
        return ESP_ERR_INVALID_STATE;
    }

    if (token == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(token, s_cache.identity.pairing_token, PAIRING_TOKEN_LEN);
    return ESP_OK;
}

esp_err_t device_identity_refresh_pairing_token(void)
{
    esp_err_t ret;

    ret = generate_pairing_token(s_cache.identity.pairing_token, PAIRING_TOKEN_LEN);
    if (ret != ESP_OK) {
        return ret;
    }

    // TODO: Actualizar timestamps cuando tengamos SNTP
    // s_cache.identity.created_at = time(NULL);
    // s_cache.identity.expires_at = s_cache.identity.created_at + PAIRING_TOKEN_VALIDITY_SEC;

    return save_identity_to_nvs();
}

esp_err_t device_identity_set_provisioned(void)
{
    if (!s_cache.loaded) {
        return ESP_ERR_INVALID_STATE;
    }

    s_cache.identity.provisioned = true;
    ESP_LOGI(TAG, "Dispositivo marcado como provisionado");
    return save_identity_to_nvs();
}

bool device_identity_is_provisioned(void)
{
    return s_cache.loaded && s_cache.identity.provisioned;
}

esp_err_t device_identity_link_user(const char *user_id)
{
    if (!s_cache.loaded) {
        return ESP_ERR_INVALID_STATE;
    }

    if (user_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_cache.identity.user_id, user_id, USER_ID_LEN - 1);
    s_cache.identity.user_id[USER_ID_LEN - 1] = '\0';
    s_cache.identity.linked = true;

    ESP_LOGI(TAG, "Dispositivo vinculado a usuario: %s", user_id);
    return save_identity_to_nvs();
}

bool device_identity_is_linked(void)
{
    return s_cache.loaded && s_cache.identity.linked;
}

esp_err_t device_identity_get_user_id(char *user_id)
{
    if (!s_cache.loaded || !s_cache.identity.linked) {
        return ESP_ERR_NOT_FOUND;
    }

    if (user_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(user_id, s_cache.identity.user_id, USER_ID_LEN);
    return ESP_OK;
}

esp_err_t device_identity_reset(void)
{
    nvs_handle_t handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    // Borrar todas las claves
    nvs_erase_key(handle, NVS_KEY_DEVICE_ID);
    nvs_erase_key(handle, NVS_KEY_PAIRING_TK);
    nvs_erase_key(handle, NVS_KEY_USER_ID);
    nvs_erase_key(handle, NVS_KEY_CREATED_AT);
    nvs_erase_key(handle, NVS_KEY_EXPIRES_AT);
    nvs_erase_key(handle, NVS_KEY_PROVISIONED);
    nvs_erase_key(handle, NVS_KEY_LINKED);

    ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret == ESP_OK) {
        // Reset caché
        memset(&s_cache.identity, 0, sizeof(device_identity_t));
        s_cache.loaded = false;
        ESP_LOGI(TAG, "Identidad reseteada, reinstalando...");
        return device_identity_init();
    }

    return ret;
}

esp_err_t device_identity_get_pairing_url(char *url, size_t url_len)
{
    if (!s_cache.loaded) {
        return ESP_ERR_INVALID_STATE;
    }

    if (url == NULL || url_len < 256) {
        return ESP_ERR_INVALID_ARG;
    }

    // Formato: ghost://link?device=GHOST-ABC123&token=xyz...
    snprintf(url, url_len,
             "ghost://link?device=%s&token=%s",
             s_cache.identity.device_id,
             s_cache.identity.pairing_token);

    return ESP_OK;
}
