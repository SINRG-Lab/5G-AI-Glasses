#include "modem_setup.h"
#include "config.h"
#include <Arduino.h>

static WalterModem modem;

uint8_t* responseBuffer = nullptr;
volatile size_t responseLen = 0;
volatile bool responseComplete = false;
volatile bool socketDisconnected = false;

static size_t responseCapacity = 0;

// Pending ring state
volatile size_t pendingRingBytes = 0;
volatile unsigned long lastRingMs = 0;
volatile unsigned long firstByteMs = 0;
volatile unsigned long firstRingAfterResetMs = 0;
static volatile uint8_t pendingConnId = 0;

// Temporary buffer for socketReceive (1500 = max TCP/modem chunk)
static uint8_t recvBuf[1500] = {};

// Socket event handler — accumulates received data into PSRAM buffer
static void socketEventHandler(
    WMSocketEventType event,
    const WMSocketEventData* data,
    void* args
    )
{
    if (event == WALTER_MODEM_SOCKET_EVENT_RING) {
        if (responseBuffer && data->data_len > 0) {
            pendingConnId = data->conn_id;
            pendingRingBytes += data->data_len;
            lastRingMs = millis();
            if (firstRingAfterResetMs == 0)
                firstRingAfterResetMs = millis();

            // Batch reads: only pull from the modem when a full 1500-byte chunk has
            // accumulated. This avoids thousands of tiny AT round-trips for large responses.
            // to cut down on the AT command overhead
            // Exception: read immediately when responseLen == 0 (first ring of a new
            // request) so that small responses (<1500 bytes total) are captured before
            // the server closes the connection.
            if (pendingRingBytes >= 1500 || responseLen == 0) {
                size_t toRead = (pendingRingBytes < 1500) ? pendingRingBytes : 1500;
                if (WalterModem::socketReceive(data->conn_id, recvBuf, toRead)) {
                    size_t toWrite = toRead;
                    if (responseLen + toWrite > responseCapacity) {
                        toWrite = responseCapacity - responseLen;
                    }
                    if (toWrite > 0) {
                        if (responseLen == 0) firstByteMs = millis();
                        memcpy(responseBuffer + responseLen, recvBuf, toWrite);
                        responseLen += toWrite;
                    }
                    pendingRingBytes -= toRead;
                    #if VERBOSITY >= 2
                    Serial.printf("  [ring] read %u bytes (total: %u, still pending: %u)\n",
                                  (unsigned)toRead, (unsigned)responseLen, (unsigned)pendingRingBytes);
                    #endif
                }
            } else {
                #if VERBOSITY >= 2
                Serial.printf("  [ring] +%u bytes announced (pending: %u, buffering)\n",
                              data->data_len, (unsigned)pendingRingBytes);
                #endif
            }
        }
    } else if (event == WALTER_MODEM_SOCKET_EVENT_DISCONNECTED) {
        // Flush any pending bytes NOW, before the library tears down the socket.
        // socketReceive must be called here while the modem socket is still valid.
        if (responseBuffer && pendingRingBytes > 0) {
            while (pendingRingBytes > 0) {
                size_t toRead = (pendingRingBytes < 1500) ? pendingRingBytes : 1500;
                if (WalterModem::socketReceive(data->conn_id, recvBuf, toRead)) {
                    size_t toWrite = toRead;
                    if (responseLen + toWrite > responseCapacity) {
                        toWrite = responseCapacity - responseLen;
                    }
                    if (toWrite > 0) {
                        memcpy(responseBuffer + responseLen, recvBuf, toWrite);
                        responseLen += toWrite;
                    }
                    pendingRingBytes -= toRead;
                    #if VERBOSITY >= 2
                    Serial.printf("  [disconnect flush] read %u bytes (total: %u)\n",
                                  (unsigned)toRead, (unsigned)responseLen);
                    #endif
                } else {
                    #if VERBOSITY >= 2
                    Serial.printf("  [disconnect flush] socketReceive failed, %u bytes lost\n",
                                  (unsigned)pendingRingBytes);
                    #endif
                    pendingRingBytes = 0;
                    break;
                }
            }
        }
        socketDisconnected = true;
        #if VERBOSITY >= 2
        Serial.printf("  [socket %u] disconnected\n", data->conn_id);
        #endif
    }
}

static void networkEventHandler(
    WMNetworkEventType event,
    const WMNetworkEventData* data,
    void* args)
{
    #if VERBOSITY >= 1
    if (event == WALTER_MODEM_NETWORK_EVENT_REG_STATE_CHANGE) {
        Serial.printf("Network reg state changed: %d\n", data->cereg.state);
    }
    #endif
}

bool modemInit()
{
    Serial.println("Initializing modem...");

    if (!modem.begin(&Serial2)) {
        Serial.println("modem.begin() failed");
        return false;
    }

    modem.setNetworkEventHandler(networkEventHandler, NULL);
    modem.setSocketEventHandler(socketEventHandler, NULL);

    if (!modem.checkComm()) {
        Serial.println("Modem communication check failed");
        return false;
    }
    Serial.println("Modem OK");

    // Unlock SIM if PIN is set
    const char* simPin = SIM_PIN;
    if (simPin[0] != '\0') {
        if (!modem.unlockSIM(NULL, NULL, NULL, simPin)) {
            Serial.println("SIM unlock failed");
            return false;
        }
        Serial.println("SIM unlocked");
    }

    // Disable RF before configuring RAT/PDP
    if (!modem.setOpState(WALTER_MODEM_OPSTATE_NO_RF)) {
        Serial.println("Failed to set NO_RF state");
        return false;
    }
    delay(1000);

    // Try to force LTE-M (non-fatal — modem may auto-select)
    if (!modem.setRAT(WALTER_MODEM_RAT_LTEM)) {
        Serial.println("Warning: could not set RAT to LTE-M, continuing with auto");
    } else {
        Serial.println("RAT set to LTE-M");
    }

    // Define PDP context
    if (!modem.definePDPContext(1, CELL_APN)) {
        Serial.println("Failed to define PDP context");
        return false;
    }

    // Set auth params if credentials provided
    const char* apnUser = APN_USERNAME;
    if (apnUser[0] != '\0') {
        if (!modem.setPDPAuthParams(WALTER_MODEM_PDP_AUTH_PROTO_PAP,
                                    APN_USERNAME, APN_PASSWORD)) {
            Serial.println("Failed to set PDP auth params");
            return false;
        }
    }

    // Enable RF
    if (!modem.setOpState(WALTER_MODEM_OPSTATE_FULL)) {
        Serial.println("Failed to set FULL op state");
        return false;
    }

    // Automatic network selection
    if (!modem.setNetworkSelectionMode(WALTER_MODEM_NETWORK_SEL_MODE_AUTOMATIC)) {
        Serial.println("Failed to set network selection mode");
        return false;
    }

    // Wait for network registration
    Serial.println("Waiting for LTE-M network...");
    for (int i = 0; i < 180; i++) {
        WalterModemNetworkRegState regState = modem.getNetworkRegState();
        if (regState == WALTER_MODEM_NETWORK_REG_REGISTERED_HOME ||
            regState == WALTER_MODEM_NETWORK_REG_REGISTERED_ROAMING) {
            Serial.println("Connected to LTE-M network!");
            return initSockets();
        }
        delay(1000);
        if (i % 10 == 0) {
            Serial.printf("  Still waiting... (%ds)\n", i);
        }
    }

    Serial.println("Network connection timeout");
    return false;
}

bool initSockets()
{
    // TLS profile 2: no cert validation (matches MicroPython), TLS 1.2
    if (!modem.tlsConfigProfile(2, WALTER_MODEM_TLS_VALIDATION_NONE,
                                WALTER_MODEM_TLS_VERSION_12)) {
        Serial.println("TLS profile config failed");
        return false;
    }

    // Configure socket 1 once (reused for both STT and TTS sequentially)
    if (!modem.socketConfig(1, 1, 1500, 90, 30, 0)) {
        Serial.println("Socket 1 config failed");
        return false;
    }

    if (!modem.socketConfigSecure(1, true, 2)) {
        Serial.println("Socket 1 TLS config failed");
        return false;
    }

    Serial.println("Socket 1 initialized");
    return true;
}

bool connectSocket(int socketId, const char* host, uint16_t port)
{
    #if VERBOSITY >= 1
    Serial.printf("Connecting TLS socket %d -> %s:%u\n", socketId, host, port);
    #endif

    if (!modem.socketDial(socketId, WALTER_MODEM_SOCKET_PROTO_TCP, port, host)) {
        Serial.println("Socket dial failed");
        return false;
    }

    #if VERBOSITY >= 1
    Serial.printf("TLS socket %d connected to %s:%u\n", socketId, host, port);
    #endif
    return true;
}

void closeSocket(int socketId)
{
    modem.socketClose(socketId);
    delay(500); // Let modem settle before next dial on same socket ID
}

void resetResponseBuffer(size_t capacity)
{
    freeResponseBuffer();
    responseBuffer = (uint8_t*)ps_calloc(1, capacity);
    responseCapacity = capacity;
    responseLen = 0;
    responseComplete = false;
    socketDisconnected = false;
    pendingRingBytes = 0;
    lastRingMs = 0;
    firstByteMs = 0;
    firstRingAfterResetMs = 0;
}

void freeResponseBuffer()
{
    if (responseBuffer) {
        free(responseBuffer);
        responseBuffer = nullptr;
    }
    responseLen = 0;
    responseCapacity = 0;
    responseComplete = false;
    socketDisconnected = false;
    pendingRingBytes = 0;
    lastRingMs = 0;
    firstByteMs = 0;
    firstRingAfterResetMs = 0;
}

bool flushPendingRing()
{
    if (pendingRingBytes == 0 || !responseBuffer) return false;

    size_t toRead = (pendingRingBytes < 1500) ? pendingRingBytes : 1500;
    if (!WalterModem::socketReceive(pendingConnId, recvBuf, toRead)) {
        return false;
    }

    size_t toWrite = toRead;
    if (responseLen + toWrite > responseCapacity) {
        toWrite = responseCapacity - responseLen;
    }
    if (toWrite > 0) {
        memcpy(responseBuffer + responseLen, recvBuf, toWrite);
        responseLen += toWrite;
    }
    pendingRingBytes -= toRead;

    #if VERBOSITY >= 2
    Serial.printf("  [flush] read %u bytes (total: %u, still pending: %u)\n",
                  (unsigned)toRead, (unsigned)responseLen, (unsigned)pendingRingBytes);
    #endif
    return true;
}
