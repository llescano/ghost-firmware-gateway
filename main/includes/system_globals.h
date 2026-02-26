/**
 * @file system_globals.h
 * @brief "Cerebro Fantasma" - Contexto global del sistema Gateway
 * 
 * Este header define la estructura central que mantiene el estado del sistema,
 * las colas de comunicación FreeRTOS y los mecanismos de sincronización.
 * 
 * @note Diseñado para migración futura a Mesh Lite con #define USE_MESH_LITE
 */

#ifndef SYSTEM_GLOBALS_H
#define SYSTEM_GLOBALS_H

#include <stdint.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// ============================================================================
// Configuración de compilación
// ============================================================================

/**
 * @brief Habilitar soporte para Mesh Lite (futuro)
 * Descomentar cuando se integre el componente esp_mesh_lite
 */
// #define USE_MESH_LITE

// ============================================================================
// Constantes del sistema
// ============================================================================

/** @brief Tamaño máximo de la cola de mensajes del controlador */
#define CONTROLLER_QUEUE_SIZE       10

/** @brief Longitud máxima de un mensaje ESP-Now */
#define ESPNOW_MAX_DATA_LEN         250

/** @brief Tamaño máximo del ID de dispositivo */
#define DEVICE_ID_MAX_LEN           16

/** @brief Namespace NVS para configuración del sistema */
#define NVS_NAMESPACE_SYSTEM        "sys_cfg"

/** @brief Clave NVS para modo de arranque */
#define NVS_KEY_BOOT_MODE           "boot_mode"

/** @brief Clave NVS para último estado conocido */
#define NVS_KEY_LAST_STATE          "last_state"

// ============================================================================
// GPIO del Gateway (ESP32-S3-Zero)
// ============================================================================

/** @brief GPIO del botón BOOT (usado para armado/desarmado de emergencia) */
#define GATEWAY_BOOT_BUTTON_GPIO    GPIO_NUM_0

/** @brief GPIO del LED WS2812 RGB (LED integrado en la placa) */ 
#define GATEWAY_LED_GPIO            GPIO_NUM_21

// ============================================================================
// Tipos enumerados
// ============================================================================

/**
 * @brief Modos de arranque del sistema
 * 
 * - LAST_STATE: Restaura el estado anterior al reiniciar
 * - FORCE_DISARMED: Siempre inicia desarmado
 * - FORCE_ARMED: Siempre inicia armado
 */
typedef enum {
    BOOT_MODE_LAST_STATE = 0,     /**< Restaurar último estado */
    BOOT_MODE_FORCE_DISARMED = 1, /**< Forzar inicio desarmado */
    BOOT_MODE_FORCE_ARMED = 2     /**< Forzar inicio armado */
} boot_mode_t;

/**
 * @brief Estados del sistema de seguridad
 */
typedef enum {
    SYS_STATE_DISARMED = 0,    /**< Sistema desarmado - sin monitoreo */
    SYS_STATE_ARMED = 1,       /**< Sistema armado - monitoreando sensores */
    SYS_STATE_ALARM = 2,       /**< Alarma activa - detección de intrusión */
    SYS_STATE_TAMPER = 3       /**< Tamper detectado - manipulación del equipo */
} system_state_t;

/**
 * @brief Tipos de dispositivos soportados
 */
typedef enum {
    DEV_TYPE_GATEWAY = 0,      /**< Gateway central (este dispositivo) */
    DEV_TYPE_SENSOR_DOOR = 1,  /**< Sensor de puerta/ventana */
    DEV_TYPE_SENSOR_PIR = 2,   /**< Sensor de movimiento PIR */
    DEV_TYPE_KEYPAD = 3        /**< Teclado para armado/desarmado */
} device_type_t;

/**
 * @brief Tipos de mensajes en la cola del controlador
 */
typedef enum {
    MSG_TYPE_SENSOR_EVENT = 0,   /**< Evento de sensor (abierto/cerrado) */
    MSG_TYPE_ARM_COMMAND = 1,    /**< Comando de armado */
    MSG_TYPE_DISARM_COMMAND = 2, /**< Comando de desarmado */
    MSG_TYPE_PANIC = 3,          /**< Botón de pánico */
    MSG_TYPE_HEARTBEAT = 4       /**< Latido de sensor */
} message_type_t;

/**
 * @brief Acciones de eventos de sensor
 */
typedef enum {
    SENSOR_ACTION_OPEN = 0,     /**< Sensor abierto */
    SENSOR_ACTION_CLOSED = 1,   /**< Sensor cerrado */
    SENSOR_ACTION_TAMPER = 2    /**< Tamper detectado */
} sensor_action_t;

// ============================================================================
// Estructuras de datos
// ============================================================================

/**
 * @brief Cabecera de mensaje ESP-Now
 */
typedef struct {
    uint8_t version;                    /**< Versión del protocolo */
    char src_id[DEVICE_ID_MAX_LEN];     /**< ID del dispositivo origen */
    device_type_t src_type;             /**< Tipo de dispositivo origen */
} message_header_t;

/**
 * @brief Payload de mensaje ESP-Now
 */
typedef struct {
    message_type_t type;        /**< Tipo de mensaje */
    uint8_t action;             /**< Acción (depende del tipo) */
    uint8_t value;              /**< Valor adicional */
} message_payload_t;

/**
 * @brief Mensaje completo para la cola del controlador
 */
typedef struct {
    message_header_t header;    /**< Cabecera del mensaje */
    message_payload_t payload;  /**< Payload del mensaje */
    int8_t rssi;                /**< Intensidad de señal recibida */
} controller_message_t;

/**
 * @brief Información de un sensor registrado
 */
typedef struct {
    char device_id[DEVICE_ID_MAX_LEN];  /**< ID único del sensor */
    device_type_t type;                  /**< Tipo de sensor */
    uint8_t state;                       /**< Estado actual (0=cerrado, 1=abierto) */
    uint8_t is_registered;               /**< Flag de registro activo */
    uint32_t last_seen;                  /**< Timestamp del último heartbeat */
    int8_t last_rssi;                    /**< Último RSSI reportado */
} sensor_info_t;

/**
 * @brief Máximo número de sensores soportados
 */
#define MAX_SENSORS     16

// ============================================================================
// Contexto Global del Sistema ("Cerebro Fantasma")
// ============================================================================

/**
 * @brief Estructura principal del contexto del sistema
 * 
 * Esta estructura centraliza todo el estado del Gateway y proporciona
 * los mecanismos de sincronización necesarios para acceso thread-safe.
 * 
 * @warning Usar SIEMPRE el mutex para acceder a los campos de estado
 */
typedef struct {
    // --- Estado del sistema ---
    system_state_t current_state;       /**< Estado actual del sistema */
    system_state_t previous_state;      /**< Estado anterior (para transiciones) */
    boot_mode_t boot_mode;              /**< Modo de arranque configurado */
    
    // --- Sensores registrados ---
    sensor_info_t sensors[MAX_SENSORS]; /**< Array de sensores */
    uint8_t sensor_count;               /**< Número de sensores activos */
    
    // --- Comunicación FreeRTOS ---
    QueueHandle_t controller_queue;     /**< Cola de mensajes al controlador */
    SemaphoreHandle_t mutex;            /**< Mutex para acceso thread-safe */
    
    // --- Información del dispositivo ---
    char device_id[DEVICE_ID_MAX_LEN];  /**< ID único del Gateway */
    
#ifdef USE_MESH_LITE
    // --- Mesh Lite (futuro) ---
    uint8_t mesh_layer;                 /**< Capa en la malla */
    uint8_t mesh_is_root;               /**< Es nodo raíz */
#endif
    
} system_context_t;

// ============================================================================
// Variable global externa
// ============================================================================

/**
 * @brief Instancia global del contexto del sistema
 * 
 * Debe ser inicializada en app_main() antes de crear tareas
 */
extern system_context_t gSystemCtx;

// ============================================================================
// Funciones de utilidad (declaraciones)
// ============================================================================

/**
 * @brief Inicializa el contexto global del sistema
 * @return ESP_OK si exitoso, código de error en caso contrario
 */
esp_err_t system_context_init(void);

/**
 * @brief Adquiere el mutex del contexto
 * @param timeout_ms Tiempo máximo de espera en ms (portMAX_DELAY para infinito)
 * @return true si adquirido, false si timeout
 */
bool system_context_lock(uint32_t timeout_ms);

/**
 * @brief Libera el mutex del contexto
 */
void system_context_unlock(void);

/**
 * @brief Busca un sensor por su ID
 * @param device_id ID del sensor a buscar
 * @return Índice en el array o -1 si no encontrado
 */
int8_t system_find_sensor(const char *device_id);

/**
 * @brief Registra o actualiza un sensor
 * @param device_id ID del sensor
 * @param type Tipo de dispositivo
 * @return Índice asignado o -1 si array lleno
 */
int8_t system_register_sensor(const char *device_id, device_type_t type);

#endif // SYSTEM_GLOBALS_H
