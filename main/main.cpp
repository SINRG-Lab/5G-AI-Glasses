/**
 * @file main.cpp
 * @author Ryder Paulson <paulson.r@northeastern.edu>
 * @brief 
 *
 * @section DESCRIPTION
 *
 * 
 */

// Standard C components
#include <stdio.h>
#include <string.h>
#include <dirent.h>

// FreeRTOS components
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ESP components
#include "esp_system.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_webrtc.h"
#include "esp_spiffs.h"
#include "driver/uart.h"

// Walter modem component from 
#include "WalterModem.h"

// Unique component to this project
#include "WalterCOMM.h"

// The TLS profile to use for the application
static const int HTTPS_TLS_PROFILE = 2;

// The HTTPS Profile
static const int MODEM_HTTPS_PROFILE = 1;

static const char* TAG = "WALTER";

/**
 * @brief Inititializes the on board storage for the config and audio file.
 */
void init_spiffs(void) 
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }
    
    size_t total = 0, used = 0;
    ret = esp_spiffs_info("storage", &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
}


void list_spiffs_files(void)
{
    DIR *dir = opendir("/spiffs");
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory");
        return;
    }
    
    struct dirent *entry;
    ESP_LOGI(TAG, "Files in SPIFFS:");
    while ((entry = readdir(dir)) != NULL) {
        ESP_LOGI(TAG, "  - %s", entry->d_name);
    }
    closedir(dir);
}

/**
 * @brief Main application entry point
 */
extern "C" void app_main(void) 
{
  ESP_LOGI(TAG, "\n\n=== Networked-5G-AI-Glasses ===\n");

  // Wait for system to stabilize
  vTaskDelay(pdMS_TO_TICKS(2000));

  // Initilize and verify on-board storage
  init_spiffs();
  list_spiffs_files();

  const int https_port = 443;

  // Test routing info
  const char* example_https_host = "quickspot.io";
  const char* example_https_post_endpoint = "/hello/post";

  // Local modem response object
  static WalterModemRsp rsp = {};

  // Start the modem - Using UART2 (same as Arduino Serial2)
  if (WalterModem::begin(static_cast<uart_port_t>(UART_NUM_2))) {
    ESP_LOGI(TAG, "Successfully initialized the modem");
  } else {
    ESP_LOGE(TAG, "Could not initialize the modem");
    return;
  }

  // Connect the modem to the lte network
  if(!comm::lteConnect()) {
    ESP_LOGE(TAG, "Could not Connect to LTE");
    return;
  }

  // Check the quality of the network connection
  if(comm::modem.getCellInformation(WALTER_MODEM_SQNMONI_REPORTS_SERVING_CELL, &rsp)) {
    WalterModemCellInformation &cellInfo = rsp.data.cellInformation;

    ESP_LOGI(TAG, "Cell Information:");
    ESP_LOGI(TAG, "-> netName: %s", cellInfo.netName);
    ESP_LOGI(TAG, "-> cc: %u", cellInfo.cc);
    ESP_LOGI(TAG, "-> nc: %u", cellInfo.nc);
    ESP_LOGI(TAG, "-> rsrp: %.2f", cellInfo.rsrp);
    ESP_LOGI(TAG, "-> cinr: %.2f", cellInfo.cinr);
    ESP_LOGI(TAG, "-> rsrq: %.2f", cellInfo.rsrq);
    ESP_LOGI(TAG, "-> tac: %u", cellInfo.tac);
    ESP_LOGI(TAG, "-> pci: %u", cellInfo.pci);
    ESP_LOGI(TAG, "-> earfcn: %u", cellInfo.earfcn);
    ESP_LOGI(TAG, "-> rssi: %.2f", cellInfo.rssi);
    ESP_LOGI(TAG, "-> paging: %u", cellInfo.paging);
    ESP_LOGI(TAG, "-> cid: %u", cellInfo.cid);
    ESP_LOGI(TAG, "-> band: %u", cellInfo.band);
    ESP_LOGI(TAG, "-> bw: %u", cellInfo.bw);
    ESP_LOGI(TAG, "-> ceLevel: %u", cellInfo.ceLevel);
  } else {
    ESP_LOGI(TAG, "Failed to get cell information.");
  }

  // Set up the TLS profile
  if(comm::setupTLSProfile(HTTPS_TLS_PROFILE)) {
    ESP_LOGI(TAG, "TLS Profile setup succeeded");
  } else {
    ESP_LOGE(TAG, "TLS Profile setup failed");
    return;
  }

  // Configure the HTTPS profile
  if(comm::modem.httpConfigProfile(MODEM_HTTPS_PROFILE, example_https_host, https_port, HTTPS_TLS_PROFILE)) {
    ESP_LOGI(TAG, "Successfully configured the HTTPS profile");
  } else {
    ESP_LOGE(TAG, "Failed to configure HTTPS profile");
  }

  // TODO Read in audio from partitioned file
  

  // Open WebRTC connection
  

  // Send audio to OpenAI


}