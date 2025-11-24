/**
 * === Communications library for the Walter Board ===
 * @file walter_com.h
 * @author Ryder Paulson <paulson.r@northeastern.edu>
 * @brief Header file for communications tools for the Walter Board
 */

#ifndef WALTER_COMM_H
#define WALTER_COMM_H

#include "WalterModem.h"
#include "esp_websocket_client.h"
#include <stdint.h>
#include <stddef.h>

namespace com {

// Global modem instance
extern WalterModem modem;

/**
 * @brief Connection type enumeration
 */
enum ConnectionType {
    CONN_NONE = 0,
    CONN_WIFI = 1,
    CONN_CELLULAR = 2
};

/**
 * @brief Global connection state
 */
struct ConnectionState {
    ConnectionType type;
    bool is_connected;
};

extern ConnectionState g_connection_state;

/**
 * @brief Get current connection type
 * @return Current connection type (CONN_NONE, CONN_WIFI, or CONN_CELLULAR)
 */
ConnectionType GetConnectionType();

/**
 * @brief Check if connected to any network
 * @return true if connected to WiFi or Cellular, false otherwise
 */
bool IsConnected();

// ========================================
// LTE Network Functions
// ========================================

/**
 * @brief Check if connected to the LTE network
 * @return true when connected, false otherwise
 */
bool CheckLTEConnected();

/**
 * @brief Wait for the modem to connect to the LTE network
 * @param timeout_sec Maximum seconds to wait before timeout
 * @return true if connected, false on timeout
 */
bool WaitForNetwork(int timeout_sec);

/**
 * @brief Connect the modem to the cellular network
 * @return true on success, false on error
 */
bool LTEConnect();

/**
 * @brief Disconnect from the LTE network
 * @return true on success, false on error
 */
bool LTEDisconnect();

// ========================================
// WiFi Functions
// ========================================

/**
 * @brief Connect to WiFi network
 * 
 * @param ssid WiFi network name (SSID)
 * @param password WiFi password (pass NULL or empty string for open networks)
 * @param timeout_ms Maximum time to wait for connection in milliseconds (0 = wait forever)
 * 
 * @return true if connection successful, false otherwise
 */
bool WiFiConnect(const char *ssid, const char *password, uint32_t timeout_ms);

/**
 * @brief Disconnect from WiFi and cleanup
 */
void WiFiDisconnect(void);

// ========================================
// HTTPS Functions
// ========================================

/**
 * @brief Setup TLS profile for HTTPS communication
 * @param https_tls_profile TLS profile number to configure
 * @return true on success, false on error
 */
bool SetupTLSProfile(int https_tls_profile);

/**
 * @brief Perform an HTTPS POST request
 * @param path URL path for the request
 * @param body Request body data
 * @param bodyLen Length of body data
 * @param mimeType MIME type of the body (e.g., "application/json")
 * @param modem_https_profile HTTPS profile ID to use
 * @param https_host Host name for the request
 * @return true on success, false on error
 */
bool HttpsPost(
    const char* path,
    const uint8_t* body,
    size_t bodyLen,
    const char* mimeType,
    int modem_https_profile,
    const char* https_host);

// ========================================
// WebSocket Functions
// ========================================


// WebSocket constants
constexpr const char* WS_MAGIC_STRING = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static const int WS_SOCKET_ID = 1; // Use socket ID 1 for WebSocket
static const int WS_TLS_PROFILE = 2; // TLS profile for WebSocket

// WebSocket opcodes (add after the namespace declaration)
enum WsOpcode {
    WS_OP_CONTINUATION = 0x0,
    WS_OP_TEXT = 0x1,
    WS_OP_BINARY = 0x2,
    WS_OP_CLOSE = 0x8,
    WS_OP_PING = 0x9,
    WS_OP_PONG = 0xA
};

// OpenAI Realtime session state structure
struct RealtimeWsSession {
    bool connected;
    char session_id[64];
    uint8_t recv_buffer[8192];
    size_t recv_buffer_len;
    esp_websocket_client_handle_t wifi_ws_handle;  // Add this for WiFi
};

extern RealtimeWsSession wsSession;

/**
 * @brief Event handler for WebSockets through WiFi
 */
void WifiWebSocketEventHandler(void* handler_args, esp_event_base_t base, 
                                int32_t event_id, void* event_data);

/**
 * @brief Generate random WebSocket key for handshake
 */
void GenerateWebSocketKey(char* key, size_t key_size);

/**
 * @brief Setup TLS profile for WebSocket connections
 * @param tls_profile TLS profile number to configure
 * @return true on success, false on error
 */
bool SetupWebSocketTLS(int tls_profile);

/**
 * @brief Send a WebSocket message
 * @param payload Message payload
 * @param payload_len Length of payload
 * @param opcode WebSocket opcode (default: TEXT frame)
 * @return true on success, false on error
 */
bool WsSend(const uint8_t* payload, size_t payload_len, uint8_t opcode = 0x1);

/**
 * @brief Receive a WebSocket message
 * @param buffer Buffer to store received message
 * @param buffer_size Size of receive buffer
 * @param received_len Pointer to store actual received length
 * @return true if message received, false otherwise
 */
bool WsReceive(uint8_t* buffer, size_t buffer_size, size_t* received_len);
}

#endif // WALTER_COMM_H