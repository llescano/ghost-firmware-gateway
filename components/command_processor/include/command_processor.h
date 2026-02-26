/**
 * @file command_processor.h
 * @brief Procesador de comandos remotos desde Supabase
 *
 * Este componente consulta periódicamente la tabla system_commands
 * y ejecuta comandos ARM/DISARM/TEST en el sistema local.
 */

#ifndef COMMAND_PROCESSOR_H
#define COMMAND_PROCESSOR_H

#include "esp_err.h"

/**
 * @brief Inicializa el procesador de comandos remotos
 *
 * Crea una tarea que consulta periódicamente Supabase
 * en busca de comandos pendientes.
 *
 * @return ESP_OK si se inicializó correctamente
 */
esp_err_t command_processor_init(void);

/**
 * @brief Detiene el procesador de comandos remotos
 *
 * @return ESP_OK si se detuvo correctamente
 */
esp_err_t command_processor_stop(void);

/**
 * @brief Verifica si hay comandos pendientes manualmente
 *
 * @return ESP_OK si la consulta se realizó correctamente
 */
esp_err_t command_processor_check_now(void);

#endif // COMMAND_PROCESSOR_H
