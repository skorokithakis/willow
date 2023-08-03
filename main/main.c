#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "periph_spiffs.h"
#include "sdkconfig.h"

#include "audio.h"
#include "config.h"
#include "display.h"
#include "input.h"
#include "log.h"
#include "network.h"
#include "shared.h"
#include "slvgl.h"
#include "system.h"
#include "tasks.h"
#include "timer.h"
#include "ui.h"
#include "was.h"

#include "endpoint/hass.h"

#if defined(CONFIG_WILLOW_ETHERNET)
#include "net/ethernet.h"
#endif

#define DEFAULT_WIS_URL "https://infer.tovera.io/api/willow"

#define I2S_PORT       I2S_NUM_0
#define PARTLABEL_USER "user"

char was_url[2048];
static const char *TAG = "WILLOW/MAIN";

static esp_err_t init_spiffs_user(void)
{
    esp_err_t ret = ESP_OK;
    periph_spiffs_cfg_t pcfg_spiffs_user = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .partition_label = PARTLABEL_USER,
        .root = "/spiffs/user",
    };
    esp_periph_handle_t phdl_spiffs_user = periph_spiffs_init(&pcfg_spiffs_user);
    ret = esp_periph_start(hdl_pset, phdl_spiffs_user);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to start spiffs user peripheral: %s", esp_err_to_name(ret));
        return ret;
    }

    while (!periph_spiffs_is_mounted(phdl_spiffs_user)) {
        ESP_LOGI(TAG, "Waiting on SPIFFS mount...");
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
    ESP_LOGI(TAG, "SPIFFS mounted");

    return ret;
}

#ifdef CONFIG_HEAP_TASK_TRACKING
#include "esp_heap_task_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_TASK_NUM  32 // Max number of per tasks info that it can store
#define MAX_BLOCK_NUM 32 // Max number of per block info that it can store

static size_t s_prepopulated_num = 0;
static heap_task_totals_t s_totals_arr[MAX_TASK_NUM];
static heap_task_block_t s_block_arr[MAX_BLOCK_NUM];

static void esp_dump_per_task_heap_info(void)
{
    heap_task_info_params_t heap_info = {0};
    heap_info.caps[0] = MALLOC_CAP_8BIT; // Gets heap with CAP_8BIT capabilities
    heap_info.mask[0] = MALLOC_CAP_8BIT;
    heap_info.caps[1] = MALLOC_CAP_32BIT; // Gets heap info with CAP_32BIT capabilities
    heap_info.mask[1] = MALLOC_CAP_32BIT;
    heap_info.tasks = NULL; // Passing NULL captures heap info for all tasks
    heap_info.num_tasks = 0;
    heap_info.totals = s_totals_arr; // Gets task wise allocation details
    heap_info.num_totals = &s_prepopulated_num;
    heap_info.max_totals = MAX_TASK_NUM; // Maximum length of "s_totals_arr"
    heap_info.blocks
        = s_block_arr; // Gets block wise allocation details. For each block, gets owner task, address and siz
    heap_info.max_blocks = MAX_BLOCK_NUM; // Maximum length of "s_block_arr"

    heap_caps_get_per_task_info(&heap_info);

    for (int i = 0; i < *heap_info.num_totals; i++) {
        printf("Task: %s -> CAP_8BIT: %d CAP_32BIT: %d\n",
               heap_info.totals[i].task != NULL ? pcTaskGetTaskName(heap_info.totals[i].task) : "Pre-Scheduler allocs",
               heap_info.totals[i].size[0],  // Heap size with CAP_8BIT capabilities
               heap_info.totals[i].size[1]); // Heap size with CAP32_BIT capabilities
    }

    printf("\n\n");
}

#endif

void app_main(void)
{
    state = STATE_INIT;
    esp_err_t ret;

    init_logging();
    ESP_LOGI(TAG, "Starting up! Please wait...");

    esp_periph_config_t pcfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    hdl_pset = esp_periph_set_init(&pcfg);

    init_system();
    init_spiffs_user();
    config_parse();
    init_display();
    init_lvgl_display();
    init_ui();

    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

#ifdef CONFIG_WILLOW_ETHERNET
    init_ethernet();
#else
    nvs_handle_t hdl_nvs;
    ret = nvs_open("WIFI", NVS_READONLY, &hdl_nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to open NVS namespace WIFI: %s", esp_err_to_name(err));
        goto err_nvs;
    }

    char psk[64];
    size_t sz = sizeof(psk);
    ret = nvs_get_str(hdl_nvs, "PSK", psk, &sz);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to get PSK from NVS namespace WIFI: %s", esp_err_to_name(err));
        goto err_nvs;
    }

    char ssid[33];
    sz = sizeof(ssid);
    ret = nvs_get_str(hdl_nvs, "SSID", ssid, &sz);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to get PSK from NVS namespace WIFI: %s", esp_err_to_name(err));
        goto err_nvs;
    }
    init_wifi(psk, ssid);
#endif

    ret = nvs_open("WAS", NVS_READONLY, &hdl_nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to open NVS namespace WAS: %s", esp_err_to_name(err));
        goto err_nvs;
    }
    sz = sizeof(was_url);
    ret = nvs_get_str(hdl_nvs, "URL", was_url, &sz);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to get WASL URL from NVS namespace WAS: %s", esp_err_to_name(err));
        goto err_nvs;
    }
    state = STATE_NVS_OK;
    ret = init_was();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize Willow Application Server connection");
        if (ld == NULL) {
            ui_pr_err("Fatal error!", "WAS initialization failed.");
        }
    }

    if (!config_valid) {
        request_config();
        // wait "indefinitely"
        vTaskDelay(portMAX_DELAY);
    }

// we jump over WAS initialization was without Wi-Fi this will never work
err_nvs:
    if (state < STATE_NVS_OK) {
        if (ld == NULL) {
            ui_pr_err("Fatal error!", "Failed to read NVS partition.");
        }
        // wait "indefinitely"
        vTaskDelay(portMAX_DELAY);
    }

    init_sntp();

    char *command_endpoint = config_get_char("command_endpoint", DEFAULT_COMMAND_ENDPOINT);
    if (strcmp(command_endpoint, "Home Assistant") == 0) {
        init_hass();
    }
    free(command_endpoint);
    init_buttons();
    init_input_key_service();
    init_audio();
    init_lvgl_touch();
    init_display_timer();

#ifndef CONFIG_WILLOW_ETHERNET
    get_mac_address(); // should be on wifi by now; print the MAC
#endif

    const esp_app_desc_t *app_desc = esp_ota_get_app_description();
    ESP_LOGI(TAG, "Startup complete! Version: %s. Waiting for wake word.", app_desc->version);

    // if we reached this point, we can mark the current partition valid
    // we can still crash on wake or other events but we should be able to do another OTA
    // we can also still crash in the while loop below - this should be improved
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ota_mark_app_valid_cancel_rollback());

    ESP_ERROR_CHECK_WITHOUT_ABORT(reset_timer(hdl_display_timer, DISPLAY_TIMEOUT_US, false));

#ifdef CONFIG_WILLOW_DEBUG_RUNTIME_STATS
    xTaskCreate(&task_debug_runtime_stats, "dbg_runtime_stats", 4 * 1024, NULL, 0, NULL);
#endif

    while (true) {
#ifdef CONFIG_WILLOW_DEBUG_MEM
        printf("MALLOC_CAP_INTERNAL:\n");
        heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
        printf("MALLOC_CAP_SPIRAM:\n");
        heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
#endif
#ifdef CONFIG_WILLOW_DEBUG_TASKS
        char buf[128];
        vTaskList(buf);
        printf("%s\n", buf);
#endif
#ifdef CONFIG_WILLOW_DEBUG_TIMERS
        (esp_timer_dump(stdout));
#endif
        ESP_LOGI(TAG, "min_free_iram: %d", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        ESP_LOGI(TAG, "min_free_spiram: %d", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "checking heap integrity");
        heap_caps_check_integrity_all(true);
#ifdef CONFIG_HEAP_TASK_TRACKING
        esp_dump_per_task_heap_info();
#endif
    }
}
