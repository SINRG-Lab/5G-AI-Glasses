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
#include <optional>

// FreeRTOS components
#include "freertos/FreeRTOS.h"

// ESP components
#include "esp_system.h"
#include "esp_log.h"
#include "esp_spiffs.h"

// Project libraries
#include "walter_com.h"
#include "audio_agent.h"
#include "walter_spiffs.h"

// Local WiFi Credentials
#include "wifi_credentials.h"

// The TLS profile to use for the application
static const int HTTPS_TLS_PROFILE = 2;

static const char* TAG = "main";

void PrintCellInfo(WalterModemCellInformation* cellInfo)
{
    ESP_LOGI(TAG, "Cell Information:");
    ESP_LOGI(TAG, "-> netName: %s", cellInfo->netName);
    ESP_LOGI(TAG, "-> cc: %u", cellInfo->cc);
    ESP_LOGI(TAG, "-> nc: %u", cellInfo->nc);
    ESP_LOGI(TAG, "-> rsrp: %.2f", cellInfo->rsrp);
    ESP_LOGI(TAG, "-> cinr: %.2f", cellInfo->cinr);
    ESP_LOGI(TAG, "-> rsrq: %.2f", cellInfo->rsrq);
    ESP_LOGI(TAG, "-> tac: %u", cellInfo->tac);
    ESP_LOGI(TAG, "-> pci: %u", cellInfo->pci);
    ESP_LOGI(TAG, "-> earfcn: %u", cellInfo->earfcn);
    ESP_LOGI(TAG, "-> rssi: %.2f", cellInfo->rssi);
    ESP_LOGI(TAG, "-> paging: %u", cellInfo->paging);
    ESP_LOGI(TAG, "-> cid: %u", cellInfo->cid);
    ESP_LOGI(TAG, "-> band: %u", cellInfo->band);
    ESP_LOGI(TAG, "-> bw: %u", cellInfo->bw);
    ESP_LOGI(TAG, "-> ceLevel: %u", cellInfo->ceLevel);
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
    spiffs::Init();
    spiffs::ListFiles();

    // Load OpenAI configuration from SPIFFS
    char openai_api_key[256];
    char openai_model[64];
    
    if (!spiffs::LoadConfig(openai_api_key, sizeof(openai_api_key), openai_model, sizeof(openai_model))) {
        ESP_LOGE(TAG, "Failed to load OpenAI configuration");
        return;
    }

    // Local modem response object
    static WalterModemRsp rsp = {};

    // Start the modem - Using UART2 (same as Arduino Serial2)
    if (WalterModem::begin(static_cast<uart_port_t>(UART_NUM_2))) {
        ESP_LOGI(TAG, "Successfully initialized the modem");
    } else {
        ESP_LOGE(TAG, "Could not initialize the modem");
        return;
    }

    // Network connection characteristics 
    const char* network_ssid = WIFI_SSID;
    const char* network_password = WIFI_PASSWORD;
    const int timeout_ms = 30000;

    // Connect to WiFi first, fallback to Cellular
    if(com::WiFiConnect(network_ssid, network_password, timeout_ms)) {
        ESP_LOGI(TAG, "Connected via WiFi");
    } else {     
        ESP_LOGI(TAG, "WiFi failed, trying cellular...");
        if(com::LTEConnect()) {
            ESP_LOGI(TAG, "Connected via Cellular");
        } else {
            ESP_LOGE(TAG, "Could not connect to WiFi or Cellular");
            return;
        }
    }

    // Log connection status
    ESP_LOGI(TAG, "Connection type: %s", 
            com::GetConnectionType() == com::CONN_WIFI ? "WiFi" : 
            com::GetConnectionType() == com::CONN_CELLULAR ? "Cellular" : "None");

    // Check the quality of the network connection
    if(com::modem.getCellInformation(WALTER_MODEM_SQNMONI_REPORTS_SERVING_CELL, &rsp)) {
        WalterModemCellInformation &cellInfo = rsp.data.cellInformation;
        PrintCellInfo(&cellInfo);
    } else {
        ESP_LOGI(TAG, "Failed to get cell information.");
    }

    // Set up the TLS profile
    if(com::SetupTLSProfile(HTTPS_TLS_PROFILE)) {
        ESP_LOGI(TAG, "TLS Profile setup succeeded");
    } else {
        ESP_LOGE(TAG, "TLS Profile setup failed");
        return;
    }

    // Open WebSocket connection
    ESP_LOGI(TAG, "Setting up WebSocket TLS profile...");
    if (!com::SetupWebSocketTLS(HTTPS_TLS_PROFILE)) {
        ESP_LOGE(TAG, "Failed to setup WebSocket TLS");
        return;
    }

    // Send audio to OpenAI in Chunks to reduce memory usage.
    const char* audio_file = "/spiffs/msg.wav";
    
    // Send audio file and receive response
    ESP_LOGI(TAG, "Sending audio to OpenAI and waiting for response...");
    std::optional<std::vector<uint8_t>> audio_response = audio_agent::SendAndRecieveAudio(
        openai_api_key,
        openai_model,
        nullptr,        // No raw audio stream
        0,              // No stream length
        audio_file,     // Use file instead
        true            // Print detailed response
    );

    if (audio_response.has_value()) {
        ESP_LOGI(TAG, "Successfully received audio response (%u bytes)", 
                audio_response->size()); 
    } else {
        ESP_LOGE(TAG, "Failed to get audio response from OpenAI");
    }

    ESP_LOGI(TAG, "Application complete");
}