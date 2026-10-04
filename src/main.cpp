// Basic Arduino and WiFi
#include "Arduino.h"
#include "WiFi.h"
#include <WiFiClientSecure.h>

// Spotify & JSON
#include <ArduinoJson.h>
#include <SpotifyArduino.h>
#include <SpotifyArduinoCert.h>
#include <HTTPClient.h>
#include "test_img.h"
#include "test_img2.h"
#include "secrets.h"

// LVGL & LCD Drivers
#include "lvgl.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h" // Specifically for your Waveshare AMOLED
#include "esp_lcd_touch_ft5x06.h"

// Low-level Hardware control
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cache.h"

// --- 1. Rotation and Orientation ---
#define Rotate_90      0
#define Rotate_NONO    1
#define AMOLED_Rotate  Rotate_90

static const char *TAG = "example";

// --- 2. Hardware Bus Definitions ---
#define LCD_HOST    SPI2_HOST
#define TOUCH_HOST  I2C_NUM_0
#define LCD_BIT_PER_PIXEL       (16)

// --- 3. LCD Pin Definitions (Waveshare ESP32-S3 AMOLED) ---
#define EXAMPLE_PIN_NUM_LCD_CS            (GPIO_NUM_9)
#define EXAMPLE_PIN_NUM_LCD_PCLK          (GPIO_NUM_10) 
#define EXAMPLE_PIN_NUM_LCD_DATA0         (GPIO_NUM_11)
#define EXAMPLE_PIN_NUM_LCD_DATA1         (GPIO_NUM_12)
#define EXAMPLE_PIN_NUM_LCD_DATA2         (GPIO_NUM_13)
#define EXAMPLE_PIN_NUM_LCD_DATA3         (GPIO_NUM_14)
#define EXAMPLE_PIN_NUM_LCD_RST           (GPIO_NUM_21)
#define EXAMPLE_PIN_NUM_BK_LIGHT          (-1)

// --- 4. Resolution Logic ---
#if (AMOLED_Rotate == Rotate_90)
    #define EXAMPLE_LCD_H_RES              600    
    #define EXAMPLE_LCD_V_RES              450
#else
    #define EXAMPLE_LCD_H_RES              450  
    #define EXAMPLE_LCD_V_RES              600
#endif

// --- 5. Touch Configuration (Optional for now) ---
#define EXAMPLE_USE_TOUCH              1 
#if EXAMPLE_USE_TOUCH
    #define EXAMPLE_PIN_NUM_TOUCH_SCL         (GPIO_NUM_48)
    #define EXAMPLE_PIN_NUM_TOUCH_SDA         (GPIO_NUM_47)
    #define EXAMPLE_PIN_NUM_TOUCH_RST         (gpio_num_t)(-1)
    #define EXAMPLE_PIN_NUM_TOUCH_INT         (gpio_num_t)(-1)
    esp_lcd_touch_handle_t tp = NULL;
#endif

// --- 6. LVGL Task & Buffer Settings ---
#define EXAMPLE_LVGL_BUF_HEIGHT        (EXAMPLE_LCD_V_RES/10)
#define EXAMPLE_LVGL_TICK_PERIOD_MS    2
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 1
#define EXAMPLE_LVGL_TASK_STACK_SIZE   (8 * 1024)
#define EXAMPLE_LVGL_TASK_PRIORITY     2
#define CANVAS_SIZE 300

#define POLL_INTERVAL_MS 3000
char currentTrackUri[100] = "";
unsigned long lastPollMs = 0;

uint8_t* pending_buffer = NULL;
uint32_t pending_size = 0;

char currentAlbumArtUrl[200];
bool newImageReady = false;

// --- 7. SH8601 Specific Initialization Commands ---
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
  {0xFE, (uint8_t []){0x20}, 1, 0}, 
  {0x26, (uint8_t []){0x0A}, 1, 0}, 
  {0x24, (uint8_t []){0x80}, 1, 0}, 
  {0xFE, (uint8_t []){0x00}, 1, 0},    
  {0x3A, (uint8_t []){0x55}, 1, 0},      
  {0xC2, (uint8_t []){0x00}, 1, 10},  
  {0x35, (uint8_t []){0x00}, 0, 0}, 
  {0x51, (uint8_t []){0x00}, 1, 10},
  {0x11, (uint8_t []){0x00}, 0, 80},  
  {0x2A, (uint8_t []){0x00,0x10,0x01,0xD1}, 4, 0},
  {0x2B, (uint8_t []){0x00,0x00,0x02,0x57}, 4, 0},
  {0x29, (uint8_t []){0x00}, 0, 10},
#if (AMOLED_Rotate == Rotate_90)
  {0x36, (uint8_t []){0x30}, 1, 0},     
#endif
  {0x51, (uint8_t []){0xFF}, 1, 0}, // Max Brightness
};

// --- Credentials ---
static_assert(sizeof(WIFI_SSID) > 1, "Set WIFI_SSID in include/secrets.h");
static_assert(sizeof(WIFI_PASSWORD) > 1, "Set WIFI_PASSWORD in include/secrets.h");
static_assert(sizeof(SPOTIFY_REFRESH_TOKEN) > 1, "Set SPOTIFY_REFRESH_TOKEN in include/secrets.h");
static_assert(sizeof(SPOTIFY_CLIENT_ID) > 1, "Set SPOTIFY_CLIENT_ID in include/secrets.h");
static_assert(sizeof(SPOTIFY_CLIENT_SECRET) > 1, "Set SPOTIFY_CLIENT_SECRET in include/secrets.h");

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* refresh_token = SPOTIFY_REFRESH_TOKEN;
char clientId[] = SPOTIFY_CLIENT_ID;
char clientSecret[] = SPOTIFY_CLIENT_SECRET;

const char* test_url = "https://i.scdn.co/image/ab67616d00001e0295d73e298e50c58ad161f696";

// --- Objects ---
WiFiClientSecure client;
SpotifyArduino spotify(client, clientId, clientSecret, refresh_token);

// UI Objects
// --- Global Objects ---
lv_obj_t *album_art_obj;             // The UI Widget

static SemaphoreHandle_t lvgl_mux = NULL;

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
  lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
  lv_disp_flush_ready(disp_driver);
  return false;
}
static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
  esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
#if (AMOLED_Rotate == Rotate_90)
  const int offsetx1 = area->x1;
  const int offsetx2 = area->x2;
  const int offsety1 = area->y1 + 16;
  const int offsety2 = area->y2 + 16;
#else
  const int offsetx1 = area->x1 + 16;
  const int offsetx2 = area->x2 + 16;
  const int offsety1 = area->y1;
  const int offsety2 = area->y2;
#endif
  // copy a buffer's content to a specific area of the display
  esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}
/* Rotate display and touch, when rotated screen in LVGL. Called when driver parameters are updated. not currentyl in use */
static void example_lvgl_update_cb(lv_disp_drv_t *drv)
{
  esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;

  switch (drv->rotated)
  {
    case LV_DISP_ROT_NONE:
      // Rotate LCD display
      esp_lcd_panel_swap_xy(panel_handle, false);
      esp_lcd_panel_mirror(panel_handle, true, false);
      break;
    case LV_DISP_ROT_90:
      // Rotate LCD display
      esp_lcd_panel_swap_xy(panel_handle, true);
      esp_lcd_panel_mirror(panel_handle, true, true);
      break;
    case LV_DISP_ROT_180:
      // Rotate LCD display
      esp_lcd_panel_swap_xy(panel_handle, false);
      esp_lcd_panel_mirror(panel_handle, false, true);
      break;
    case LV_DISP_ROT_270:
      // Rotate LCD display
      esp_lcd_panel_swap_xy(panel_handle, true);
      esp_lcd_panel_mirror(panel_handle, false, false);
      break;
  }
}
void example_lvgl_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area)
{
  uint16_t x1 = area->x1;
  uint16_t x2 = area->x2;

  uint16_t y1 = area->y1;
  uint16_t y2 = area->y2;

  // round the start of coordinate down to the nearest 2M number
  area->x1 = (x1 >> 1) << 1;
  area->y1 = (y1 >> 1) << 1;
  // round the end of coordinate up to the nearest 2N+1 number
  area->x2 = ((x2 >> 1) << 1) + 1;
  area->y2 = ((y2 >> 1) << 1) + 1;
}

#if EXAMPLE_USE_TOUCH
static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)drv->user_data;
    assert(tp);

    uint16_t tp_x;
    uint16_t tp_y;
    uint8_t tp_cnt = 0;
    /* Read data from touch controller into memory */
    esp_lcd_touch_read_data(tp);
    /* Read data from touch controller */
    bool tp_pressed = esp_lcd_touch_get_coordinates(tp, &tp_x, &tp_y, NULL, &tp_cnt, 1);
    if (tp_pressed && tp_cnt > 0) {
        data->point.x = tp_x ;
        data->point.y = tp_y ;
        data->state = LV_INDEV_STATE_PRESSED;
        ESP_LOGD(TAG, "Touch position: %d,%d", tp_x, tp_y);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
#endif

static void example_increase_lvgl_tick(void *arg)
{
  /* Tell LVGL how many milliseconds has elapsed */
  lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}
static bool example_lvgl_lock(int timeout_ms)
{
  assert(lvgl_mux && "bsp_display_start must be called first");

  const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}
static void example_lvgl_unlock(void)
{
  assert(lvgl_mux && "bsp_display_start must be called first");
  xSemaphoreGive(lvgl_mux);
}
static void example_lvgl_port_task(void *arg)
{
  ESP_LOGI(TAG, "Starting LVGL task");
  uint32_t task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
  while (1)
  {
    // Lock the mutex due to the LVGL APIs are not thread-safe
    if (example_lvgl_lock(-1)) {
        task_delay_ms = lv_timer_handler();
        // Release the mutex
        example_lvgl_unlock();
    }
    if (task_delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS) {
        task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    } else if (task_delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS) {
        task_delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
    }
    vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
  }
}
     
void initDisplay() {
    const spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(
        EXAMPLE_PIN_NUM_LCD_PCLK,
        EXAMPLE_PIN_NUM_LCD_DATA0,
        EXAMPLE_PIN_NUM_LCD_DATA1,
        EXAMPLE_PIN_NUM_LCD_DATA2,
        EXAMPLE_PIN_NUM_LCD_DATA3,
        EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * LCD_BIT_PER_PIXEL / 8
    );
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    static lv_disp_drv_t disp_drv;
    esp_lcd_panel_io_handle_t io_handle = NULL;
    const esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(
        EXAMPLE_PIN_NUM_LCD_CS,
        example_notify_lvgl_flush_ready,
        &disp_drv
    );
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    sh8601_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    lv_init();
    static lv_disp_draw_buf_t disp_buf;
    lv_color_t *buf1 = (lv_color_t*)heap_caps_malloc(EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    lv_color_t *buf2 = (lv_color_t*)heap_caps_malloc(EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = EXAMPLE_LCD_H_RES;
    disp_drv.ver_res = EXAMPLE_LCD_V_RES;
    disp_drv.flush_cb = example_lvgl_flush_cb;
    disp_drv.rounder_cb = example_lvgl_rounder_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    lv_disp_drv_register(&disp_drv);

    Serial.println("Display initialized");
}

void initUI() {
    lvgl_mux = xSemaphoreCreateMutex();

    if (xSemaphoreTake(lvgl_mux, portMAX_DELAY) == pdTRUE) {
        // Black background
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_PART_MAIN);

        // Image object placeholder
        album_art_obj = lv_img_create(lv_scr_act());
        lv_obj_set_size(album_art_obj, 300, 300);
        lv_obj_align(album_art_obj, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(album_art_obj, lv_color_hex(0x333333), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(album_art_obj, LV_OPA_COVER, LV_PART_MAIN);

        

        xSemaphoreGive(lvgl_mux);
    }
    Serial.println("UI initialized");
}

void initWiFi() {
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected");
    Serial.println(WiFi.localIP());
}

void startLVGLTask() {
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &example_increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

    xTaskCreate(example_lvgl_port_task, "LVGL", EXAMPLE_LVGL_TASK_STACK_SIZE, NULL, EXAMPLE_LVGL_TASK_PRIORITY, NULL);
    
    Serial.println("LVGL task started");
}

lv_img_dsc_t my_local_jpg;

void update_image_descriptor(const uint8_t* new_data_ptr, uint32_t new_size) {
    my_local_jpg.header.always_zero = 0;
    my_local_jpg.header.w = 300;
    my_local_jpg.header.h = 300;
    my_local_jpg.header.cf = LV_IMG_CF_RAW; 
    
    // Assign the new info we just got from the internet
    my_local_jpg.data_size = new_size;
    my_local_jpg.data = new_data_ptr; 
}

uint8_t* current_art_buffer = NULL;

void downloadTask(void *pvParameters) {
    const char *url = (const char *)pvParameters;
    
    WiFiClientSecure dlClient;
    dlClient.setInsecure();
    HTTPClient http;

    if (http.begin(dlClient, url)) {
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            int contentLength = http.getSize();
            
            // Handle chunked (unknown size) — allocate a reasonable max
            int allocSize = (contentLength > 0) ? contentLength : 80000;
            uint8_t *temp_buffer = (uint8_t *)heap_caps_malloc(allocSize, MALLOC_CAP_SPIRAM);
            
            if (temp_buffer != NULL) {
                WiFiClient *stream = http.getStreamPtr();
                int bytesRead = 0;
                while (http.connected() && bytesRead < allocSize) {
                    if (stream->available()) {
                        bytesRead += stream->readBytes(temp_buffer + bytesRead, 
                                                       stream->available());
                    }
                    vTaskDelay(1); // yield to watchdog
                }

                if (bytesRead > 0) {
                    pending_buffer = temp_buffer;
                    pending_size = bytesRead;
                    newImageReady = true;
                } else {
                    free(temp_buffer);
                }
            }
        }
        http.end();
    }
    free((void*)url);
    vTaskDelete(NULL); // self-delete when done
}

void printCurrentlyPlaying(CurrentlyPlaying currentlyPlaying) {
    // Only update if the track has changed
    if (strcmp(currentTrackUri, currentlyPlaying.trackUri) != 0) {
        Serial.print("New track: ");
        Serial.println(currentlyPlaying.trackName);
        strncpy(currentTrackUri, currentlyPlaying.trackUri, sizeof(currentTrackUri) - 1);

        for (int i = 0; i < currentlyPlaying.numImages; i++) {
            if (currentlyPlaying.albumImages[i].width == 300) {
                strncpy(currentAlbumArtUrl, currentlyPlaying.albumImages[i].url, sizeof(currentAlbumArtUrl) - 1);
                break;
            }
        }
        char *urlCopy = (char*)malloc(strlen(currentAlbumArtUrl) + 1);
        strcpy(urlCopy, currentAlbumArtUrl);
        Serial.print("Downloading URL: ");
        Serial.println(currentAlbumArtUrl);
        xTaskCreate(downloadTask, "download", 8192, (void*)urlCopy, 1, NULL);
    }
}

void setup() {
    Serial.begin(115200);
    delay(500); 
    Serial.println("\n--- Initializing Spotify AMOLED Demo ---");
    initDisplay();
    initUI();
    lv_split_jpeg_init();
    
    startLVGLTask();
    initWiFi();
    client.setInsecure();

    Serial.println("Refreshing Spotify token...");
if (spotify.refreshAccessToken()) {
    Serial.println("Spotify connected successfully!");
} else {
    Serial.println("Spotify token refresh FAILED");
}
    
}

void loop() {
    if (newImageReady) {
    newImageReady = false;
    if (example_lvgl_lock(500)) {
        Serial.print("Swapping buffer, pending_size: ");
        Serial.println(pending_size);
        Serial.print("current_art_buffer before free: ");
        Serial.println((uint32_t)current_art_buffer, HEX);
        if (current_art_buffer != NULL) free(current_art_buffer);
        current_art_buffer = pending_buffer;
        Serial.print("current_art_buffer after swap: ");
        Serial.println((uint32_t)current_art_buffer, HEX);
        update_image_descriptor(current_art_buffer, pending_size);
        Serial.print("my_local_jpg.data_size: ");
        Serial.println(my_local_jpg.data_size);
        Serial.print("my_local_jpg.data ptr: ");
        Serial.println((uint32_t)my_local_jpg.data, HEX);
        lv_img_set_src(album_art_obj, NULL);
        lv_img_set_src(album_art_obj, &my_local_jpg);
        lv_obj_invalidate(album_art_obj);
        example_lvgl_unlock();
        Serial.println("Display updated successfully");
        vTaskDelay(pdMS_TO_TICKS(200)); // give LVGL time to render
    } else {
    Serial.println("LVGL lock timed out!");
    }
}

    if (WiFi.status() == WL_CONNECTED) {
        unsigned long now = millis();
        if (now - lastPollMs > POLL_INTERVAL_MS) {
            lastPollMs = now;
            Serial.println("Polling Spotify...");
            spotify.getCurrentlyPlaying(printCurrentlyPlaying, "CA");
            Serial.println("Polling done");
        }
    }
}
