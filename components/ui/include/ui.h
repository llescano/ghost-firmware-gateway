/**
 * @file ui.h
 * @brief Componente de interfaz de usuario (LED y botón) para el Gateway
 * 
 * Maneja el LED RGB WS2812 para indicar el estado del sistema y el botón BOOT
 * para armado/desarmado manual.
 */

#ifndef UI_H
#define UI_H

#include "system_globals.h"

// ============================================================================
// Definiciones de estados LED
// ============================================================================

/** @brief Indicadores LED personalizados para el sistema de seguridad */
typedef enum {
    LED_SYS_DISARMED = 0,     /**< Sistema desarmado - Verde sólido */
    LED_SYS_ARMED,            /**< Sistema armado - Rojo sólido */
    LED_SYS_ALARM,            /**< Alarma activa - Rojo parpadeando rápido */
    LED_SYS_TAMPER,           /**< Tamper detectado - Amarillo parpadeando */
    LED_SYS_BOOT,             /**< Inicializando - Azul parpadeando */
    LED_SYS_ERROR,            /**< Error - Rojo/Verde alternando */
    LED_SYS_PROVISIONING,     /**< Modo provisioning - Azul fijo */
    LED_SYS_OFFLINE,          /**< Offline - Naranja parpadeando lento */
    LED_SYS_UNCONFIGURED,     /**< Sin configurar - Rojo parpadeando rápido */
} led_system_state_t;

// ============================================================================
// Tipos de callback para botón
// ============================================================================

/**
 * @brief Tipo de callback para click simple del botón
 * @note Se llama cuando se detecta un click simple en el botón BOOT
 */
typedef void (*ui_button_click_cb_t)(void);

/**
 * @brief Tipo de callback para long press del botón
 * @note Se llama cuando se detecta una pulsación larga (~5s)
 */
typedef void (*ui_button_long_press_cb_t)(void);

/**
 * @brief Tipo de callback para factory reset del botón
 * @note Se llama cuando se detecta una pulsación muy larga (~10s)
 */
typedef void (*ui_button_factory_reset_cb_t)(void);

// ============================================================================
// Inicialización
// ============================================================================

/**
 * @brief Inicializa el módulo de UI (LED y botón)
 * 
 * Configura el LED WS2812 y el botón BOOT.
 * 
 * @return ESP_OK si la inicialización fue exitosa
 */
esp_err_t ui_init(void);

/**
 * @brief Detiene el módulo de UI
 * 
 * @return ESP_OK si se detuvo correctamente
 */
esp_err_t ui_deinit(void);

// ============================================================================
// Configuración de callbacks del botón
// ============================================================================

/**
 * @brief Configura el callback para click simple del botón BOOT
 * 
 * @param callback Función a llamar cuando se detecta un click simple
 */
void ui_set_button_click_callback(ui_button_click_cb_t callback);

/**
 * @brief Configura el callback para long press del botón BOOT
 *
 * @param callback Función a llamar cuando se detecta una pulsación larga (~5s)
 */
void ui_set_button_long_press_callback(ui_button_long_press_cb_t callback);

/**
 * @brief Configura el callback para factory reset del botón BOOT
 *
 * @param callback Función a llamar cuando se detecta una pulsación muy larga (~10s)
 */
void ui_set_button_factory_reset_callback(ui_button_factory_reset_cb_t callback);

// ============================================================================
// Control del LED
// ============================================================================

/**
 * @brief Establece el estado del LED según el estado del sistema
 * 
 * @param state Estado del sistema a indicar
 * @return ESP_OK si el cambio fue exitoso
 */
esp_err_t ui_set_system_state(system_state_t state);

/**
 * @brief Establece un estado personalizado del LED
 * 
 * @param state Estado LED personalizado
 * @return ESP_OK si el cambio fue exitoso
 */
esp_err_t ui_set_led_state(led_system_state_t state);

/**
 * @brief Hace parpadear el LED un número de veces
 * 
 * @param color Color del parpadeo (0=rojo, 1=verde, 2=azul, 3=amarillo)
 * @param times Número de parpadeos
 */
void ui_blink(uint8_t color, uint8_t times);

/**
 * @brief Enciende el LED en un color específico
 * 
 * @param r Componente rojo (0-255)
 * @param g Componente verde (0-255)
 * @param b Componente azul (0-255)
 */
void ui_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Apaga el LED
 */
void ui_led_off(void);

// ============================================================================
// Tarea de UI
// ============================================================================

/**
 * @brief Tarea de actualización de UI
 * 
 * Monitorea el estado del sistema y actualiza el LED en consecuencia.
 * 
 * @param pvParameters Parámetros de la tarea (no usado)
 */
void ui_task(void *pvParameters);

#endif // UI_H
