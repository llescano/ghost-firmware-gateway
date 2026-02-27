/**
 * @file realtime_commands.h
 * @brief Procesador de comandos en tiempo real via Supabase Realtime
 *
 * Reemplaza al command_processor que usa polling.
 * Ahora los comandos llegan instantáneamente via WebSocket.
 */

#ifndef REALTIME_COMMANDS_H
#define REALTIME_COMMANDS_H

#include "esp_err.h"

/**
 * @brief Inicializa el procesador de comandos realtime
 *
 * Se conecta a Supabase Realtime y escucha cambios en system_commands.
 *
 * @return ESP_OK si se inicializó correctamente
 */
esp_err_t realtime_commands_init(void);

/**
 * @brief Detiene el procesador de comandos realtime
 *
 * @return ESP_OK si se detuvo correctamente
 */
esp_err_t realtime_commands_stop(void);

#endif // REALTIME_COMMANDS_H
