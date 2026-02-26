/**
 * @file controller.h
 * @brief Controlador principal del sistema de seguridad Gateway
 * 
 * Este componente es el "cerebro" del sistema que procesa eventos de sensores,
 * gestiona el estado del sistema y coordina los demás componentes.
 */

#ifndef CONTROLLER_H
#define CONTROLLER_H

#include "system_globals.h"

// ============================================================================
// Inicialización
// ============================================================================

/**
 * @brief Inicializa el controlador del sistema
 * 
 * Crea la cola de mensajes, inicializa el estado del sistema desde NVS,
 * y crea la tarea del controlador.
 * 
 * @return ESP_OK si la inicialización fue exitosa
 */
esp_err_t controller_init(void);

/**
 * @brief Detiene el controlador
 * 
 * @return ESP_OK si se detuvo correctamente
 */
esp_err_t controller_deinit(void);

// ============================================================================
// Control de estado del sistema
// ============================================================================

/**
 * @brief Obtiene el estado actual del sistema
 * 
 * @return Estado actual del sistema
 */
system_state_t controller_get_state(void);

/**
 * @brief Establece el estado del sistema
 * 
 * @param state Nuevo estado del sistema
 * @return ESP_OK si el cambio fue exitoso
 */
esp_err_t controller_set_state(system_state_t state);

/**
 * @brief Arma el sistema
 * 
 * @return ESP_OK si el armado fue exitoso
 * @return ESP_ERR_INVALID_STATE si el sistema ya está armado
 */
esp_err_t controller_arm(void);

/**
 * @brief Desarma el sistema
 * 
 * @return ESP_OK si el desarmado fue exitoso
 */
esp_err_t controller_disarm(void);

/**
 * @brief Activa la alarma
 * 
 * @return ESP_OK si la activación fue exitosa
 */
esp_err_t controller_trigger_alarm(void);

/**
 * @brief Desactiva la alarma
 * 
 * @return ESP_OK si la desactivación fue exitosa
 */
esp_err_t controller_clear_alarm(void);

// ============================================================================
// Gestión de sensores
// ============================================================================

/**
 * @brief Procesa un evento de sensor recibido
 * 
 * @param message Mensaje recibido del sensor
 * @return ESP_OK si el procesamiento fue exitoso
 */
esp_err_t controller_process_sensor_event(const controller_message_t *message);

/**
 * @brief Actualiza el estado de un sensor
 * 
 * @param device_id ID del sensor
 * @param state Nuevo estado (0=cerrado, 1=abierto)
 * @return ESP_OK si la actualización fue exitosa
 */
esp_err_t controller_update_sensor_state(const char *device_id, uint8_t state);

// ============================================================================
// Configuración de boot
// ============================================================================

/**
 * @brief Obtiene el modo de arranque configurado
 * 
 * @return Modo de arranque actual
 */
boot_mode_t controller_get_boot_mode(void);

/**
 * @brief Establece el modo de arranque
 * 
 * @param mode Nuevo modo de arranque
 * @return ESP_OK si el cambio fue exitoso
 */
esp_err_t controller_set_boot_mode(boot_mode_t mode);

// ============================================================================
// Tarea del controlador
// ============================================================================

/**
 * @brief Tarea principal del controlador
 * 
 * Esta tarea se ejecuta continuamente, procesando mensajes de la cola
 * y actualizando el estado del sistema.
 * 
 * @param pvParameters Parámetros de la tarea (no usado)
 */
void controller_task(void *pvParameters);

// ============================================================================
// Utilidades
// ============================================================================

/**
 * @brief Imprime el estado actual del sistema (para debug)
 */
void controller_print_state(void);

/**
 * @brief Obtiene el contexto del sistema
 * 
 * @return Puntero al contexto global del sistema
 */
system_context_t* controller_get_context(void);

#endif // CONTROLLER_H
