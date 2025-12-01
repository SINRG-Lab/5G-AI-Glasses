/**
 * @file walter_spiffs.cpp
 * @brief Implementation of SPIFFS filesystem utilities
 *
 * @section DESCRIPTION
 *
 * Implements SPIFFS initialization, file listing, configuration loading,
 * and custom audio stream elements for ESP-ADF pipelines.
 */

#include "walter_spiffs.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "mbedtls/base64.h"
#include "audio_element.h"
#include "cJSON.h"

namespace spiffs {

namespace {
constexpr const char* kTag = "spiffs";
}  // namespace

void Init() {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(kTag, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(kTag, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(kTag, "Failed to initialize SPIFFS (%s)", 
                    esp_err_to_name(ret));
        }
        return;
    }
    
    size_t total = 0;
    size_t used = 0;
    ret = esp_spiffs_info("storage", &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(kTag, "Partition size: total: %d, used: %d", total, used);
    }
}

void ListFiles() {
    DIR* dir = opendir("/spiffs");
    if (dir == NULL) {
        ESP_LOGE(kTag, "Failed to open directory");
        return;
    }
    
    struct dirent* entry;
    ESP_LOGI(kTag, "Files in SPIFFS:");
    while ((entry = readdir(dir)) != NULL) {
        ESP_LOGI(kTag, "  - %s", entry->d_name);
    }
    closedir(dir);
}

bool LoadConfig(char* api_key, size_t api_key_size,
                char* model, size_t model_size) {
    constexpr const char* kConfigPath = "/spiffs/config.json";
    constexpr size_t kMaxConfigSize = 4096;
    
    // Open config file
    FILE* file = fopen(kConfigPath, "r");
    if (!file) {
        ESP_LOGE(kTag, "Failed to open config file: %s", kConfigPath);
        return false;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size > kMaxConfigSize) {
        ESP_LOGE(kTag, "Invalid config file size: %ld", file_size);
        fclose(file);
        return false;
    }
    
    // Read file content
    char* json_string = static_cast<char*>(malloc(file_size + 1));
    if (!json_string) {
        ESP_LOGE(kTag, "Failed to allocate memory for config");
        fclose(file);
        return false;
    }
    
    size_t bytes_read = fread(json_string, 1, file_size, file);
    fclose(file);
    
    if (bytes_read != static_cast<size_t>(file_size)) {
        ESP_LOGE(kTag, "Failed to read config file");
        free(json_string);
        return false;
    }
    
    json_string[file_size] = '\0';
    ESP_LOGI(kTag, "Config file loaded (%ld bytes)", file_size);
    
    // Parse JSON
    cJSON* root = cJSON_Parse(json_string);
    free(json_string);
    
    if (!root) {
        const char* error_ptr = cJSON_GetErrorPtr();
        if (error_ptr) {
            ESP_LOGE(kTag, "JSON parse error: %s", error_ptr);
        }
        return false;
    }
    
    // Extract OpenAI configuration
    cJSON* openai = cJSON_GetObjectItem(root, "openai");
    if (!openai) {
        ESP_LOGE(kTag, "Missing 'openai' object in config");
        cJSON_Delete(root);
        return false;
    }
    
    // Get API key
    cJSON* api_key_json = cJSON_GetObjectItem(openai, "api_key");
    if (!api_key_json || !cJSON_IsString(api_key_json)) {
        ESP_LOGE(kTag, "Missing or invalid 'api_key' in config");
        cJSON_Delete(root);
        return false;
    }
    
    // Get model
    cJSON* model_json = cJSON_GetObjectItem(openai, "model");
    if (!model_json || !cJSON_IsString(model_json)) {
        ESP_LOGE(kTag, "Missing or invalid 'model' in config");
        cJSON_Delete(root);
        return false;
    }
    
    // Copy to output buffers
    strncpy(api_key, api_key_json->valuestring, api_key_size - 1);
    api_key[api_key_size - 1] = '\0';
    
    strncpy(model, model_json->valuestring, model_size - 1);
    model[model_size - 1] = '\0';
    
    ESP_LOGI(kTag, "OpenAI config loaded:");
    ESP_LOGI(kTag, "  Model: %s", model);
    ESP_LOGI(kTag, "  API Key: %.*s...", 10, api_key);
    
    cJSON_Delete(root);
    return true;
}

namespace {

// Stream element callback for reading from SPIFFS
int SpiffsRead(audio_element_handle_t el, char* buffer, int len,
               TickType_t wait_time, void* ctx) {
    FILE* file = static_cast<FILE*>(audio_element_getdata(el));
    if (!file) {
        return AEL_IO_FAIL;
    }
    
    int bytes_read = fread(buffer, 1, len, file);
    if (bytes_read == 0) {
        if (feof(file)) {
            return AEL_IO_DONE;
        }
        return AEL_IO_FAIL;
    }
    
    return bytes_read;
}

// Stream element callback for opening file
esp_err_t SpiffsOpen(audio_element_handle_t el) {
    const char* uri = audio_element_get_uri(el);
    if (!uri) {
        ESP_LOGE(kTag, "No URI provided");
        return ESP_FAIL;
    }
    
    ESP_LOGI(kTag, "Opening SPIFFS file: %s", uri);
    FILE* file = fopen(uri, "rb");
    if (!file) {
        ESP_LOGE(kTag, "Failed to open file: %s", uri);
        return ESP_FAIL;
    }
    
    audio_element_setdata(el, file);
    return ESP_OK;
}

// Stream element callback for closing file
esp_err_t SpiffsClose(audio_element_handle_t el) {
    FILE* file = static_cast<FILE*>(audio_element_getdata(el));
    if (file) {
        fclose(file);
        audio_element_setdata(el, NULL);
    }
    return ESP_OK;
}

}  // namespace

audio_element_handle_t StreamInit() {
    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open = SpiffsOpen;
    cfg.close = SpiffsClose;
    cfg.read = SpiffsRead;
    cfg.process = NULL;
    cfg.destroy = NULL;
    cfg.task_stack = 2048;
    cfg.task_prio = 5;
    cfg.task_core = 0;
    cfg.out_rb_size = 8 * 1024;
    cfg.tag = "spiffs";
    
    audio_element_handle_t el = audio_element_init(&cfg);
    if (!el) {
        ESP_LOGE(kTag, "Failed to create SPIFFS stream element");
        return NULL;
    }
    
    return el;
}

}