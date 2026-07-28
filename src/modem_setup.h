#ifndef MODEM_SETUP_H
#define MODEM_SETUP_H

#include <WalterModem.h>

// How long (ms) with no new ring event before flushing the remaining pending bytes.
// Kept at 200: lowering to 60 (with a 15 ms poll) made flushPendingRing() race
// the async socket event handler over the shared recvBuf and the agent's audio
// frames were lost (0 bytes received). Revisit only with proper locking.
#define RING_CHUNK_TIMEOUT_MS 200

// Response buffer in PSRAM, filled by socket event handler
extern uint8_t* responseBuffer;
extern volatile size_t responseLen;
extern volatile bool responseComplete;
extern volatile bool socketDisconnected;

// Pending ring state — bytes announced by rings but not yet read from modem
extern volatile size_t pendingRingBytes;
extern volatile unsigned long lastRingMs;

// Timestamp of the first byte written into responseBuffer (0 until data arrives)
extern volatile unsigned long firstByteMs;

// Timestamp of the first ring event after the caller resets it to 0.
// Used to measure when Deepgram's response actually arrives at the modem
// (which can happen during upload, before we parse WS frames).
extern volatile unsigned long firstRingAfterResetMs;

// Initialize modem hardware and connect to LTE-M network
bool modemInit();

// One-time TLS socket init: configures TLS profile + socketConfig + socketConfigSecure for socket 1.
// Called automatically by modemInit().
bool initSockets();

// Per-request connect: dials socket socketId to host:port (socketConfig already done by initSockets).
// Returns true on success.
bool connectSocket(int socketId, const char* host, uint16_t port);

// Close a socket, ignoring errors
void closeSocket(int socketId);

// Reset the response buffer for a new request
void resetResponseBuffer(size_t capacity);

// Free the response buffer
void freeResponseBuffer();

// Flush any pending ring bytes that haven't been read yet (call from main loop on idle timeout)
bool flushPendingRing();

#endif
