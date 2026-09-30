/* Blink Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "sdkconfig.h"

/* Identificador usado para agrupar los mensajes de registro de esta aplicacion. */
static const char *ETIQUETA = "sensor_lcd";

/* Pines y direccion del LCD I2C conectado al ESP32-WROOM-32. */
#define LED_GPIO CONFIG_BLINK_GPIO
#define DHT22_GPIO CONFIG_DHT22_GPIO
#define LCD_SDA_GPIO GPIO_NUM_21
#define LCD_SCL_GPIO GPIO_NUM_22
#define LCD_DIRECCION_I2C 0x27

/* Bits del expansor PCF8574 que controla el LCD. */
#define LCD_LUZ_FONDO 0x08
#define LCD_HABILITAR 0x04
#define LCD_SELECCION_REGISTRO 0x01

/* Periodo minimo recomendado entre lecturas del DHT22. */
#define DHT22_INTERVALO_US 2000000LL
#define DHT22_TIEMPO_ESPERA_US 120

/* Estado compartido por el control del LED y la pantalla. */
static bool estado_led = false;
static i2c_master_dev_handle_t dispositivo_lcd;
#ifdef CONFIG_BLINK_LED_STRIP
static led_strip_handle_t tira_led;
#endif

/* Escribe un byte en el expansor I2C conectado al LCD. */
static void lcd_escribir_byte(uint8_t valor)
{
    ESP_ERROR_CHECK(i2c_master_transmit(dispositivo_lcd, &valor, 1, -1));
}

/* Envia un nibble del LCD y genera el pulso de habilitacion requerido. */
static void lcd_escribir_nibble(uint8_t nibble, bool es_dato)
{
    uint8_t valor = (nibble & 0xF0) | LCD_LUZ_FONDO;
    if (es_dato) {
        valor |= LCD_SELECCION_REGISTRO;
    }

    lcd_escribir_byte(valor | LCD_HABILITAR);
    lcd_escribir_byte(valor);
}

/* Envia un byte completo al LCD en dos nibbles de cuatro bits. */
static void lcd_enviar(uint8_t valor, bool es_dato)
{
    lcd_escribir_nibble(valor, es_dato);
    lcd_escribir_nibble(valor << 4, es_dato);
    vTaskDelay(pdMS_TO_TICKS(1));
}

/* Envia un comando al LCD y espera mas tiempo para borrar o volver al inicio. */
static void lcd_enviar_comando(uint8_t comando)
{
    lcd_enviar(comando, false);
    if (comando == 0x01 || comando == 0x02) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* Inicializa el bus I2C y configura el LCD de caracteres en modo de cuatro bits. */
static void configurar_lcd(void)
{
    i2c_master_bus_handle_t bus_i2c;
    i2c_master_bus_config_t configuracion_bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = LCD_SDA_GPIO,
        .scl_io_num = LCD_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&configuracion_bus, &bus_i2c));

    i2c_device_config_t configuracion_dispositivo = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LCD_DIRECCION_I2C,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_i2c, &configuracion_dispositivo, &dispositivo_lcd));

    vTaskDelay(pdMS_TO_TICKS(50));
    lcd_escribir_nibble(0x30, false);
    vTaskDelay(pdMS_TO_TICKS(5));
    lcd_escribir_nibble(0x30, false);
    vTaskDelay(pdMS_TO_TICKS(1));
    lcd_escribir_nibble(0x30, false);
    lcd_escribir_nibble(0x20, false);
    lcd_enviar_comando(0x28);
    lcd_enviar_comando(0x0C);
    lcd_enviar_comando(0x06);
    lcd_enviar_comando(0x01);
}

/* Escribe una fila completa para borrar caracteres antiguos que hayan quedado. */
static void lcd_escribir_fila(uint8_t direccion, const char *texto)
{
    char fila[17];
    snprintf(fila, sizeof(fila), "%-16.16s", texto);
    lcd_enviar_comando(direccion);
    for (size_t indice = 0; indice < 16; indice++) {
        lcd_enviar((uint8_t)fila[indice], true);
    }
}

/* Muestra la temperatura, la humedad y el estado sincronizado del LED. */
static void lcd_actualizar(bool lectura_correcta, int16_t temperatura_decimas,
                           int16_t humedad_decimas, bool led_encendido)
{
    char primera_fila[64];
    char segunda_fila[17];

    if (lectura_correcta) {
        int temperatura_absoluta = abs(temperatura_decimas);
        snprintf(primera_fila, sizeof(primera_fila), "T:%s%d.%d H:%d.%d",
                 temperatura_decimas < 0 ? "-" : "",
                 temperatura_absoluta / 10, temperatura_absoluta % 10,
                 humedad_decimas / 10, humedad_decimas % 10);
    } else {
        snprintf(primera_fila, sizeof(primera_fila), "T:--.- H:--.-");
    }

    snprintf(segunda_fila, sizeof(segunda_fila), "LED %s",
             led_encendido ? "ON" : "OFF");
    lcd_escribir_fila(0x80, primera_fila);
    lcd_escribir_fila(0xC0, segunda_fila);
}

/* Espera un nivel logico del pin del sensor sin quedarse bloqueado indefinidamente. */
static esp_err_t dht22_esperar_nivel(int nivel, uint32_t limite_us)
{
    uint32_t transcurrido_us = 0;
    while (gpio_get_level(DHT22_GPIO) != nivel && transcurrido_us < limite_us) {
        esp_rom_delay_us(1);
        transcurrido_us++;
    }

    return gpio_get_level(DHT22_GPIO) == nivel ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* Solicita y valida una trama DHT22; devuelve temperatura y humedad en decimas. */
static esp_err_t dht22_leer(int16_t *temperatura_decimas, int16_t *humedad_decimas)
{
    uint8_t datos[5] = {0};

    ESP_ERROR_CHECK(gpio_set_direction(DHT22_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(DHT22_GPIO, 0));
    esp_rom_delay_us(1200);
    ESP_ERROR_CHECK(gpio_set_level(DHT22_GPIO, 1));
    ESP_ERROR_CHECK(gpio_set_direction(DHT22_GPIO, GPIO_MODE_INPUT));

    if (dht22_esperar_nivel(0, DHT22_TIEMPO_ESPERA_US) != ESP_OK ||
        dht22_esperar_nivel(1, DHT22_TIEMPO_ESPERA_US) != ESP_OK ||
        dht22_esperar_nivel(0, DHT22_TIEMPO_ESPERA_US) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    for (uint8_t bit = 0; bit < 40; bit++) {
        if ((bit > 0 && dht22_esperar_nivel(0, DHT22_TIEMPO_ESPERA_US) != ESP_OK) ||
            dht22_esperar_nivel(1, DHT22_TIEMPO_ESPERA_US) != ESP_OK) {
            return ESP_ERR_TIMEOUT;
        }

        esp_rom_delay_us(40);
        datos[bit / 8] = (uint8_t)((datos[bit / 8] << 1) | gpio_get_level(DHT22_GPIO));
    }

    uint8_t suma_verificacion = (uint8_t)(datos[0] + datos[1] + datos[2] + datos[3]);
    if (suma_verificacion != datos[4]) {
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t humedad = (uint16_t)((datos[0] << 8) | datos[1]);
    int16_t temperatura = (int16_t)(((datos[2] & 0x7F) << 8) | datos[3]);
    *temperatura_decimas = (datos[2] & 0x80) ? -temperatura : temperatura;
    *humedad_decimas = (int16_t)humedad;
    return ESP_OK;
}

/* Configura el LED integrado como salida y lo deja inicialmente apagado. */
static void configurar_led(void)
{
    ESP_ERROR_CHECK(gpio_reset_pin(LED_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(LED_GPIO, 0));
}

/* Configura la entrada del DHT22 con resistencia pull-up interna habilitada. */
static void configurar_dht22(void)
{
    gpio_config_t configuracion_gpio = {
        .pin_bit_mask = 1ULL << DHT22_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&configuracion_gpio));
}

/* Actualiza el nivel del LED usando GPIO o la tira, segun la configuracion existente. */
static void establecer_estado_led(bool encendido)
{
#ifdef CONFIG_BLINK_LED_STRIP
    if (encendido) {
        ESP_ERROR_CHECK(led_strip_set_pixel(tira_led, 0, 16, 16, 16));
        ESP_ERROR_CHECK(led_strip_refresh(tira_led));
    } else {
        ESP_ERROR_CHECK(led_strip_clear(tira_led));
    }
#else
    ESP_ERROR_CHECK(gpio_set_level(LED_GPIO, encendido ? 1 : 0));
#endif
}

/* Prepara el tipo de LED seleccionado para conservar el comportamiento del proyecto. */
static void inicializar_led(void)
{
#ifdef CONFIG_BLINK_LED_STRIP
    led_strip_config_t configuracion_tira = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
    };
#if CONFIG_BLINK_LED_STRIP_BACKEND_RMT
    led_strip_rmt_config_t configuracion_rmt = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&configuracion_tira, &configuracion_rmt, &tira_led));
#elif CONFIG_BLINK_LED_STRIP_BACKEND_SPI
    led_strip_spi_config_t configuracion_spi = {
        .spi_bus = SPI2_HOST,
        .flags.with_dma = true,
    };
    ESP_ERROR_CHECK(led_strip_new_spi_device(&configuracion_tira, &configuracion_spi, &tira_led));
#else
#error "Backend de tira LED no compatible"
#endif
    ESP_ERROR_CHECK(led_strip_clear(tira_led));
#else
    configurar_led();
#endif
}

/* Punto de entrada requerido por ESP-IDF; coordina lecturas, pantalla y parpadeo. */
void app_main(void)
{
    int16_t temperatura_decimas = 0;
    int16_t humedad_decimas = 0;
    bool lectura_correcta = false;
    int64_t ultima_lectura_us = -DHT22_INTERVALO_US;
    TickType_t instante_anterior = xTaskGetTickCount();

    inicializar_led();
    configurar_dht22();
    configurar_lcd();

    while (true) {
        int64_t tiempo_actual_us = esp_timer_get_time();
        if (tiempo_actual_us - ultima_lectura_us >= DHT22_INTERVALO_US) {
            esp_err_t resultado = dht22_leer(&temperatura_decimas, &humedad_decimas);
            ultima_lectura_us = tiempo_actual_us;
            if (resultado == ESP_OK) {
                lectura_correcta = true;
            } else {
                ESP_LOGW(ETIQUETA, "No se pudo leer el DHT22: %s", esp_err_to_name(resultado));
            }
        }

        estado_led = !estado_led;
        establecer_estado_led(estado_led);
        lcd_actualizar(lectura_correcta, temperatura_decimas, humedad_decimas, estado_led);
        ESP_LOGI(ETIQUETA, "LED %s", estado_led ? "ON" : "OFF");
        vTaskDelayUntil(&instante_anterior, pdMS_TO_TICKS(CONFIG_BLINK_PERIOD));
    }
}
