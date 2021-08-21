/* SD card and FAT filesystem example.
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <math.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif
#include "lvgl_helpers.h"

#ifdef CONFIG_IDF_TARGET_ESP32
#include "driver/sdmmc_host.h"
#endif

static const char *TAG = "main";

#define MOUNT_POINT "/sdcard"

// This example can use SDMMC and SPI peripherals to communicate with SD card.
// By default, SDMMC peripheral is used.
// To enable SPI mode, uncomment the following line:

#define USE_SPI_MODE

// ESP32-S2 and ESP32-C3 doesn't have an SD Host peripheral, always use SPI:
#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32C3
#ifndef USE_SPI_MODE
#define USE_SPI_MODE
#endif // USE_SPI_MODE
// on ESP32-S2, DMA channel must be the same as host id
#define SPI_DMA_CHAN host.slot
#endif //CONFIG_IDF_TARGET_ESP32S2

// DMA channel to be used by the SPI peripheral
#ifndef SPI_DMA_CHAN
#define SPI_DMA_CHAN 1
#endif //SPI_DMA_CHAN

// When testing SD and SPI modes, keep in mind that once the card has been
// initialized in SPI mode, it can not be reinitialized in SD mode without
// toggling power to the card.

#ifdef USE_SPI_MODE
// Pin mapping when using SPI mode.
// With this mapping, SD card can be used both in SPI and 1-line SD mode.
// Note that a pull-up on CS line is required in SD mode.
#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2
#define PIN_NUM_MISO (22)
#define PIN_NUM_MOSI (19)
#define PIN_NUM_CLK (21)
#define PIN_NUM_CS (0)

#elif CONFIG_IDF_TARGET_ESP32C3
#define PIN_NUM_MISO 18
#define PIN_NUM_MOSI 9
#define PIN_NUM_CLK 8
#define PIN_NUM_CS 19

#endif //CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2
#endif //USE_SPI_MODE

#define LV_TICK_PERIOD_MS (10)

#define MAP_NAME "map2"
#define TILE_SIZE (256)

// Oliwa
// #define LOC_LAT (54.41270414275709)
// #define LOC_LON (18.553496223916973)

// Gdansk
#define LOC_LAT (54.3520)
#define LOC_LON (18.6466)

#define MAX_ZOOM_LEVEL (16)

static SemaphoreHandle_t xGuiSemaphore;

static void deg2num(double lat, double lon, uint8_t zoom,
                    size_t *x, size_t *y, uint16_t *dx, uint16_t *dy)
{
    double tmp;
    double lat_rad = lat * (M_PI / 180.0);
    size_t n = 1 << zoom;
    double xtile = (lon + 180.0) / 360.0 * n;
    double ytile = (1.0 - asinh(tan(lat_rad)) / M_PI) / 2.0 * n;

    *x = xtile;
    *y = ytile;
    *dx = TILE_SIZE * modf(xtile, &tmp);
    *dy = TILE_SIZE * modf(ytile, &tmp);
}

static void lv_tick_task(void *arg)
{
    (void)arg;

    lv_tick_inc(LV_TICK_PERIOD_MS);
}

static void lvgl_task(void *pvParameter)
{
    (void)pvParameter;

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (pdTRUE == xSemaphoreTake(xGuiSemaphore, portMAX_DELAY))
        {
            lv_task_handler();
            xSemaphoreGive(xGuiSemaphore);
        }
    }
}

static lv_fs_res_t open_cb(struct _lv_fs_drv_t *drv, void *file_p, const char *path, lv_fs_mode_t mode)
{
    (void)drv;
    (void)mode;

    FILE *fp = fopen(path, "rb"); // only reading is supported

    *((FILE **)file_p) = fp;
    return NULL == fp ? LV_FS_RES_UNKNOWN : LV_FS_RES_OK;
}

static lv_fs_res_t close_cb(struct _lv_fs_drv_t *drv, void *file_p)
{
    (void)drv;

    FILE *fp = *((FILE **)file_p);
    fclose(fp);
    return LV_FS_RES_OK;
}

static lv_fs_res_t read_cb(struct _lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br)
{
    (void)drv;

    FILE *fp = *((FILE **)file_p);
    *br = fread(buf, 1, btr, fp);
    return (*br <= 0) ? LV_FS_RES_UNKNOWN : LV_FS_RES_OK;
}

static lv_fs_res_t seek_cb(struct _lv_fs_drv_t *drv, void *file_p, uint32_t pos)
{
    (void)drv;

    FILE *fp = *((FILE **)file_p);
    fseek(fp, pos, SEEK_SET);

    return LV_FS_RES_OK;
}

static lv_fs_res_t tell_cb(struct _lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
    (void)drv;

    FILE *fp = *((FILE **)file_p);
    *pos_p = ftell(fp);

    return LV_FS_RES_OK;
}

static bool ready_cb(struct _lv_fs_drv_t *drv)
{
    (void)drv;
    return true;
}

static void lvgl_init(void)
{
    xGuiSemaphore = xSemaphoreCreateMutex();
    assert(xGuiSemaphore);

    lv_init();

    /* Initialize SPI or I2C bus used by the drivers */
    lvgl_driver_init();

    lv_color_t *buf1 = heap_caps_malloc(DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1 != NULL);

    /* Use double buffered when not working with monochrome displays */
#ifndef CONFIG_LV_TFT_DISPLAY_MONOCHROME
    lv_color_t *buf2 = heap_caps_malloc(DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf2 != NULL);
#else
    static lv_color_t *buf2 = NULL;
#endif

    static lv_disp_buf_t disp_buf;

    uint32_t size_in_px = DISP_BUF_SIZE;

#if defined CONFIG_LV_TFT_DISPLAY_CONTROLLER_IL3820 || defined CONFIG_LV_TFT_DISPLAY_CONTROLLER_JD79653A || defined CONFIG_LV_TFT_DISPLAY_CONTROLLER_UC8151D || defined CONFIG_LV_TFT_DISPLAY_CONTROLLER_SSD1306

    /* Actual size in pixels, not bytes. */
    size_in_px *= 8;
#endif

    /* Initialize the working buffer depending on the selected display.
     * NOTE: buf2 == NULL when using monochrome displays. */
    lv_disp_buf_init(&disp_buf, buf1, buf2, size_in_px);

    lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = disp_driver_flush;

    /* When using a monochrome display we need to register the callbacks:
     * - rounder_cb
     * - set_px_cb */
#ifdef CONFIG_LV_TFT_DISPLAY_MONOCHROME
    disp_drv.rounder_cb = disp_driver_rounder;
    disp_drv.set_px_cb = disp_driver_set_px;
#endif

    disp_drv.buffer = &disp_buf;
    lv_disp_drv_register(&disp_drv);

    /* Create and start a periodic timer interrupt to call lv_tick_inc */
    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &lv_tick_task,
        .name = "periodic_gui"};
    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, LV_TICK_PERIOD_MS * 1000));

    static lv_fs_drv_t drv;
    lv_fs_drv_init(&drv); /*Basic initialization*/

    drv.letter = 'S';               /*An uppercase letter to identify the drive */
    drv.file_size = sizeof(FILE *); /*Size required to store a file object*/
    drv.ready_cb = ready_cb;        /*Callback to tell if the drive is ready to use */
    drv.open_cb = open_cb;          /*Callback to open a file */
    drv.close_cb = close_cb;        /*Callback to close a file */
    drv.read_cb = read_cb;          /*Callback to read a file */
    drv.seek_cb = seek_cb;          /*Callback to seek in a file (Move cursor) */
    drv.tell_cb = tell_cb;          /*Callback to tell the cursor position  */

    lv_fs_drv_register(&drv); /*Finally register the drive*/
}

void app_main(void)
{
    BaseType_t st;
    esp_err_t ret;
    size_t x, y;
    uint16_t dx, dy;
    char filename[64];
    int n;
    // Options for mounting the filesystem.
    // If format_if_mount_failed is set to true, SD card will be partitioned and
    // formatted in case when mounting fails.
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif // EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .max_files = 5,
        .allocation_unit_size = 16 * 1024};
    sdmmc_card_t *card;
    const char mount_point[] = MOUNT_POINT;

    lvgl_init();

    ESP_LOGI(TAG, "Initializing SD card");

    // Use settings defined above to initialize SD card and mount FAT filesystem.
    // Note: esp_vfs_fat_sdmmc/sdspi_mount is all-in-one convenience functions.
    // Please check its source code and implement error recovery when developing
    // production applications.
#ifndef USE_SPI_MODE
    ESP_LOGI(TAG, "Using SDMMC peripheral");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    // To use 1-line SD mode, uncomment the following line:
    // slot_config.width = 1;

    // GPIOs 15, 2, 4, 12, 13 should have external 10k pull-ups.
    // Internal pull-ups are not sufficient. However, enabling internal pull-ups
    // does make a difference some boards, so we do that here.
    gpio_set_pull_mode(15, GPIO_PULLUP_ONLY); // CMD, needed in 4- and 1- line modes
    gpio_set_pull_mode(2, GPIO_PULLUP_ONLY);  // D0, needed in 4- and 1-line modes
    gpio_set_pull_mode(4, GPIO_PULLUP_ONLY);  // D1, needed in 4-line mode only
    gpio_set_pull_mode(12, GPIO_PULLUP_ONLY); // D2, needed in 4-line mode only
    gpio_set_pull_mode(13, GPIO_PULLUP_ONLY); // D3, needed in 4- and 1-line modes

    ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);
#else
    ESP_LOGI(TAG, "Using SPI peripheral");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
#endif //USE_SPI_MODE

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                          "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                          "Make sure SD card lines have pull-up resistors in place.",
                     esp_err_to_name(ret));
        }
        return;
    }

    // Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, card);

    lv_obj_t *scr = lv_disp_get_scr_act(NULL);

    static lv_obj_t *tile;
    tile = lv_img_create(scr, NULL);
    lv_img_set_auto_size(tile, true);

    static lv_obj_t *button;
    button = lv_btn_create(scr, NULL);
    lv_obj_set_size(button, CONFIG_LV_HOR_RES_MAX - 10, CONFIG_LV_VER_RES_MAX / 4);

    lv_obj_set_style_local_bg_color(button, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_GREEN);
    lv_obj_set_style_local_bg_opa(button, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_OPA_40);
    lv_obj_set_style_local_border_color(button, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_BLACK);
    lv_obj_align(button, NULL, LV_ALIGN_IN_BOTTOM_MID, 0, 0);

    static lv_obj_t *label;
    label = lv_label_create(button, NULL);
    lv_obj_set_style_local_text_font(label, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, &lv_font_montserrat_14);
    lv_obj_set_style_local_text_color(label, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_BLACK);
    lv_obj_align(label, NULL, LV_ALIGN_CENTER, 5, 5);

    static lv_style_t style_line;
    lv_style_init(&style_line);
    lv_style_set_line_width(&style_line, LV_STATE_DEFAULT, 4);
    lv_style_set_line_color(&style_line, LV_STATE_DEFAULT, LV_COLOR_BLUE);
    lv_style_set_line_rounded(&style_line, LV_STATE_DEFAULT, true);

    lv_point_t line_points1[2] = {{0, 0}, {10, 0}};
    lv_obj_t *line1;
    line1 = lv_line_create(scr, NULL);
    lv_obj_add_style(line1, LV_LINE_PART_MAIN, &style_line);
    lv_line_set_points(line1, line_points1, 2);

    lv_point_t line_points2[2] = {{0, 0}, {0, 10}};
    lv_obj_t *line2;
    line2 = lv_line_create(scr, NULL);
    lv_obj_add_style(line2, LV_LINE_PART_MAIN, &style_line);
    lv_line_set_points(line2, line_points2, 2);

    st = xTaskCreatePinnedToCore(lvgl_task, "lvgl_task", 4096 * 2, NULL, 0, NULL, 0);
    assert(st == pdPASS);

#define OFFSET (-(TILE_SIZE - 240) / 2)

    while (1)
    {
        for (uint8_t z = 0; z <= MAX_ZOOM_LEVEL; z++)
        {
            deg2num(LOC_LAT, LOC_LON, z, &x, &y, &dx, &dy);
            ESP_LOGD(TAG, "(%d, %d) (%d, %d)", x, y, dx, dy);
            n = snprintf(filename, sizeof(filename), "S:" MOUNT_POINT "/" MAP_NAME "/%d/%d/%d.bin", z, x, y);
            assert(n < sizeof(filename));
            ESP_LOGI(TAG, "Drawing image: %s", filename);

            if (pdTRUE == xSemaphoreTake(xGuiSemaphore, portMAX_DELAY))
            {
                lv_img_set_src(tile, filename);

                lv_obj_align(line1, NULL, LV_ALIGN_CENTER, dx + OFFSET - (TILE_SIZE / 2), dy + OFFSET - (TILE_SIZE / 2));
                lv_obj_align(line2, NULL, LV_ALIGN_CENTER, dx + OFFSET - (TILE_SIZE / 2), dy + OFFSET - (TILE_SIZE / 2));
                lv_obj_align(tile, NULL, LV_ALIGN_CENTER, OFFSET, OFFSET);
                lv_label_set_text_fmt(label, "Lon: %lf X: %d\nLat: %lf Y: %d\nZoom: %d",
                                      LOC_LAT, x, LOC_LON, y, z);
                xSemaphoreGive(xGuiSemaphore);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    // All done, unmount partition and disable SDMMC or SPI peripheral
    esp_vfs_fat_sdcard_unmount(mount_point, card);
    ESP_LOGI(TAG, "Card unmounted");
}
