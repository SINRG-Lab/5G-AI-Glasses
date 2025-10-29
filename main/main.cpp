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
#include "WalterHTTPS.h"

#define HTTPS_PORT 443
#define HTTPS_HOST "quickspot.io"
#define HTTPS_GET_ENDPOINT "/hello/get"
#define HTTPS_POST_ENDPOINT "/hello/post"

// The TLS profile to use for the application (1 is reserved for BlueCherry)
#define HTTPS_TLS_PROFILE 2

// The HTTPS Profile
#define MODEM_HTTPS_PROFILE 1

/**
 * @brief Main application entry point
 */
extern "C" void app_main(void) {
  ESP_LOGI(TAG, "\n\n=== WalterModem HTTPS example ===\n");

  // Wait for system to stabilize
  vTaskDelay(pdMS_TO_TICKS(2000));

  // Start the modem - Using UART2 (same as Arduino Serial2)
  if (WalterModem::begin(static_cast<uart_port_t>(UART_NUM_2))) {
    ESP_LOGI(TAG, "Successfully initialized the modem");
  } else {
    ESP_LOGE(TAG, "Could not initialize the modem");
    return;
  }

  // Connect the modem to the lte network
  if(!lteConnect()) {
    ESP_LOGE(TAG, "Could not Connect to LTE");
    return;
  }

  // Set up the TLS profile
  if(setupTLSProfile(HTTPS_TLS_PROFILE)) {
    ESP_LOGI(TAG, "TLS Profile setup succeeded");
  } else {
    ESP_LOGE(TAG, "TLS Profile setup failed");
    return;
  }

  // Configure the HTTPS profile
  if(modem.httpConfigProfile(MODEM_HTTPS_PROFILE, HTTPS_HOST, HTTPS_PORT, HTTPS_TLS_PROFILE)) {
    ESP_LOGI(TAG, "Successfully configured the HTTPS profile");
  } else {
    ESP_LOGE(TAG, "Failed to configure HTTPS profile");
  }
  
  // Generic POST
  const char jsonBody[] = "{\"hello\":\"walter\"}";
  if(!httpsPost(HTTPS_POST_ENDPOINT, (const uint8_t*) jsonBody, strlen(jsonBody),
                "application/json", MODEM_HTTPS_PROFILE, HTTPS_HOST)) {
    ESP_LOGE(TAG, "HTTPS POST failed, restarting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
  }
}