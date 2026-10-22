// Importing Libraries

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "RECEIVER"; // was missing - used by mac_print()

// --- LCD pin definitions (were missing) ---
#define LCD_RS  GPIO_NUM_19
#define LCD_EN  GPIO_NUM_23
#define LCD_D4  GPIO_NUM_18
#define LCD_D5  GPIO_NUM_17
#define LCD_D6  GPIO_NUM_16
#define LCD_D7  GPIO_NUM_15

// --- Variables and Structs ---

/* ---------------- ESP-NOW peer MAC ---------------- */
// static uint8_t peerAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; Dummy
static uint8_t peerAddress[6] = {0x88, 0x57, 0x21, 0x2D, 0xE2, 0x00};

typedef struct {
    char text[16];       // e.g. "TurkNasko" or status label
    char time_str[16];   // pre-formatted "00:12.345"
} struct_message;

static struct_message myData;

// --- Setup Functions ---

// LCD Setup

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
 *                    ESP-NOW + WiFi Setup
 * ========================================================= */

static void mac_print(void) {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "My MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Receiver doesn't send anything, but the callback still needs a
// valid function to register. Kept as a harmless stub.
static void on_data_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    ESP_LOGI(TAG, "Send status: %s",
             status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    struct_message incoming;
    if (len != sizeof(incoming)) return;
    memcpy(&incoming, data, sizeof(incoming));

    lcd_set_cursor(0, 0);
    lcd_print(incoming.text);
    lcd_print("        "); // pad to clear leftovers

    lcd_set_cursor(0, 1);
    lcd_print(incoming.time_str);
    lcd_print("     ");
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

// Main

void app_main(void) {
    wifi_init();
    mac_print();
    espnow_init();
    lcd_init();

    lcd_print("Waiting...");  // <-- missing semicolon fixed

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); // main loop delay
    }
}