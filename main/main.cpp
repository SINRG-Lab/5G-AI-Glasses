/**
 * @file main.cpp
 * @author Ryder Paulson <paulson.r@northeastern.edu>
 * @brief 
 *
 * @section DESCRIPTION
 *
 * 
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "WalterModem.h"
#include "WalterCOMM.h"

// The TLS profile to use for the application
static const int HTTPS_TLS_PROFILE = 2;

// The HTTPS Profile
static const int MODEM_HTTPS_PROFILE = 1;

static const char* TAG = "WALTER";

/**
 * @brief Main application entry point
 */
extern "C" void app_main(void) 
{
  ESP_LOGI(TAG, "\n\n=== Networked-5G-AI-Glasses ===\n");

  // Wait for system to stabilize
  vTaskDelay(pdMS_TO_TICKS(2000));

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
  
  // Generic POST
  const char example_jsonBody[] = "{\"hello\":\"walter\"}";
  if(!comm::httpsPost(example_https_post_endpoint, (const uint8_t*) example_jsonBody, strlen(example_jsonBody),
                "application/json", MODEM_HTTPS_PROFILE, example_https_host)) {
    ESP_LOGE(TAG, "HTTPS POST failed, restarting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
  }
}