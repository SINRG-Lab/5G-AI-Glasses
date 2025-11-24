// Standard C components
#include <string.h>
#include <vector>
#include <optional>

#include "esp_system.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_spiffs.h"
#include "mbedtls/base64.h"

#include "audio_pipeline.h"
#include "audio_element.h"
#include "mp3_decoder.h"
#include "wav_decoder.h"
#include "cJSON.h"

#include "walter_com.h"
#include "walter_spiffs.h"

static const char* TAG = "AUDIO AGENT";

namespace audio_agent {

namespace {
static bool g_cert_bundle_initialized = false;

/**
 * @brief Ensures that the certification bundle is initialzied 
 */
bool EnsureCertBundleInitialized() 
{
    if (!g_cert_bundle_initialized) {
        esp_crt_bundle_attach(NULL);
        ESP_LOGI(TAG, "Certificate bundle attached");
        g_cert_bundle_initialized = true;
    }
    return true;
}

// Forward declarations
bool SendAudioFile(const char* file_path);
bool SendAudioStream(const uint8_t* audio_data, size_t audio_len);
std::optional<std::vector<uint8_t>> ReceiveAudioResponse(bool print_response);
void ParseAndPrintResponse(cJSON* json, const char* type_str, int* audio_chunk_count);
bool RealtimeSendAudio(const uint8_t* audio_data, size_t audio_len);
bool RealtimeCommitAudio();
bool RealtimeSendText(const char* text);
bool RealtimeGenerateResponse();
bool RealtimeConnect(const char* api_key, const char* model);
void RealtimeDisconnect();
}

/**
 * @brief Send and receive audio from OpenAI's realtime API. 
 * 
 * Audio can be included either as raw audio data or a file path to read audio from.
 * The format of the file can be a .wav or .mp3.
 * 
 * @param openai_api_key OpenAI API key
 * @param openai_model What model from OpenAI to use
 * @param audio_data_stream A stream of audio data
 * @param file_path Path to audio file
 * @param print_response Boolean of whether to print out detailed information about 
 *                       the JSON audio response from the API.  
 * @return Optional containing raw audio data from OpenAI if successful, std::nullopt on failure
 */
std::optional<std::vector<uint8_t>> SendAndRecieveAudio(
    const char* openai_api_key, 
    const char* openai_model, 
    const uint8_t* audio_data_stream, 
    size_t audio_data_len,
    const char* file_path,
    bool print_response
) 
{   
    EnsureCertBundleInitialized();

    ESP_LOGI(TAG, "Connecting to OpenAI Realtime API...");
    if (!RealtimeConnect(openai_api_key, openai_model)) {
        ESP_LOGE(TAG, "Failed to connect to OpenAI Realtime API");
        return std::nullopt;
    }
    ESP_LOGI(TAG, "Successfully connected to OpenAI Realtime API");

    // Send audio
    bool send_success = false;
    if (file_path != nullptr) {
        send_success = SendAudioFile(file_path);
    } else if (audio_data_stream != nullptr && audio_data_len > 0) {
        send_success = SendAudioStream(audio_data_stream, audio_data_len);
    } else {
        ESP_LOGE(TAG, "No audio data provided");
        RealtimeDisconnect();
        return std::nullopt;
    }

    if (!send_success) {
        ESP_LOGE(TAG, "Failed to send audio");
        RealtimeDisconnect();
        return std::nullopt;
    }

    // Receive response
    std::optional<std::vector<uint8_t>> audio_response = ReceiveAudioResponse(print_response);

    // Disconnect from OpenAI
    ESP_LOGI(TAG, "Disconnecting from OpenAI Realtime API...");
    RealtimeDisconnect();
    ESP_LOGI(TAG, "Disconnected from OpenAI");

    return audio_response;
}



namespace {
/**
 * @brief Read and decode audio file using ESP-ADF pipeline with SPIFFS
 * 
 * @param file_path Path to audio file in SPIFFS (e.g., "/spiffs/audio.mp3")
 * @return true on success, false on error
 */
bool SendAudioFile(const char* file_path)
{
    ESP_LOGI(TAG, "Creating audio pipeline for file: %s", file_path);

    // Create audio pipeline
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    audio_pipeline_handle_t pipeline = audio_pipeline_init(&pipeline_cfg);

    if (!pipeline) {
        ESP_LOGE(TAG, "Failed to create pipeline");
        return false;
    }

    // Create SPIFFS stream reader
    audio_element_handle_t spiffs_reader = spiffs::StreamInit();
    if (!spiffs_reader) {
        ESP_LOGE(TAG, "Failed to create SPIFFS reader");
        audio_pipeline_deinit(pipeline);
        return false;
    }

    // Determine file type and create decoder
    audio_element_handle_t decoder = NULL;
    const char* file_ext = strrchr(file_path, '.');

    if (file_ext && strcmp(file_ext, ".mp3") == 0) {
        mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
        decoder = mp3_decoder_init(&mp3_cfg);
        ESP_LOGI(TAG, "Using MP3 decoder");
    } 

    else if (file_ext && strcmp(file_ext, ".wav") == 0) {
        wav_decoder_cfg_t wav_cfg = DEFAULT_WAV_DECODER_CONFIG();
        decoder = wav_decoder_init(&wav_cfg);
        ESP_LOGI(TAG, "Using WAV decoder");
    }

    else {
        ESP_LOGE(TAG, "Unsupported file format: %s", file_ext ? file_ext : "unknown");
        audio_element_deinit(spiffs_reader);
        audio_pipeline_deinit(pipeline);
        return false;
    }

    if (!decoder) {
        ESP_LOGE(TAG, "Failed to create decoder");
        audio_element_deinit(spiffs_reader);
        audio_pipeline_deinit(pipeline);
        return false;
    }

    // Register elements to pipeline
    audio_pipeline_register(pipeline, spiffs_reader, "spiffs");
    audio_pipeline_register(pipeline, decoder, "decoder");

    // Link elements: spiffs -> decoder
    const char *link_tag[2] = {"spiffs", "decoder"};
    audio_pipeline_link(pipeline, &link_tag[0], 2);

    // Set the file URI
    audio_element_set_uri(spiffs_reader, file_path);

    // Start the pipeline
    audio_pipeline_run(pipeline);

    // Read decoded PCM data and send to OpenAI
    // 0.1s of 24kHz mono PCM16
    const size_t chunk_size = 4800;
    uint8_t* buffer = (uint8_t*)malloc(chunk_size);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        audio_pipeline_stop(pipeline);
        audio_pipeline_wait_for_stop(pipeline);
        audio_pipeline_terminate(pipeline);
        audio_pipeline_unregister(pipeline, spiffs_reader);
        audio_pipeline_unregister(pipeline, decoder);
        audio_element_deinit(spiffs_reader);
        audio_element_deinit(decoder);
        audio_pipeline_deinit(pipeline);
        return false;
    }

    bool success = true;
    size_t total_sent = 0;
    int bytes_read;

    ESP_LOGI(TAG, "Reading and sending decoded audio...");

    // Read decoded audio from the decoder element
    while ((bytes_read = audio_element_output(decoder, (char*)buffer, chunk_size)) > 0) {
        // Send chunk to OpenAI
        if (!RealtimeSendAudio(buffer, bytes_read)) {
            ESP_LOGE(TAG, "Failed to send audio chunk");
            success = false;
            break;
        }

        total_sent += bytes_read;
        if (total_sent % 48000 == 0) { // Log every second
            ESP_LOGI(TAG, "Sent %u bytes", total_sent);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Commit audio buffer
    if (success) {
        ESP_LOGI(TAG, "Audio complete (%u bytes). Committing...", total_sent);
        if (!RealtimeCommitAudio()) {
            ESP_LOGE(TAG, "Failed to commit audio");
            success = false;
        } else {
            ESP_LOGI(TAG, "Audio committed successfully");
        }
    }

    // Cleanup
    free(buffer);
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);
    audio_pipeline_unregister(pipeline, spiffs_reader);
    audio_pipeline_unregister(pipeline, decoder);
    audio_element_deinit(spiffs_reader);
    audio_element_deinit(decoder);
    audio_pipeline_deinit(pipeline);
    ESP_LOGI(TAG, "Pipeline cleaned up");
    return success;
}


/**
 * @brief Send raw PCM audio stream to OpenAI
 * 
 * @param audio_data PCM16 audio data (mono, 24kHz)
 * @param audio_len Length of audio data
 * @return true on success, false on error
 */
bool SendAudioStream(const uint8_t* audio_data, size_t audio_len)
{
    ESP_LOGI(TAG, "Sending raw audio stream (%u bytes)...", audio_len);
    
    // Send audio in chunks (0.1s of 24kHz mono PCM16)
    const size_t chunk_size = 4800;
    size_t total_sent = 0;
    
    for (size_t offset = 0; offset < audio_len; offset += chunk_size) {
        size_t bytes_to_send = (offset + chunk_size > audio_len) ? 
                            (audio_len - offset) : chunk_size;
        
        if (!RealtimeSendAudio(audio_data + offset, bytes_to_send)) {
            ESP_LOGE(TAG, "Failed to send audio chunk at offset %u", offset);
            return false;
        }
        
        total_sent += bytes_to_send;
        if (total_sent % 48000 == 0) { // Log every second
            ESP_LOGI(TAG, "Sent %u bytes", total_sent);
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    // Commit audio buffer
    ESP_LOGI(TAG, "Audio stream complete (%u bytes). Committing...", total_sent);
    if (!RealtimeCommitAudio()) {
        ESP_LOGE(TAG, "Failed to commit audio");
        return false;
    }
    
    ESP_LOGI(TAG, "Audio stream committed successfully");
    return true;
}

/**
 * @brief Receive audio response from OpenAI
 * 
 * @param print_response Whether to print detailed JSON response information
 * @return Optional containing decoded PCM audio data if successful, std::nullopt on failure
 */
std::optional<std::vector<uint8_t>> ReceiveAudioResponse(bool print_response)
{
    std::vector<uint8_t> audio_chunks;
    uint8_t response_buffer[8192];
    size_t response_len;
    bool response_complete = false;
    int audio_chunk_count = 0;
    
    ESP_LOGI(TAG, "Waiting for audio response from OpenAI...");
    
    // Poll for up to 60 seconds
    const int max_polling_time = 60; // seconds
    for (int i = 0; i < max_polling_time && !response_complete; i++) {
        if (com::WsReceive(response_buffer, sizeof(response_buffer) - 1, &response_len)) {
            response_buffer[response_len] = '\0';
            
            // Parse JSON response
            cJSON *json = cJSON_Parse((const char*)response_buffer);
            if (json == NULL) {
                ESP_LOGW(TAG, "Failed to parse JSON response");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            
            // Get message type
            cJSON *type = cJSON_GetObjectItem(json, "type");
            if (type == NULL || !cJSON_IsString(type)) {
                cJSON_Delete(json);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            
            const char* type_str = type->valuestring;
            
            // Print response if requested
            if (print_response) {
                ParseAndPrintResponse(json, type_str, &audio_chunk_count);
            }
            
            // Handle audio delta (actual audio data)
            if (strcmp(type_str, "response.audio.delta") == 0) {
                cJSON *delta = cJSON_GetObjectItem(json, "delta");
                if (delta && cJSON_IsString(delta)) {
                    // Decode base64 audio
                    const char* base64_audio = delta->valuestring;
                    size_t base64_len = strlen(base64_audio);
                    size_t decoded_len = (base64_len * 3) / 4;
                    
                    std::vector<uint8_t> decoded_chunk(decoded_len);
                    size_t actual_len;
                    
                    if (mbedtls_base64_decode(decoded_chunk.data(), decoded_len, 
                                            &actual_len, (const uint8_t*)base64_audio, 
                                            base64_len) == 0) {
                        decoded_chunk.resize(actual_len);
                        audio_chunks.insert(audio_chunks.end(), 
                                        decoded_chunk.begin(), 
                                        decoded_chunk.end());
                        audio_chunk_count++;
                    }
                }
            }
            
            // Check if response is complete
            if (strcmp(type_str, "response.done") == 0) {
                ESP_LOGI(TAG, "Response complete. Received %d audio chunks (%u bytes total)",
                        audio_chunk_count, audio_chunks.size());
                response_complete = true;
            }
            
            cJSON_Delete(json);
        }
        
        // Pause briefly between polls
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    if (!response_complete) {
        ESP_LOGE(TAG, "Response timeout - no complete response received");
        return std::nullopt;
    }
    
    if (audio_chunks.empty()) {
        ESP_LOGW(TAG, "No audio data received in response");
        return std::nullopt;
    }
    
    return audio_chunks;
}

/**
 * @brief Parse and print detailed JSON response information
 * 
 * @param json Parsed JSON object
 * @param type_str Message type string
 * @param audio_chunk_count Pointer to audio chunk counter
 */
void ParseAndPrintResponse(cJSON* json, const char* type_str, int* audio_chunk_count)
{
    if (strcmp(type_str, "session.created") == 0) {
        ESP_LOGI(TAG, "✓ Session created successfully");
        
        cJSON *session = cJSON_GetObjectItem(json, "session");
        if (session) {
            cJSON *id = cJSON_GetObjectItem(session, "id");
            if (id && cJSON_IsString(id)) {
                ESP_LOGI(TAG, "  Session ID: %s", id->valuestring);
            }
            
            cJSON *model = cJSON_GetObjectItem(session, "model");
            if (model && cJSON_IsString(model)) {
                ESP_LOGI(TAG, "  Model: %s", model->valuestring);
            }
        }
    }
    else if (strcmp(type_str, "input_audio_buffer.committed") == 0) {
        ESP_LOGI(TAG, "✓ Audio buffer committed");
    }
    else if (strcmp(type_str, "response.audio.delta") == 0) {
        if (*audio_chunk_count == 0) {
            ESP_LOGI(TAG, "✓ Receiving audio response...");
        }
        (*audio_chunk_count)++;
        
        cJSON *delta = cJSON_GetObjectItem(json, "delta");
        if (delta && cJSON_IsString(delta)) {
            size_t base64_len = strlen(delta->valuestring);
            size_t decoded_estimate = (base64_len * 3) / 4;
            ESP_LOGI(TAG, "  Audio chunk #%d: %u bytes base64 (~%u bytes PCM)",
                    *audio_chunk_count, base64_len, decoded_estimate);
        }
    }
    else if (strcmp(type_str, "response.audio.done") == 0) {
        ESP_LOGI(TAG, "✓ Audio response complete");
        ESP_LOGI(TAG, "  Total chunks: %d", *audio_chunk_count);
    }
    else if (strcmp(type_str, "response.done") == 0) {
        ESP_LOGI(TAG, "✓ Response generation complete");
        
        cJSON *response = cJSON_GetObjectItem(json, "response");
        if (response) {
            cJSON *usage = cJSON_GetObjectItem(response, "usage");
            if (usage) {
                ESP_LOGI(TAG, "Usage statistics:");
                
                cJSON *total_tokens = cJSON_GetObjectItem(usage, "total_tokens");
                if (total_tokens && cJSON_IsNumber(total_tokens)) {
                    ESP_LOGI(TAG, "  Total tokens: %d", total_tokens->valueint);
                }
            }
        }
    }
}


// ========================================
// OpenAI Realtime API Functions
// ========================================

/**
 * @brief Send audio to OpenAI Realtime API
 * 
 * @param audio_data PCM16 audio data (mono, 24kHz)
 * @param audio_len Length of audio data
 * @return true on success, false on error
 */
bool RealtimeSendAudio(const uint8_t* audio_data, size_t audio_len)
{
    // Base64 encode audio
    size_t base64_len;
    mbedtls_base64_encode(NULL, 0, &base64_len, audio_data, audio_len);

    uint8_t* base64_audio = (uint8_t*)malloc(base64_len + 1);
    if(!base64_audio) {
        ESP_LOGE(TAG, "Failed to allocate base64 buffer");
        return false;
    }

    mbedtls_base64_encode(base64_audio, base64_len + 1, &base64_len, audio_data, audio_len);
    base64_audio[base64_len] = '\0';

    // Create JSON message
    size_t json_size = base64_len + 128;
    char* json = (char*)malloc(json_size);
    if(!json) {
        free(base64_audio);
        return false;
    }

    snprintf(json, json_size,
                "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}",
                base64_audio);

    bool result = com::WsSend((uint8_t*)json, strlen(json), com::WS_OP_TEXT);

    free(base64_audio);
    free(json);

    return result;
}

/**
 * @brief Commit audio and trigger response
 */
bool RealtimeCommitAudio()
{
    const char* msg = "{\"type\":\"input_audio_buffer.commit\"}";
    return com::WsSend((uint8_t*)msg, strlen(msg), com::WS_OP_TEXT);
}

/**
 * @brief Send text message
 */
bool RealtimeSendText(const char* text)
{
    char json[1024];
    snprintf(json, sizeof(json),
                "{\"type\":\"conversation.item.create\",\"item\":{"
                "\"type\":\"message\",\"role\":\"user\",\"content\":["
                "{\"type\":\"input_text\",\"text\":\"%s\"}]}}",
                text);
    return com::WsSend((uint8_t*)json, strlen(json), com::WS_OP_TEXT);
}

/**
 * @brief Request response generation
 */
bool RealtimeGenerateResponse()
{
    const char* msg = "{\"type\":\"response.create\"}";
    return com::WsSend((uint8_t*)msg, strlen(msg), com::WS_OP_TEXT);
}

/**
 * @brief Connect to OpenAI Realtime API via WebSocket
 * 
 * @param api_key Your OpenAI API key
 * @param model Model name (e.g., "gpt-4o-realtime-preview-2024-12-17")
 * @return true on success, false on error
 */
bool RealtimeConnect(const char* api_key, const char* model)
{
    // Check if we have any network connectivity
    if(!com::IsConnected()) {
        ESP_LOGE(TAG, "Not connected to any network");
        return false;
    }

    com::ConnectionType conn_type = com::GetConnectionType();
    ESP_LOGI(TAG, "Connecting via %s", 
             conn_type == com::CONN_WIFI ? "WiFi" : "Cellular");

    if (conn_type == com::CONN_WIFI) {
        // WiFi connection - use ESP-IDF WebSocket client
        ESP_LOGI(TAG, "Initializing WiFi WebSocket connection...");
        
        // Build WebSocket URI
        char ws_uri[512];
        snprintf(ws_uri, sizeof(ws_uri),
                "wss://api.openai.com/v1/realtime?model=%s", model);
        
        // Build authorization header
        char auth_header[512];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
        
        // Configure WebSocket client with certificate bundle
        esp_websocket_client_config_t ws_cfg = {};
        ws_cfg.uri = ws_uri;
        ws_cfg.task_stack = 8192;
        ws_cfg.buffer_size = 8192;
        ws_cfg.headers = auth_header;
        ws_cfg.subprotocol = nullptr;
        ws_cfg.user_agent = "ESP32-Walter";
        ws_cfg.disable_auto_reconnect = true;
        
        // CRITICAL: Enable certificate bundle for TLS verification
        ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
        ws_cfg.skip_cert_common_name_check = false;
        
        // Create WebSocket client
        com::wsSession.wifi_ws_handle = esp_websocket_client_init(&ws_cfg);
        if (com::wsSession.wifi_ws_handle == nullptr) {
            ESP_LOGE(TAG, "Failed to initialize WiFi WebSocket client");
            return false;
        }
        
        // Register event handler
        esp_websocket_register_events(com::wsSession.wifi_ws_handle, 
                                    WEBSOCKET_EVENT_ANY,
                                    com::WifiWebSocketEventHandler, 
                                    nullptr);
        
        // Connect
        if (esp_websocket_client_start(com::wsSession.wifi_ws_handle) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start WiFi WebSocket client");
            esp_websocket_client_destroy(com::wsSession.wifi_ws_handle);
            com::wsSession.wifi_ws_handle = nullptr;
            return false;
        }
        
        // Wait for connection
        int retry_count = 0;
        const int max_retries = 30; // 3 seconds
        while (!com::wsSession.connected && retry_count < max_retries) {
            vTaskDelay(pdMS_TO_TICKS(100));
            retry_count++;
        }
        
        if (!com::wsSession.connected) {
            ESP_LOGE(TAG, "WiFi WebSocket connection timeout");
            esp_websocket_client_destroy(com::wsSession.wifi_ws_handle);
            com::wsSession.wifi_ws_handle = nullptr;
            return false;
        }
        
        ESP_LOGI(TAG, "WiFi WebSocket connected to OpenAI Realtime API");
        return true;
        
    } else if (conn_type == com::CONN_CELLULAR) {
        // Cellular connection - use Walter modem sockets (existing implementation)
        WalterModemRsp rsp = {};
        
        // Get and log PDP address to verify data connection
        if(com::modem.getPDPAddress(&rsp, NULL, NULL, 1)) {
            ESP_LOGI(TAG, "PDP context active with IP: %s", rsp.data.pdpAddressList.pdpAddress);
        } else {
            ESP_LOGE(TAG, "No PDP address - data connection not active");
            return false;
        }

        // Configure socket
        ESP_LOGI(TAG, "Configuring socket...");
        if(!com::modem.socketConfig(&rsp, NULL, NULL, 1, 1500, 90, 60, 5000)) {
            ESP_LOGE(TAG, "Failed to configure WebSocket");
            if(rsp.type == WALTER_MODEM_RSP_DATA_TYPE_CME_ERROR) {
                ESP_LOGE(TAG, "CME Error: %d", rsp.data.cmeError);
            }
            ESP_LOGE(TAG, "Modem state: %d", rsp.result);
            return false;
        }

        ESP_LOGI(TAG, "Socket configured successfully, socket ID: %d", rsp.data.socketId);

        // Enable TLS on socket
        if(!com::modem.socketConfigSecure(true, com::WS_TLS_PROFILE, com::WS_SOCKET_ID)) {
            ESP_LOGE(TAG, "Failed to enable TLS on WebSocket");
            return false;
        }

        // Connect to OpenAI
        if(!com::modem.socketDial("api.openai.com", 443, 0, &rsp, NULL, NULL, 
                            WALTER_MODEM_SOCKET_PROTO_TCP, 
                            WALTER_MODEM_ACCEPT_ANY_REMOTE_DISABLED, 
                            com::WS_SOCKET_ID)) {
            ESP_LOGE(TAG, "Failed to connect to OpenAI");
            return false;
        }

        ESP_LOGI(TAG, "TCP connection established");

        // Generate WebSocket key
        char ws_key[32];
        com::GenerateWebSocketKey(ws_key, sizeof(ws_key));

        // Build WebSocket handshake request
        char handshake[1024];
        int handshake_len = snprintf(handshake, sizeof(handshake),
            "GET /v1/realtime?model=%s HTTP/1.1\r\n"
            "Host: api.openai.com\r\n"
            "Authorization: Bearer %s\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "OpenAI-Beta: realtime=v1\r\n"
            "\r\n",
            model, api_key, ws_key);

        // Send handshake
        if(!com::modem.socketSend((uint8_t*)handshake, handshake_len, &rsp, NULL, NULL, 
                            WALTER_MODEM_RAI_NO_INFO, com::WS_SOCKET_ID)) {
            ESP_LOGE(TAG, "Failed to send WebSocket handshake");
            com::modem.socketClose(&rsp, NULL, NULL, com::WS_SOCKET_ID);
            return false;
        }

        ESP_LOGI(TAG, "WebSocket handshake sent");

        // Wait for handshake response
        vTaskDelay(pdMS_TO_TICKS(2000));

        uint8_t response[512];
        uint16_t available = com::modem.socketAvailable(com::WS_SOCKET_ID);
        if(available > 0) {
            if(com::modem.socketReceive(available, sizeof(response), response, com::WS_SOCKET_ID, &rsp)) {
                // Check for "101 Switching Protocols"
                if(strstr((char*)response, "101") != NULL) {
                    ESP_LOGI(TAG, "WebSocket connected to OpenAI Realtime API");
                    com::wsSession.connected = true;
                    return true;
                }
            }
        }

        ESP_LOGE(TAG, "WebSocket handshake failed");
        com::modem.socketClose(&rsp, NULL, NULL, com::WS_SOCKET_ID);
        return false;
    }

    ESP_LOGE(TAG, "Unknown connection type");
    return false;
}

/**
 * @brief Disconnect from Realtime API
 */
void RealtimeDisconnect()
{
    if(com::wsSession.connected) {
        com::ConnectionType conn_type = com::GetConnectionType();
        
        if (conn_type == com::CONN_WIFI) {
            // Close WiFi WebSocket
            if (com::wsSession.wifi_ws_handle != nullptr) {
                esp_websocket_client_close(com::wsSession.wifi_ws_handle, portMAX_DELAY);
                esp_websocket_client_destroy(com::wsSession.wifi_ws_handle);
                com::wsSession.wifi_ws_handle = nullptr;
            }
        } else if (conn_type == com::CONN_CELLULAR) {
            // Close cellular socket
            com::WsSend(NULL, 0, com::WS_OP_CLOSE);
            WalterModemRsp rsp = {};
            com::modem.socketClose(&rsp, NULL, NULL, com::WS_SOCKET_ID);
        }
        
        com::wsSession.connected = false;
        ESP_LOGI(TAG, "WebSocket disconnected");
    }
}
}
}