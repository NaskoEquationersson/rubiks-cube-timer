/*
 * ESP-IDF skeleton: HD44780 parallel LCD + TTP223 touch sensor + ESP-NOW
 *
 * Pin mapping matches your Arduino LiquidCrystal setup:
 *   RS -> GPIO 19
 *   E  -> GPIO 23
 *   D4 -> GPIO 18
 *   D5 -> GPIO 17
 *   D6 -> GPIO 16
 *   D7 -> GPIO 15
 *   RW -> GND (tied directly, not driven from ESP32)
 *   Sensor OUT -> GPIO 27
 *
 * IMPORTANT: Replace `peerAddress[]` below with the actual MAC address
 * of the other ESP32 you're sending to. Print your own board's MAC
 * (see mac_print() below) and hardcode it on the *other* device.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"   // esp_rom_delay_us
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_timer.h"

static const char *TAG = "APP";

/* ---------------- LCD pin definitions ---------------- */
#define LCD_RS  GPIO_NUM_19
#define LCD_EN  GPIO_NUM_23
#define LCD_D4  GPIO_NUM_18
#define LCD_D5  GPIO_NUM_17
#define LCD_D6  GPIO_NUM_16
#define LCD_D7  GPIO_NUM_15

/* ---------------- Sensor pin ---------------- */
#define SENSOR_1_GPIO GPIO_NUM_27
#define SENSOR_2_GPIO GPIO_NUM_14
#define SENSOR_3_GPIO GPIO_NUM_12
#define SENSOR_4_GPIO GPIO_NUM_13

/* ---------------- LED pins ------------------ */
#define GREEN_LED_GPIO GPIO_NUM_25
#define RED_LED_GPIO GPIO_NUM_32

/* ---------------- Reset Button -------------- */
#define RESET_BUTTON_GPIO GPIO_NUM_26

/* ---------------- Timer Variables ---------------- */
typedef enum {
    TIMER_IDLE,      // waiting for hands to touch all sensors
    TIMER_READY,      // all sensors touched, waiting for release to start
    TIMER_RUNNING,    // counting, waiting for all-touch again to stop
    TIMER_STOPPED     // finished, showing final time until reset
} timer_state_t;

static timer_state_t timer_state = TIMER_IDLE;
static int64_t start_time = 0;
static int64_t stopped_elapsed = 0;
static int prev_all_touched = 0; // for edge detection

/* ---------------- ESP-NOW peer MAC (EDIT THIS) ---------------- */
static uint8_t peerAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef struct {
    int sensorState;
} struct_message;

static struct_message myData;

/* =========================================================
 *                    HD44780 4-bit driver
 * ========================================================= */

static void lcd_pulse_enable(void) {
    gpio_set_level(LCD_EN, 1);
    esp_rom_delay_us(1);
    gpio_set_level(LCD_EN, 0);
    esp_rom_delay_us(100); // most commands need >37us, some need more
}

static void lcd_write4bits(uint8_t nibble) {
    gpio_set_level(LCD_D4, (nibble >> 0) & 0x01);
    gpio_set_level(LCD_D5, (nibble >> 1) & 0x01);
    gpio_set_level(LCD_D6, (nibble >> 2) & 0x01);
    gpio_set_level(LCD_D7, (nibble >> 3) & 0x01);
    lcd_pulse_enable();
}

static void lcd_send(uint8_t value, int mode /* 0 = command, 1 = data */) {
    gpio_set_level(LCD_RS, mode);
    lcd_write4bits(value >> 4);
    lcd_write4bits(value & 0x0F);
}

static void lcd_command(uint8_t cmd) {
    lcd_send(cmd, 0);
}

static void lcd_write_char(char c) {
    lcd_send((uint8_t)c, 1);
}

static void lcd_print(const char *str) {
    while (*str) {
        lcd_write_char(*str++);
    }
}

static void lcd_set_cursor(uint8_t col, uint8_t row) {
    static const uint8_t row_offsets[] = {0x00, 0x40};
    lcd_command(0x80 | (col + row_offsets[row]));
}

static void lcd_clear(void) {
    lcd_command(0x01);
    esp_rom_delay_us(2000); // clear needs ~1.5-2ms
}

static void lcd_gpio_setup(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LCD_RS) | (1ULL << LCD_EN) |
                        (1ULL << LCD_D4) | (1ULL << LCD_D5) |
                        (1ULL << LCD_D6) | (1ULL << LCD_D7),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

static void lcd_init(void) {
    lcd_gpio_setup();

    // Power-on wait
    vTaskDelay(pdMS_TO_TICKS(50));

    // HD44780 4-bit init sequence (standard datasheet sequence)
    gpio_set_level(LCD_RS, 0);
    lcd_write4bits(0x03);
    vTaskDelay(pdMS_TO_TICKS(5));
    lcd_write4bits(0x03);
    esp_rom_delay_us(150);
    lcd_write4bits(0x03);
    lcd_write4bits(0x02); // set to 4-bit mode

    lcd_command(0x28); // function set: 4-bit, 2 line, 5x8 font
    lcd_command(0x0C); // display on, cursor off, blink off
    lcd_command(0x06); // entry mode: increment, no shift
    lcd_clear();
}

/* =========================================================
 *                    Sensor
 * ========================================================= */

static void sensor_init(void) {
    gpio_reset_pin(SENSOR_1_GPIO);
    gpio_set_direction(SENSOR_1_GPIO, GPIO_MODE_INPUT);
    gpio_reset_pin(SENSOR_2_GPIO);
    gpio_set_direction(SENSOR_2_GPIO, GPIO_MODE_INPUT);
    gpio_reset_pin(SENSOR_3_GPIO);
    gpio_set_direction(SENSOR_3_GPIO, GPIO_MODE_INPUT);
    gpio_reset_pin(SENSOR_4_GPIO);
    gpio_set_direction(SENSOR_4_GPIO, GPIO_MODE_INPUT);
}

static void led_init(void) {
    gpio_reset_pin(GREEN_LED_GPIO);
    gpio_set_direction(GREEN_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_reset_pin(RED_LED_GPIO);
    gpio_set_direction(RED_LED_GPIO, GPIO_MODE_OUTPUT);
}

static void reset_button_init(void) {
    gpio_reset_pin(RESET_BUTTON_GPIO);
    gpio_set_direction(RESET_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(RESET_BUTTON_GPIO, GPIO_PULLUP_ONLY);
}

/* =========================================================
 *                    Timer
 * ========================================================= */
static void format_time(int64_t elapsed_us, char *buf, size_t buf_len) {
    int64_t elapsed_ms = elapsed_us / 1000;
    int minutes = (elapsed_ms / 1000) / 60;
    int seconds = (elapsed_ms / 1000) % 60;
    int millis  = elapsed_ms % 1000;
    snprintf(buf, buf_len, "%02d:%02d.%03d", minutes, seconds, millis);
}

void update_timer(int all_touched, int reset_pressed) {
    if (reset_pressed) {
        timer_state = TIMER_IDLE;
        prev_all_touched = 0;
        return;
    }

    int rising_edge  = (all_touched && !prev_all_touched);  // just became fully touched
    int falling_edge = (!all_touched && prev_all_touched);  // just released
    prev_all_touched = all_touched;

    switch (timer_state) {
        case TIMER_IDLE:
            if (rising_edge) {
                timer_state = TIMER_READY; // all touched, now wait for release
            }
            break;

        case TIMER_READY:
            if (falling_edge) {
                start_time = esp_timer_get_time();
                timer_state = TIMER_RUNNING; // released -> start counting
            }
            break;

        case TIMER_RUNNING:
            if (rising_edge) { // touched all again -> stop
                stopped_elapsed = esp_timer_get_time() - start_time;
                timer_state = TIMER_STOPPED;
            }
            break;

        case TIMER_STOPPED:
            // do nothing until reset
            break;
    }
}

/* =========================================================
 *                    ESP-NOW
 * ========================================================= */

static void mac_print(void) {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "My MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void on_data_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    ESP_LOGI(TAG, "Send status: %s",
             status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    struct_message incoming;
    if (len != sizeof(incoming)) {
        ESP_LOGW(TAG, "Unexpected data length: %d", len);
        return;
    }
    memcpy(&incoming, data, sizeof(incoming));
    ESP_LOGI(TAG, "Received sensorState: %d", incoming.sensorState);

    // Reflect received value on our own LCD too, if desired:
    lcd_set_cursor(0, 1);
    lcd_print("RX: ");
    lcd_print(incoming.sensorState ? "1 " : "0 ");
}

static void wifi_init(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void espnow_init(void) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

    esp_now_peer_info_t peerInfo = {0};
    memcpy(peerInfo.peer_addr, peerAddress, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ESP-NOW peer");
    }
}

/* =========================================================
 *                    Main loop / app_main
 * ========================================================= */

void app_main(void) {
    wifi_init();
    mac_print();       // check the Serial/log output to get this board's MAC
    espnow_init();

    sensor_init();
    led_init();
    reset_button_init();
    lcd_init();
    lcd_print("TurkNasko");

    while (1) {
        int state = (gpio_get_level(SENSOR_1_GPIO) && gpio_get_level(SENSOR_2_GPIO) && gpio_get_level(SENSOR_3_GPIO) && gpio_get_level(SENSOR_4_GPIO));

        int reset_pressed = (gpio_get_level(RESET_BUTTON_GPIO) == 0);

        update_timer(state, reset_pressed);

        char timebuf[16];
        int64_t display_elapsed;

        if (timer_state == TIMER_RUNNING) {
            display_elapsed = esp_timer_get_time() - start_time;
        } else if (timer_state == TIMER_STOPPED) {
            display_elapsed = stopped_elapsed;
        } else {
            display_elapsed = 0;
        }

        format_time(display_elapsed, timebuf, sizeof(timebuf));


        // Update local LCD
        lcd_set_cursor(0, 1);
        lcd_print(timebuf);
        lcd_print("     "); // pad to clear leftover chars from longer previous strings

        /*
        Old code to display touch state on the LCD.
        lcd_set_cursor(0, 1);
        lcd_print("Touch: ");
        lcd_write_char(state ? '1' : '0');
        lcd_print(" ");
        */

        // Update LEDs
        gpio_set_level(GREEN_LED_GPIO, state);
        gpio_set_level(RED_LED_GPIO, !state);

        // Send over ESP-NOW
        myData.sensorState = state;
        esp_err_t result = esp_now_send(peerAddress, (uint8_t *)&myData, sizeof(myData));
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "esp_now_send failed: %s", esp_err_to_name(result));
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}