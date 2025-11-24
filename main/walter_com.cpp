/**
 * === Communications library for the Walter Board ===
 * @file WalterCOMM.cpp
 * @author Ryder Paulson <paulson.r@northeastern.edu>
 * @brief A set of communications tools for the Walter Board
 */
#include "esp_system.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"
#include "WalterModem.h"
#include "walter_com.h"
#include <string.h>

#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     5

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

// Local response object for Modem
static WalterModemRsp rsp = {};

// Local buffer for incoming HTTPS response
static uint8_t incomingBuf[1024] = { 0 };

static const char *TAG = "COMMUNICATIONS";

// ========================================
// CA CERTIFICATE (FROM DPTECHNIC'S HTTPS EXAMPLE)
// ========================================
static const char ca_cert[] = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)EOF";

/**
 * @brief Common routine to wait for and print an HTTP response.
 */
static bool WaitForHttpsResponse(uint8_t profile, const char* contentType) 
{
  ESP_LOGI(TAG, "Waiting for reply...");
  const uint16_t maxPolls = 30;
  for(uint16_t i = 0; i < maxPolls; i++) {
    if(com::modem.httpDidRing(profile, incomingBuf, sizeof(incomingBuf), &rsp)) {
      ESP_LOGI(TAG, "HTTPS status code (Modem): %d", rsp.data.httpResponse.httpStatus);
      ESP_LOGI(TAG, "Content type: %s", contentType);
      ESP_LOGI(TAG, "Payload:\n%s", incomingBuf);
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  ESP_LOGE(TAG, "HTTPS response timeout");
  return false;
}

namespace com{
WalterModem modem;

// Global connection state
ConnectionState g_connection_state = {CONN_NONE, false};

ConnectionType GetConnectionType() 
{
    return g_connection_state.type;
}

bool IsConnected() 
{
    return g_connection_state.is_connected;
}

// ========================================
// LTE Network Functions
// ========================================

/**
 * @brief This function checks if we are connected to the LTE network
 *
 * @return true when connected, false otherwise
 */
bool CheckLTEConnected() {
    WalterModemNetworkRegState regState = modem.getNetworkRegState();
    return (regState == WALTER_MODEM_NETWORK_REG_REGISTERED_HOME ||
            regState == WALTER_MODEM_NETWORK_REG_REGISTERED_ROAMING);
}

/**
 * @brief This function waits for the modem to be connected to the LTE network.
 *
 * @param timeout_sec The amount of seconds to wait before returning a time-out.
 *
 * @return true if connected, false on time-out.
 */
bool WaitForNetwork(int timeout_sec) 
{
    ESP_LOGI(TAG, "Connecting to the network...");
    int time = 0;
    while(!CheckLTEConnected()) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    time++;
    if(time > timeout_sec) {
        return false;
    }
    }
    ESP_LOGI(TAG, "Connected to the network");
    return true;
}

/**
 * @brief Disconnect from the LTE network.
 *
 * This function will disconnect the modem from the LTE network and block until
 * the network is actually disconnected. After the network is disconnected the
 * GNSS subsystem can be used.
 *
 * @return true on success, false on error.
 */
bool LTEDisconnect() 
{
    // Set the operational state to minimum
    if(modem.setOpState(WALTER_MODEM_OPSTATE_MINIMUM)) {
        ESP_LOGI(TAG, "Successfully set operational state to MINIMUM");
    } else {
        ESP_LOGE(TAG, "Could not set operational state to MINIMUM");
        return false;
    }

    // Wait for the network to become available
    WalterModemNetworkRegState regState = modem.getNetworkRegState();
    while(regState != WALTER_MODEM_NETWORK_REG_NOT_SEARCHING) {
        vTaskDelay(pdMS_TO_TICKS(100));
        regState = modem.getNetworkRegState();
    }

    ESP_LOGI(TAG, "Disconnected from the network");
    g_connection_state.type = CONN_NONE;
    g_connection_state.is_connected = false;
    return true;
}

/**
 * @brief This function tries to connect the modem to the cellular network.
 *
 * @return true on success, false on error.
 */
bool LTEConnect() 
{
    // Set the operational state to NO RF
    if(modem.setOpState(WALTER_MODEM_OPSTATE_NO_RF)) {
        ESP_LOGI(TAG, "Successfully set operational state to NO RF");
    } else {
        ESP_LOGE(TAG, "Could not set operational state to NO RF");
        return false;
    }

    // Create PDP context
    if(modem.definePDPContext()) {
        ESP_LOGI(TAG, "Created PDP context");
    } else {
        ESP_LOGE(TAG, "Could not create PDP context");
        return false;
    }

    // Set the operational state to full
    if(modem.setOpState(WALTER_MODEM_OPSTATE_FULL)) {
        ESP_LOGI(TAG, "Successfully set operational state to FULL");
    } else {
        ESP_LOGE(TAG, "Could not set operational state to FULL");
        return false;
    }

    // Set the network operator selection to automatic
    if(modem.setNetworkSelectionMode(WALTER_MODEM_NETWORK_SEL_MODE_AUTOMATIC)) {
        ESP_LOGI(TAG, "Network selection mode was set to automatic");
    } else {
        ESP_LOGE(TAG, "Could not set the network selection mode to automatic");
        return false;
    }

    // Wait for network registration
    if(!WaitForNetwork(300)) {
        ESP_LOGE(TAG, "Network registration timeout");
        return false;
    }

    // Attach to network
    if(!modem.setNetworkAttachmentState(true)) {
        ESP_LOGE(TAG, "Could not attach to network");
        return false;
    }

    // Wait for attachment to complete
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Verify PDP context is active
    if(modem.getPDPAddress(&rsp, NULL, NULL, 1)) {
        ESP_LOGI(TAG, "PDP context active with IP: %s", rsp.data.pdpAddressList.pdpAddress);
        g_connection_state.type = CONN_CELLULAR;
        g_connection_state.is_connected = true;
        return true;
    }

    ESP_LOGE(TAG, "No IP address assigned");
    g_connection_state.type = CONN_NONE;
    g_connection_state.is_connected = false;
    return false;
}

// ========================================
// WiFi Functions
// ========================================

/**
 * @brief WiFi event handler
 * 
 * @return NULL
 */
static void WiFiEventHandler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying connection to WiFi... (attempt %d/%d)", 
                    s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "Failed to connect to WiFi after %d attempts", WIFI_MAX_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * Connect to WiFi network
 * 
 * @param ssid WiFi network name (SSID)
 * @param password WiFi password (pass NULL or empty string for open networks)
 * @param timeout_ms Maximum time to wait for connection in milliseconds (0 = wait forever)
 * 
 * @return true if connection successful, false otherwise
 */
bool WiFiConnect(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (ssid == NULL || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "SSID cannot be NULL or empty");
        return false;
    }

    // Initialize NVS (required for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Create event group for WiFi events
    s_wifi_event_group = xEventGroupCreate();
    s_retry_num = 0;

    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create default WiFi station
    esp_netif_create_default_wifi_sta();

    // Initialize WiFi with default configuration
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WiFiEventHandler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &WiFiEventHandler,
                                                        NULL,
                                                        &instance_got_ip));

    // Configure WiFi connection
    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    
    if (password != NULL && strlen(password) > 0) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    
    wifi_config.sta.threshold.authmode = (password != NULL && strlen(password) > 0) 
                                        ? WIFI_AUTH_WPA2_PSK 
                                        : WIFI_AUTH_OPEN;

    // Set WiFi mode and configuration
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid);

    // Wait for connection or failure
    TickType_t wait_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                          pdFALSE,
                                          pdFALSE,
                                          wait_ticks);

    // Check connection result
    bool success = false;
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Successfully connected to WiFi");
        g_connection_state.type = CONN_WIFI;
        g_connection_state.is_connected = true;
        success = true;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to WiFi");
        g_connection_state.type = CONN_NONE;
        g_connection_state.is_connected = false;
        success = false;
    } else {
        ESP_LOGE(TAG, "WiFi connection timeout");
        g_connection_state.type = CONN_NONE;
        g_connection_state.is_connected = false;
        success = false;
    }

    // Cleanup (only unregister handlers, keep WiFi running if successful)
    if (!success) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
        esp_wifi_stop();
        esp_wifi_deinit();
    }

    vEventGroupDelete(s_wifi_event_group);
    
    return success;
}

/**
 * Disconnect from WiFi and cleanup
 */
void WiFiDisconnect(void)
{
    ESP_LOGI(TAG, "Disconnecting from WiFi");
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    g_connection_state.type = CONN_NONE;
    g_connection_state.is_connected = false;
}

// ========================================
// HTTPS Functions
// ========================================

/**
 * @brief Writes TLS credentials to the modem's NVS and configures the TLS profile.
 *
 * This function stores the provided TLS certificates and private keys into the modem's
 * non-volatile storage (NVS), and then sets up a TLS profile for secure communication.
 * These configuration changes are persistent across reboots.
 *
 * @note
 * - Certificate indexes 0-10 are reserved for Sequans and BlueCherry internal usage.
 * - Private key index 1 is reserved for BlueCherry internal usage.
 * - Do not attempt to override or use these reserved indexes.
 *
 * @return
 * - true if the credentials were successfully written and the profile configured.
 * - false otherwise.
 */
bool SetupTLSProfile(int https_tls_profile) 
{
    if(!modem.tlsWriteCredential(false, 12, ca_cert)) {
        ESP_LOGE(TAG, "CA cert upload failed");
        return false;
    }

    if(modem.tlsConfigProfile(https_tls_profile, WALTER_MODEM_TLS_VALIDATION_CA,
                            WALTER_MODEM_TLS_VERSION_12, 12)) {
        ESP_LOGI(TAG, "TLS profile configured");
    } else {
        ESP_LOGE(TAG, "TLS profile configuration failed");
        return false;
    }

    return true;
}

/**
 * @brief Perform an HTTPS POST request with a body.
 */
bool HttpsPost(
    const char* path, 
    const uint8_t* body, 
    size_t bodyLen, 
    const char* mimeType, 
    int modem_https_profile, 
    const char* https_host) 
{
    char ctBuf[32] = { 0 };

    ESP_LOGI(TAG, "Sending HTTPS POST to %s%s (%u bytes, type %s)", https_host, path,
                (unsigned) bodyLen, mimeType);
    if(!modem.httpSend(modem_https_profile, path, (uint8_t*) body, (uint16_t) bodyLen,
                    WALTER_MODEM_HTTP_SEND_CMD_POST, WALTER_MODEM_HTTP_POST_PARAM_JSON, ctBuf,
                    sizeof(ctBuf))) {
        ESP_LOGE(TAG, "HTTPS POST failed");
        return false;
    }
    ESP_LOGI(TAG, "HTTPS POST successfully sent");
    return WaitForHttpsResponse(modem_https_profile, ctBuf);
}

// ========================================
// WebSocket Implementation
// ========================================

// WebSocket frame structure
struct WsFrame {
    bool fin;
    uint8_t opcode;
    bool masked;
    uint64_t payload_len;
    uint8_t mask_key[4];
    uint8_t* payload;
};

RealtimeWsSession wsSession = {false, {0}, {0}, 0, nullptr};

/**
 * @brief WiFi WebSocket event handler
 */
void WifiWebSocketEventHandler(void* handler_args, esp_event_base_t base, 
                                int32_t event_id, void* event_data)
{
    esp_websocket_event_data_t* data = (esp_websocket_event_data_t*)event_data;
    
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WiFi WebSocket connected");
            wsSession.connected = true;
            break;
            
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WiFi WebSocket disconnected");
            wsSession.connected = false;
            break;
            
        case WEBSOCKET_EVENT_DATA:
            ESP_LOGI(TAG, "WiFi WebSocket data received: %d bytes", data->data_len);
            // Store received data in session buffer
            if (data->data_len <= sizeof(wsSession.recv_buffer)) {
                memcpy(wsSession.recv_buffer, data->data_ptr, data->data_len);
                wsSession.recv_buffer_len = data->data_len;
            } else {
                ESP_LOGW(TAG, "Received data too large for buffer");
            }
            break;
            
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WiFi WebSocket error");
            wsSession.connected = false;
            break;
            
        default:
            break;
    }
}

/**
 * @brief Generate WebSocket accept key from client key
 */
bool GenerateAcceptKey(const char* client_key, char* accept_key, size_t accept_key_size)
{
    char concat[128];
    snprintf(concat, sizeof(concat), "%s%s", client_key, WS_MAGIC_STRING);

    uint8_t hash[20];
    mbedtls_sha1((uint8_t*)concat, strlen(concat), hash);

    size_t olen;
    if(mbedtls_base64_encode((uint8_t*)accept_key, accept_key_size, &olen, 
                            hash, sizeof(hash)) != 0) {
        return false;
    }
    accept_key[olen] = '\0';
    return true;
}

/**
 * @brief Generate random WebSocket key for handshake
 */
void GenerateWebSocketKey(char* key, size_t key_size)
{
    uint8_t random_bytes[16];
    for(int i = 0; i < 16; i++) {
        random_bytes[i] = esp_random() & 0xFF;
    }

    size_t olen;
    mbedtls_base64_encode((uint8_t*)key, key_size, &olen, random_bytes, 16);
    key[olen] = '\0';
}

/**
 * @brief Mask WebSocket payload data
 */
static void MaskPayload(uint8_t* payload, size_t len, const uint8_t* mask_key)
{
    for(size_t i = 0; i < len; i++) {
        payload[i] ^= mask_key[i % 4];
    }
}

/**
 * @brief Create a WebSocket frame header
 */
static size_t CreateWsFrameHeader(uint8_t* buffer, uint8_t opcode, size_t payload_len, bool mask)
{
    size_t header_len = 2;

    // Byte 0: FIN bit + opcode
    buffer[0] = 0x80 | (opcode & 0x0F);

    // Byte 1: MASK bit + payload length
    if(payload_len < 126) {
        buffer[1] = (mask ? 0x80 : 0x00) | (uint8_t)payload_len;
    } else if(payload_len < 65536) {
        buffer[1] = (mask ? 0x80 : 0x00) | 126;
        buffer[2] = (payload_len >> 8) & 0xFF;
        buffer[3] = payload_len & 0xFF;
        header_len = 4;
    } else {
        buffer[1] = (mask ? 0x80 : 0x00) | 127;
        for(int i = 0; i < 8; i++) {
            buffer[2 + i] = (payload_len >> (56 - i * 8)) & 0xFF;
        }
        header_len = 10;
    }

    // Add masking key if needed
    if(mask) {
        for(int i = 0; i < 4; i++) {
            buffer[header_len + i] = esp_random() & 0xFF;
        }
        header_len += 4;
    }

    return header_len;
}

/**
 * @brief Parse incoming WebSocket frame
 */
static bool ParseWsFrame(const uint8_t* data, size_t data_len, WsFrame* frame)
{
    if(data_len < 2) return false;

    frame->fin = (data[0] & 0x80) != 0;
    frame->opcode = data[0] & 0x0F;
    frame->masked = (data[1] & 0x80) != 0;

    size_t offset = 2;
    uint8_t len_code = data[1] & 0x7F;

    if(len_code < 126) {
        frame->payload_len = len_code;
    } else if(len_code == 126) {
        if(data_len < 4) return false;
        frame->payload_len = (data[2] << 8) | data[3];
        offset = 4;
    } else {
        if(data_len < 10) return false;
        frame->payload_len = 0;
        for(int i = 0; i < 8; i++) {
            frame->payload_len = (frame->payload_len << 8) | data[2 + i];
        }
        offset = 10;
    }

    if(frame->masked) {
        if(data_len < offset + 4) return false;
        memcpy(frame->mask_key, data + offset, 4);
        offset += 4;
    }

    if(data_len < offset + frame->payload_len) return false;

    frame->payload = (uint8_t*)(data + offset);
    return true;
}

/**
 * @brief Setup TLS for WebSocket connection
 */
bool SetupWebSocketTLS(int tls_profile)
{
    // Configure TLS profile for api.openai.com
    // Note: You need to upload the appropriate CA certificate first
    if(modem.tlsConfigProfile(tls_profile, WALTER_MODEM_TLS_VALIDATION_CA,
                            WALTER_MODEM_TLS_VERSION_12, 12)) {
        ESP_LOGI(TAG, "WebSocket TLS profile configured");
        return true;
    } else {
        ESP_LOGE(TAG, "WebSocket TLS profile configuration failed");
        return false;
    }
}

/**
 * @brief Send a WebSocket message
 * 
 * @param payload Message payload (JSON string)
 * @param payload_len Length of payload
 * @param opcode WebSocket opcode (default: TEXT)
 * @return true on success, false on error
 */
bool WsSend(const uint8_t* payload, size_t payload_len, uint8_t opcode)
{
    if(!wsSession.connected) {
        ESP_LOGE(TAG, "WebSocket not connected");
        return false;
    }

    if (!IsConnected()) {
        ESP_LOGE(TAG, "No network connection available");
        return false;
    }

    if (GetConnectionType() == CONN_WIFI) {
        if (wsSession.wifi_ws_handle == nullptr) {
            ESP_LOGE(TAG, "WiFi WebSocket handle not initialized");
            return false;
        }
        
        // All OpenAI Realtime API messages are JSON text (including base64 audio)
        if (esp_websocket_client_send_text(wsSession.wifi_ws_handle, 
                                        (const char*)payload, 
                                        payload_len, 
                                        portMAX_DELAY) < 0) {
            ESP_LOGE(TAG, "Failed to send WiFi WebSocket message");
            return false;
        }
        
        return true;
        
    } else if (GetConnectionType() == CONN_CELLULAR) {
        // Use Walter modem socket for cellular (existing implementation)
        // Create frame header
        uint8_t header[14];
        size_t header_len = CreateWsFrameHeader(header, opcode, payload_len, true);

        // Get mask key from header
        uint8_t mask_key[4];
        memcpy(mask_key, header + header_len - 4, 4);

        // Create masked payload
        uint8_t* masked_payload = (uint8_t*)malloc(payload_len);
        if(!masked_payload) {
            ESP_LOGE(TAG, "Failed to allocate payload buffer");
            return false;
        }
        memcpy(masked_payload, payload, payload_len);
        MaskPayload(masked_payload, payload_len, mask_key);

        WalterModemRsp rsp = {};
        bool success = true;
        
        if(!modem.socketSend(header, header_len, &rsp, NULL, NULL, 
                            WALTER_MODEM_RAI_NO_INFO, WS_SOCKET_ID)) {
            ESP_LOGE(TAG, "Failed to send frame header");
            success = false;
        } else if(!modem.socketSend(masked_payload, payload_len, &rsp, NULL, NULL, 
                                    WALTER_MODEM_RAI_NO_INFO, WS_SOCKET_ID)) {
            ESP_LOGE(TAG, "Failed to send frame payload");
            success = false;
        }

        free(masked_payload);
        return success;
    }

    ESP_LOGE(TAG, "Unknown connection type");
    return false;
}

/**
 * @brief Receive WebSocket message
 * 
 * @param buffer Buffer to store received message
 * @param buffer_size Size of buffer
 * @param received_len Pointer to store actual received length
 * @return true if message received, false otherwise
 */
bool WsReceive(uint8_t* buffer, size_t buffer_size, size_t* received_len)
{
    if(!wsSession.connected) {
        return false;
    }

    if (!IsConnected()) {
        ESP_LOGE(TAG, "No network connection available");
        return false;
    }

    if (GetConnectionType() == CONN_WIFI) {
        // For WiFi, data is received via event handler
        if (wsSession.recv_buffer_len == 0) {
            return false;  // No data available
        }
        
        if (wsSession.recv_buffer_len > buffer_size) {
            ESP_LOGE(TAG, "Received data larger than buffer");
            return false;
        }
        
        memcpy(buffer, wsSession.recv_buffer, wsSession.recv_buffer_len);
        *received_len = wsSession.recv_buffer_len;
        buffer[*received_len] = '\0';
        
        // Clear the buffer after reading
        wsSession.recv_buffer_len = 0;
        
        return true;
        
    } else if (GetConnectionType() == CONN_CELLULAR) {
        // Use Walter modem socket for cellular (existing implementation)
        uint8_t frame_data[2048];
        WalterModemRsp rsp = {};
        
        uint16_t available = modem.socketAvailable(WS_SOCKET_ID);
        if(available == 0) return false;

        if(!modem.socketReceive(available, sizeof(frame_data), frame_data, WS_SOCKET_ID, &rsp)) {
            ESP_LOGE(TAG, "Failed to receive data");
            return false;
        }

        WsFrame frame;
        if(!ParseWsFrame(frame_data, available, &frame)) {
            ESP_LOGE(TAG, "Failed to parse WebSocket frame");
            return false;
        }

        // Handle different opcodes
        switch(frame.opcode) {
            case WS_OP_TEXT:
            case WS_OP_BINARY:
                if(frame.payload_len > buffer_size) {
                    ESP_LOGE(TAG, "Received payload too large");
                    return false;
                }
                memcpy(buffer, frame.payload, frame.payload_len);
                *received_len = frame.payload_len;
                buffer[*received_len] = '\0';
                return true;
                
            case WS_OP_PING:
                WsSend(frame.payload, frame.payload_len, WS_OP_PONG);
                return false;
                
            case WS_OP_CLOSE:
                ESP_LOGI(TAG, "WebSocket close frame received");
                wsSession.connected = false;
                return false;
                
            default:
                return false;
        }
    }

    return false;
}
}