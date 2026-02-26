/**
 * @file comm.h
 * @brief Componente de comunicación ESP-Now para el Gateway
 * 
 * Maneja la comunicación inalámbrica con sensores y otros dispositivos
 * utilizando el protocolo ESP-Now.
 * 
 * @note Diseñado para migración futura a Mesh Lite con #define USE_MESH_LITE
 */

#ifndef COMM_H
#define COMM_H

#include "system_globals.h"
#include "esp_now.h"  // Para esp_now_recv_info_t y esp_now_send_status_t

// ============================================================================
// Inicialización y configuración
// ============================================================================

/**
 * @brief Inicializa el módulo de comunicación ESP-Now
 * 
 * Configura WiFi en modo estación, inicializa ESP-Now y registra
 * los callbacks de recepción y envío.
 * 
 * @return ESP_OK si la inicialización fue exitosa
 * @return ESP_ERR_* en caso de error
 */
esp_err_t comm_init(void);

/**
 * @brief Detiene el módulo de comunicación
 * 
 * @return ESP_OK si se detuvo correctamente
 */
esp_err_t comm_deinit(void);

// ============================================================================
// Envío de mensajes
// ============================================================================

/**
 * @brief Envía un mensaje a un dispositivo específico
 * 
 * @param dest_mac Dirección MAC del dispositivo destino (NULL para broadcast)
 * @param message Puntero al mensaje a enviar
 * @return ESP_OK si el mensaje fue enviado correctamente
 * @return ESP_ERR_* en caso de error
 */
esp_err_t comm_send_message(const uint8_t *dest_mac, const controller_message_t *message);

/**
 * @brief Envía un mensaje de broadcast a todos los sensores
 * 
 * @param message Puntero al mensaje a enviar
 * @return ESP_OK si el mensaje fue enviado correctamente
 */
esp_err_t comm_broadcast_message(const controller_message_t *message);

// ============================================================================
// Registro de sensores
// ============================================================================

/**
 * @brief Registra un nuevo sensor en la lista de dispositivos conocidos
 * 
 * @param mac_addr Dirección MAC del sensor
 * @param device_id ID único del sensor
 * @param type Tipo de dispositivo
 * @return ESP_OK si el registro fue exitoso
 */
esp_err_t comm_register_sensor(const uint8_t *mac_addr, const char *device_id, device_type_t type);

/**
 * @brief Elimina un sensor de la lista de dispositivos conocidos
 * 
 * @param device_id ID del sensor a eliminar
 * @return ESP_OK si la eliminación fue exitosa
 */
esp_err_t comm_unregister_sensor(const char *device_id);

/**
 * @brief Obtiene la información de un sensor registrado
 * 
 * @param device_id ID del sensor a buscar
 * @param sensor_info Puntero donde se almacenará la información
 * @return ESP_OK si el sensor fue encontrado
 * @return ESP_ERR_NOT_FOUND si el sensor no está registrado
 */
esp_err_t comm_get_sensor_info(const char *device_id, sensor_info_t *sensor_info);

// ============================================================================
// Callbacks (llamados desde ISR)
// ============================================================================

/**
 * @brief Callback de recepción de datos ESP-Now
 * 
 * @note Esta función es llamada desde ISR. No debe bloquear.
 * Los datos se copian y se envían a la cola del controlador.
 * 
 * @param recv_info Información de la recepción (MAC del remitente)
 * @param data Puntero a los datos recibidos
 * @param len Longitud de los datos
 */
void comm_esp_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);

/**
 * @brief Callback de confirmación de envío ESP-Now
 * 
 * @param tx_info Información del envío (contiene MAC destino)
 * @param status Estado del envío (ESP_NOW_SEND_SUCCESS o ESP_NOW_SEND_FAIL)
 */
void comm_esp_now_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status);

// ============================================================================
// Utilidades
// ============================================================================

/**
 * @brief Obtiene la dirección MAC del gateway
 * 
 * @param mac_addr Buffer donde se almacenará la MAC (6 bytes)
 */
void comm_get_gateway_mac(uint8_t *mac_addr);

/**
 * @brief Imprime la lista de sensores registrados (para debug)
 */
void comm_print_registered_sensors(void);

#endif // COMM_H
