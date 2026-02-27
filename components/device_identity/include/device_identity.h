/**
 * @file device_identity.h
 * @brief Identidad y tokens de vinculación para Ghost Gateway
 *
 * Gestiona la identidad única del dispositivo:
 * - device_id único desde MAC address
 * - pairing_token para vinculación con usuario
 * - Almacenamiento en NVS
 */

#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Longitud del device_id (GHOST-XXXXXX + null) */
#define DEVICE_ID_LEN        16

/** @brief Longitud del pairing_token */
#define PAIRING_TOKEN_LEN    64

/** @brief Longitud del user_id de Supabase */
#define USER_ID_LEN          64

/**
 * @brief Estructura de identidad del dispositivo
 */
typedef struct {
    char device_id[DEVICE_ID_LEN];      /**< ID único: GHOST-XXXXXX */
    char pairing_token[PAIRING_TOKEN_LEN]; /**< Token para vinculación */
    char user_id[USER_ID_LEN];          /**< UID de Supabase (si vinculado) */
    int64_t created_at;                 /**< Timestamp de creación */
    int64_t expires_at;                 /**< Expiración del token (24h) */
    bool provisioned;                   /**< WiFi configurado */
    bool linked;                        /**< Vinculado a usuario */
} device_identity_t;

/**
 * @brief Inicializa el módulo de identidad
 *
 * Genera o recupera el device_id desde NVS.
 * Crea un nuevo pairing_token si no existe o expiró.
 *
 * @return ESP_OK si la inicialización fue exitosa
 */
esp_err_t device_identity_init(void);

/**
 * @brief Obtiene la identidad del dispositivo
 *
 * @param[out] identity Puntero a estructura donde se copiará la identidad
 * @return ESP_OK si se obtuvo correctamente
 * @return ESP_ERR_INVALID_ARG si identity es NULL
 */
esp_err_t device_identity_get(device_identity_t *identity);

/**
 * @brief Obtiene el device_id (string)
 *
 * @param[out] device_id Buffer donde se escribirá el device_id (mínimo DEVICE_ID_LEN)
 * @return ESP_OK si se obtuvo correctamente
 */
esp_err_t device_identity_get_id(char *device_id);

/**
 * @brief Obtiene el pairing_token actual
 *
 * @param[out] token Buffer donde se escribirá el token (mínimo PAIRING_TOKEN_LEN)
 * @return ESP_OK si se obtuvo correctamente
 */
esp_err_t device_identity_get_pairing_token(char *token);

/**
 * @brief Genera un nuevo pairing_token
 *
 * Invalida el token anterior y genera uno nuevo con 24h de validez.
 *
 * @return ESP_OK si se generó correctamente
 */
esp_err_t device_identity_refresh_pairing_token(void);

/**
 * @brief Marca el dispositivo como provisionado (WiFi configurado)
 *
 * @return ESP_OK si se marcó correctamente
 */
esp_err_t device_identity_set_provisioned(void);

/**
 * @brief Verifica si el dispositivo está provisionado
 *
 * @return true si tiene credenciales WiFi guardadas
 * @return false si no está provisionado
 */
bool device_identity_is_provisioned(void);

/**
 * @brief Vincula el dispositivo a un usuario
 *
 * @param user_id UID del usuario de Supabase
 * @return ESP_OK si se vinculó correctamente
 */
esp_err_t device_identity_link_user(const char *user_id);

/**
 * @brief Verifica si el dispositivo está vinculado a un usuario
 *
 * @return true si está vinculado
 * @return false si no está vinculado
 */
bool device_identity_is_linked(void);

/**
 * @brief Obtiene el user_id vinculado
 *
 * @param[out] user_id Buffer donde se escribirá el user_id (mínimo USER_ID_LEN)
 * @return ESP_OK si hay usuario vinculado
 * @return ESP_ERR_NOT_FOUND si no está vinculado
 */
esp_err_t device_identity_get_user_id(char *user_id);

/**
 * @brief Resetea la identidad (borra NVS)
 *
 * Útil para testing o fábrica. Regenera device_id y token.
 *
 * @return ESP_OK si se reseteó correctamente
 */
esp_err_t device_identity_reset(void);

/**
 * @brief Genera URL de vinculación con QR code
 *
 * Formato: ghost://link?device=GHOST-ABC123&token=xyz...
 *
 * @param[out] url Buffer donde se escribirá la URL (mínimo 256 bytes)
 * @param url_len Tamaño del buffer
 * @return ESP_OK si se generó correctamente
 */
esp_err_t device_identity_get_pairing_url(char *url, size_t url_len);

#ifdef __cplusplus
}
#endif

#endif // DEVICE_IDENTITY_H
