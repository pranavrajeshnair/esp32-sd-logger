#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <errno.h> 

static const char *TAG = "SD_FIXER";
#define MOUNT_POINT "/sdcard"

// --- PINS (ESP32-S3) ---
#define PIN_NUM_CS    10
#define PIN_NUM_MOSI  11
#define PIN_NUM_CLK   12
#define PIN_NUM_MISO  13

// --- DATA STRUCTURE ---
typedef struct {
    char filepath[64];
    char data[256];
} sd_msg_t;

QueueHandle_t sd_queue_handle = NULL;
sdmmc_card_t *card;


// --- HELPER 1: Create Folder if Missing ---
void ensure_folder(const char *path) {
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        if (mkdir(path, 0777) == 0) {
            ESP_LOGI(TAG, "SUCCESS: Created folder %s", path);
        } else {
            ESP_LOGE(TAG, "ERROR: Could not create %s. Reason: %s", path, strerror(errno));
        }
    }
}

// --- HELPER 2: Read Specific File (NEW ADDITION) ---
void read_specific_file(const char *path) {
    ESP_LOGI(TAG, "Attempting to read file: %s", path);
    
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Could not open file for reading. (Does '%s' exist?)", path);
        return;
    }

    char buffer[128]; // Buffer to hold text line by line
    printf("\n---------- FILE CONTENT START ----------\n");
    
    // Read line by line until end of file
    while (fgets(buffer, sizeof(buffer), f) != NULL) {
        printf("%s", buffer);
    }
    
    printf("\n----------- FILE CONTENT END -----------\n");
    fclose(f);
}


// --- CONSUMER TASK (SD CARD MANAGER) ---
void sd_card_worker_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Initializing SD Card...");

    // 1. SETUP MOUNT CONFIG
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true, 
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    // 2. SETUP SPI BUS
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    
    spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);

    // 3. MOUNT THE CARD
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = SPI2_HOST;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 400; 

    esp_err_t ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card. Error: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "SD Card Mounted Successfully!");
    
    // 4. CREATE FOLDERS
    ensure_folder("/sdcard/audio");
    ensure_folder("/sdcard/data");
    ensure_folder("/sdcard/config");

    // ---------------------------------------------------------
    // *** NEW STEP: READ THE FILE NOW (Before entering loop) ***
    // ---------------------------------------------------------
    // Try to read "file.txt" inside "data" folder
    read_specific_file("/sdcard/data/file.txt"); 

    // If your file is named just "file" without .txt, uncomment this line:
    // read_specific_file("/sdcard/data/file");


    // 5. PROCESS QUEUE LOOP (Your existing write logic)
    sd_msg_t incoming_msg;
    while (1) {
        if (xQueueReceive(sd_queue_handle, &incoming_msg, portMAX_DELAY) == pdTRUE) {
            
            FILE *f = fopen(incoming_msg.filepath, "a");
            
            if (f == NULL) {
                ESP_LOGE(TAG, "Write Failed: %s | Reason: %s", incoming_msg.filepath, strerror(errno));
            } else {
                fprintf(f, "%s\n", incoming_msg.data);
                fclose(f);
                ESP_LOGI(TAG, "Saved: %s", incoming_msg.filepath);
            }
        }
    }
}


// --- PRODUCER (MAIN LOOP) ---
void app_main(void)
{
    sd_queue_handle = xQueueCreate(10, sizeof(sd_msg_t));
    xTaskCreate(sd_card_worker_task, "sd_worker", 8192, NULL, 5, NULL);

    int counter = 0;
    while (1) {
        sd_msg_t msg;

        // Write to log.txt
        sprintf(msg.filepath, "/sdcard/data/log.txt");
        sprintf(msg.data, "Timestamp: %d, Sensor: OK", counter);
        xQueueSend(sd_queue_handle, &msg, 0);

        // Write to config/settings.cfg
        if (counter % 5 == 0) {
            sprintf(msg.filepath, "/sdcard/config/settings.cfg");
            sprintf(msg.data, "UPDATE_ID=%d", counter);
            xQueueSend(sd_queue_handle, &msg, 0);
        }

        counter++;
        vTaskDelay(pdMS_TO_TICKS(1000)); 
    }
}