/**
 * @file ui.c
 * @brief Implementación del componente de interfaz de usuario (LED y botón) para el Gateway
 * 
 * Usa el componente espressif/led_indicator para controlar el LED WS2812
 * y espressif/button para el botón BOOT
 */

#include "ui.h"
#include "esp_log.h"
#include "led_indicator.h"
#include "led_indicator_blink_default.h"
#include "led_indicator_strips.h"
#include "iot_button.h"
#include "button_gpio.h"

static const char *TAG = "UI";

// ============================================================================
// Definiciones de estados LED personalizados
// ============================================================================

/** @brief Número de estados LED personalizados */
#define LED_CUSTOM_STATES_COUNT 6

/** @brief Índices de estados personalizados (de mayor a menor prioridad) */
typedef enum {
    LED_CUSTOM_ALARM = 0,     // Mayor prioridad
    LED_CUSTOM_TAMPER,
    LED_CUSTOM_ARMED,
    LED_CUSTOM_DISARMED,
    LED_CUSTOM_ERROR,
    LED_CUSTOM_BOOT,          // Menor prioridad
} led_custom_index_t;

// ============================================================================
// Helpers para formato de color
// ============================================================================

/**
 * @brief Empaqueta valores HSV en un uint32_t
 * @param index Índice del LED (0-126, 127 para todos)
 * @param h Hue (0-360)
 * @param s Saturation (0-255)
 * @param v Value/Brightness (0-255)
 */
#define PACK_HSV(index, h, s, v) (((index) << 24) | ((h) << 16) | ((s) << 8) | (v))

/**
 * @brief Empaqueta valores RGB en un uint32_t
 * @param index Índice del LED (0-126, 127 para todos)
 * @param r Red (0-255)
 * @param g Green (0-255)
 * @param b Blue (0-255)
 */
#define PACK_RGB(index, r, g, b) (((index) << 24) | ((r) << 16) | ((g) << 8) | (b))

// ============================================================================
// Configuración de brillo para desarrollo
// ============================================================================
//
// Durante desarrollo, los LEDs en las placas prototipo son muy intensos.
// Usamos CONFIG_GHOST_DEV_MODE=y en sdkconfig para reducir el brillo al 25%.
//
// Para producción: comentar CONFIG_GHOST_DEV_MODE en sdkconfig
//
// Modo desarrollo:  brillo al 10% (DEV_BRIGHTNESS = 26)
// Modo producción:    brillo al 100% (DEV_BRIGHTNESS = 255)
// ============================================================================

#ifdef CONFIG_GHOST_DEV_MODE
#define DEV_BRIGHTNESS  26  // 10% de 255
#else
#define DEV_BRIGHTNESS  255  // Brillo completo para producción
#endif

// Colores HSV predefinidos (H: 0-360, S: 0-255, V: 0-255)
#define HSV_RED     PACK_HSV(0, 0,   255, DEV_BRIGHTNESS)   // Rojo
#define HSV_GREEN   PACK_HSV(0, 120, 255, DEV_BRIGHTNESS)   // Verde
#define HSV_BLUE    PACK_HSV(0, 240, 255, DEV_BRIGHTNESS)   // Azul
#define HSV_YELLOW  PACK_HSV(0, 60,  255, DEV_BRIGHTNESS)   // Amarillo
#define HSV_OFF     PACK_HSV(0, 0,   0,   0)               // Apagado

// ============================================================================
// Configuración del botón
// ============================================================================

/** @brief Tiempo de long press en ms */
#define BUTTON_LONG_PRESS_TIME_MS  2000

// ============================================================================
// Variables privadas
// ============================================================================

/** @brief Handle del indicador LED */
static led_indicator_handle_t s_led_handle = NULL;

/** @brief Handle del botón BOOT */
static button_handle_t s_boot_button = NULL;

/** @brief Estado actual del LED */
static led_system_state_t s_current_led_state = LED_SYS_BOOT;

/** @brief Callback para click simple */
static ui_button_click_cb_t s_on_click_callback = NULL;

/** @brief Callback para long press */
static ui_button_long_press_cb_t s_on_long_press_callback = NULL;

/** @brief Secuencias de blink personalizadas */
static blink_step_t const *s_led_blink_lists[] = {
    // LED_CUSTOM_ALARM: Rojo parpadeando rápido (200ms)
    (blink_step_t const []) {
        {LED_BLINK_HSV, HSV_RED, 0},       // Establecer color rojo
        {LED_BLINK_HOLD, LED_STATE_ON, 200},
        {LED_BLINK_HOLD, LED_STATE_OFF, 200},
        {LED_BLINK_LOOP, 0, 0},
    },
    // LED_CUSTOM_TAMPER: Amarillo parpadeando (500ms)
    (blink_step_t const []) {
        {LED_BLINK_HSV, HSV_YELLOW, 0},    // Establecer color amarillo
        {LED_BLINK_HOLD, LED_STATE_ON, 500},
        {LED_BLINK_HOLD, LED_STATE_OFF, 500},
        {LED_BLINK_LOOP, 0, 0},
    },
    // LED_CUSTOM_ARMED: Rojo sólido
    (blink_step_t const []) {
        {LED_BLINK_HSV, HSV_RED, 0},       // Establecer color rojo
        {LED_BLINK_HOLD, LED_STATE_ON, 1000},  // Mantener 1 segundo (loop infinito)
        {LED_BLINK_LOOP, 0, 0},
    },
    // LED_CUSTOM_DISARMED: Verde sólido
    (blink_step_t const []) {
        {LED_BLINK_HSV, HSV_GREEN, 0},     // Establecer color verde
        {LED_BLINK_HOLD, LED_STATE_ON, 1000},  // Mantener 1 segundo (loop infinito)
        {LED_BLINK_LOOP, 0, 0},
    },
    // LED_CUSTOM_ERROR: Rojo/Verde alternando
    (blink_step_t const []) {
        {LED_BLINK_HSV, HSV_RED, 0},
        {LED_BLINK_HOLD, LED_STATE_ON, 300},
        {LED_BLINK_HSV, HSV_GREEN, 0},
        {LED_BLINK_HOLD, LED_STATE_ON, 300},
        {LED_BLINK_LOOP, 0, 0},
    },
    // LED_CUSTOM_BOOT: Azul parpadeando lento (500ms)
    (blink_step_t const []) {
        {LED_BLINK_HSV, HSV_BLUE, 0},      // Establecer color azul
        {LED_BLINK_HOLD, LED_STATE_ON, 500},
        {LED_BLINK_HOLD, LED_STATE_OFF, 500},
        {LED_BLINK_LOOP, 0, 0},
    },
};

// ============================================================================
// Callbacks del botón
// ============================================================================

/**
 * @brief Callback para click simple del botón BOOT
 */
static void button_click_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, "Botón BOOT: click simple detectado");
    
    if (s_on_click_callback) {
        s_on_click_callback();
    }
}

/**
 * @brief Callback para long press del botón BOOT
 */
static void button_long_press_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, "Botón BOOT: long press detectado");
    
    if (s_on_long_press_callback) {
        s_on_long_press_callback();
    }
}

// ============================================================================
// Funciones privadas
// ============================================================================

/**
 * @brief Configura el LED WS2812 usando led_indicator
 */
static esp_err_t led_indicator_init(void)
{
    // Configuración del LED strip WS2812
    led_strip_config_t strip_config = {
        .strip_gpio_num = GATEWAY_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
        .flags.invert_out = false,
    };
    
    // Configuración RMT para el LED strip
    led_indicator_strips_config_t strips_config = {
        .led_strip_cfg = strip_config,
        .led_strip_driver = LED_STRIP_RMT,
        .led_strip_rmt_cfg = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10 * 1000 * 1000,  // 10MHz
            .flags.with_dma = false,
        },
    };
    
    // Configuración del indicador LED
    led_indicator_config_t config = {
        .blink_lists = s_led_blink_lists,
        .blink_list_num = LED_CUSTOM_STATES_COUNT,
    };
    
    esp_err_t ret = led_indicator_new_strips_device(&config, &strips_config, &s_led_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error creando indicador LED: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Indicador LED WS2812 creado en GPIO %d", GATEWAY_LED_GPIO);
    return ESP_OK;
}

/**
 * @brief Configura el botón BOOT usando espressif/button v4.x
 */
static esp_err_t button_init(void)
{
    // Configuración del botón (solo tiempos)
    button_config_t btn_cfg = {
        .long_press_time = BUTTON_LONG_PRESS_TIME_MS,
        .short_press_time = 50,  // debounce
    };
    
    // Configuración del GPIO del botón
    button_gpio_config_t gpio_cfg = {
        .gpio_num = GATEWAY_BOOT_BUTTON_GPIO,
        .active_level = 0,  // El botón BOOT es activo bajo (pull-up)
        .enable_power_save = false,
        .disable_pull = false,  // Usar pull interno
    };
    
    // Crear el botón con la nueva API v4.x
    esp_err_t ret = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &s_boot_button);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error creando botón BOOT: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Registrar callback para click simple (API v4.x requiere 5 argumentos)
    ret = iot_button_register_cb(s_boot_button, BUTTON_SINGLE_CLICK, NULL, button_click_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Error registrando callback de click simple");
    }
    
    // Registrar callback para long press (BUTTON_LONG_PRESS_UP)
    ret = iot_button_register_cb(s_boot_button, BUTTON_LONG_PRESS_UP, NULL, button_long_press_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Error registrando callback de long press");
    }
    
    ESP_LOGI(TAG, "Botón BOOT configurado en GPIO %d", GATEWAY_BOOT_BUTTON_GPIO);
    return ESP_OK;
}

// ============================================================================
// Funciones públicas
// ============================================================================

esp_err_t ui_init(void)
{
    ESP_LOGI(TAG, "Inicializando módulo de UI");
    
    // Inicializar LED
    esp_err_t ret = led_indicator_init();
    if (ret != ESP_OK) {
        return ret;
    }
    



    // Inicializar botón
    ret = button_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Error inicializando botón, continuando sin él");
        // No es crítico si el botón falla
    }
    
    // Iniciar con estado de boot
    led_indicator_start(s_led_handle, LED_CUSTOM_BOOT);
    
    ESP_LOGI(TAG, "Módulo de UI inicializado");
    return ESP_OK;
}

esp_err_t ui_deinit(void)
{
    if (s_boot_button) {
        iot_button_delete(s_boot_button);
        s_boot_button = NULL;
    }
    
    if (s_led_handle) {
        led_indicator_delete(s_led_handle);
        s_led_handle = NULL;
    }
    return ESP_OK;
}

void ui_set_button_click_callback(ui_button_click_cb_t callback)
{
    s_on_click_callback = callback;
    ESP_LOGI(TAG, "Callback de click configurado");
}

void ui_set_button_long_press_callback(ui_button_long_press_cb_t callback)
{
    s_on_long_press_callback = callback;
    ESP_LOGI(TAG, "Callback de long press configurado");
}

esp_err_t ui_set_system_state(system_state_t state)
{
    switch (state) {
        case SYS_STATE_DISARMED:
            return ui_set_led_state(LED_SYS_DISARMED);
        case SYS_STATE_ARMED:
            return ui_set_led_state(LED_SYS_ARMED);
        case SYS_STATE_ALARM:
            return ui_set_led_state(LED_SYS_ALARM);
        case SYS_STATE_TAMPER:
            return ui_set_led_state(LED_SYS_TAMPER);
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t ui_set_led_state(led_system_state_t state)
{
    if (s_led_handle == NULL) {
        ESP_LOGW(TAG, "LED no inicializado");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Mapear estado ANTERIOR a índice para detenerlo correctamente
    // Los valores de led_system_state_t y led_custom_index_t NO coinciden
    led_custom_index_t prev_index;
    switch (s_current_led_state) {
        case LED_SYS_DISARMED:
            prev_index = LED_CUSTOM_DISARMED;
            break;
        case LED_SYS_ARMED:
            prev_index = LED_CUSTOM_ARMED;
            break;
        case LED_SYS_ALARM:
            prev_index = LED_CUSTOM_ALARM;
            break;
        case LED_SYS_TAMPER:
            prev_index = LED_CUSTOM_TAMPER;
            break;
        case LED_SYS_BOOT:
            prev_index = LED_CUSTOM_BOOT;
            break;
        case LED_SYS_ERROR:
        default:
            prev_index = LED_CUSTOM_ERROR;
            break;
    }
    
    // Detener estado anterior usando el índice correcto
    led_indicator_stop(s_led_handle, prev_index);
    
    // Mapear estado NUEVO a índice
    led_custom_index_t index;
    switch (state) {
        case LED_SYS_DISARMED:
            index = LED_CUSTOM_DISARMED;
            break;
        case LED_SYS_ARMED:
            index = LED_CUSTOM_ARMED;
            break;
        case LED_SYS_ALARM:
            index = LED_CUSTOM_ALARM;
            break;
        case LED_SYS_TAMPER:
            index = LED_CUSTOM_TAMPER;
            break;
        case LED_SYS_BOOT:
            index = LED_CUSTOM_BOOT;
            break;
        case LED_SYS_ERROR:
        default:
            index = LED_CUSTOM_ERROR;
            break;
    }
    
    // Establecer nuevo estado
    s_current_led_state = state;
    led_indicator_start(s_led_handle, index);
    
    ESP_LOGI(TAG, "Estado LED cambiado a: %d", state);
    return ESP_OK;
}

void ui_blink(uint8_t color, uint8_t times)
{
    if (s_led_handle == NULL) {
        return;
    }
    
    // Mapear color a HSV
    uint32_t hsv_color;
    switch (color) {
        case 0:  // Rojo
            hsv_color = HSV_RED;
            break;
        case 1:  // Verde
            hsv_color = HSV_GREEN;
            break;
        case 2:  // Azul
            hsv_color = HSV_BLUE;
            break;
        case 3:  // Amarillo
            hsv_color = HSV_YELLOW;
            break;
        default:
            hsv_color = HSV_RED;
            break;
    }
    
    // Guardar estado actual
    led_system_state_t prev_state = s_current_led_state;
    
    // Detener cualquier blink actual
    led_indicator_stop(s_led_handle, s_current_led_state);
    
    // Hacer parpadeos manuales
    for (int i = 0; i < times; i++) {
        led_indicator_set_hsv(s_led_handle, hsv_color);
        led_indicator_set_on_off(s_led_handle, true);
        vTaskDelay(pdMS_TO_TICKS(200));
        led_indicator_set_on_off(s_led_handle, false);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    
    // Restaurar estado anterior
    ui_set_led_state(prev_state);
}

void ui_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_led_handle == NULL) {
        return;
    }
    
    // Usar RGB directamente
    uint32_t rgb = PACK_RGB(0, r, g, b);
    led_indicator_set_rgb(s_led_handle, rgb);
    led_indicator_set_on_off(s_led_handle, true);
}

void ui_led_off(void)
{
    if (s_led_handle == NULL) {
        return;
    }
    
    led_indicator_stop(s_led_handle, s_current_led_state);
    led_indicator_set_on_off(s_led_handle, false);
}

void ui_task(void *pvParameters)
{
    // Esta tarea ya no es necesaria con led_indicator
    // El componente maneja internamente las secuencias de blink
    ESP_LOGI(TAG, "Tarea de UI iniciada (modo delegado a led_indicator)");
    
    // La tarea puede eliminarse o usarse para otras funciones de UI
    vTaskDelete(NULL);
}
