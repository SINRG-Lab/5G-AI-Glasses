#include "gemini_api.h"
#include "config.h"
#include "modem_setup.h"
#include "audio_utils.h"
#include <WalterModem.h>
#include <ArduinoJson.h>

namespace gemini {

static const char* GEMINI_HOST = "generativelanguage.googleapis.com";
static const uint16_t GEMINI_PORT = 443;

// Send data over socket in 1500-byte chunks with small delays
static bool sendChunked(int socketId, const uint8_t* data, size_t len)
{
    const size_t chunkSize = 1500;
    size_t sent = 0;

    while (sent < len) {
        size_t toSend = len - sent;
        if (toSend > chunkSize) toSend = chunkSize;

        if (!WalterModem::socketSend(socketId, (uint8_t*)(data + sent), (uint16_t)toSend)) {
            Serial.printf("socketSend failed at byte %u\n", (unsigned)sent);
            return false;
        }
        sent += toSend;

        #if VERBOSITY >= 2
        if (sent % 30000 < chunkSize) {
            Serial.printf("  Sent %u/%u bytes (%u%%)\n", (unsigned)sent, (unsigned)len, (unsigned)(sent * 100 / len));
        }
        #endif
        delay(5); // 5ms between chunks, matching MicroPython
    }

    #if VERBOSITY >= 1
    Serial.printf("Sent %u bytes total\n", (unsigned)sent);
    #endif
    return true;
}

// Wait for the HTTP response to arrive via the socket event handler.
// Polls responseBuffer/responseLen until it can parse a complete response.
// Returns true when done (responseBuffer contains full HTTP response).
static bool waitForResponse(unsigned long timeoutMs, unsigned long idleMs = 5000)
{
    unsigned long start = millis();
    unsigned long lastDataTime = 0;
    size_t lastLen = 0;

    while (millis() - start < timeoutMs) {
        // On disconnect, immediately flush any pending bytes and return.
        // Do this before the chunk-timeout check so short responses (<1500 bytes)
        // that arrive entirely before the server closes the connection are not lost.
        if (socketDisconnected) {
            while (pendingRingBytes > 0) {
                if (!flushPendingRing()) break;
            }
            #if VERBOSITY >= 1
            Serial.println("Response complete (socket closed by server)");
            #endif
            return responseLen > 0;
        }

        // Flush pending ring bytes once they've stopped arriving for RING_CHUNK_TIMEOUT_MS
        if (pendingRingBytes > 0 && lastRingMs > 0 &&
            millis() - lastRingMs > RING_CHUNK_TIMEOUT_MS) {
            flushPendingRing();
        }

        size_t curLen = responseLen;

        if (curLen > lastLen) {
            lastLen = curLen;
            lastDataTime = millis();
        }

        // Check for chunked transfer encoding end marker
        if (curLen > 7) {
            // Look for "0\r\n\r\n" which marks end of chunked response
            const char* bufStr = (const char*)responseBuffer;
            if (memmem(bufStr, curLen, "\r\n0\r\n\r\n", 7) != nullptr) {
                #if VERBOSITY >= 1
                Serial.println("Response complete (chunked end marker)");
                #endif
                return true;
            }
        }

        // If we have data but nothing new for idleMs, consider it done
        if (curLen > 0 && lastDataTime > 0 && millis() - lastDataTime > idleMs) {
            #if VERBOSITY >= 1
            Serial.printf("Response complete (no new data for %lus, %u bytes)\n",
                          idleMs / 1000, (unsigned)curLen);
            #endif
            return true;
        }

        delay(100);
    }

    Serial.printf("Response timeout (%u bytes received)\n", (unsigned)responseLen);
    return responseLen > 0;
}

// Parse HTTP response body from the response buffer.
// Handles both chunked and content-length encoding.
// Returns the body as a String, or empty string on failure.
static String extractHttpBody()
{
    if (responseLen == 0) return "";

    // Find header/body boundary
    const char* buf = (const char*)responseBuffer;
    const char* headerEnd = (const char*)memmem(buf, responseLen, "\r\n\r\n", 4);
    if (!headerEnd) {
        Serial.println("No HTTP header boundary found");
        return "";
    }

    size_t headerLen = headerEnd - buf;
    const char* body = headerEnd + 4;
    size_t bodyLen = responseLen - headerLen - 4;

    #if VERBOSITY >= 2
    // Print status line
    const char* firstLine = (const char*)memchr(buf, '\n', headerLen);
    if (firstLine) {
        Serial.printf("HTTP response: %.*s\n", (int)(firstLine - buf), buf);
    }
    #endif

    // Check for chunked transfer encoding
    bool isChunked = (memmem(buf, headerLen, "chunked", 7) != nullptr);

    if (isChunked) {
        // Decode chunked encoding
        String decoded;
        decoded.reserve(bodyLen);
        const char* p = body;
        const char* end = buf + responseLen;

        while (p < end) {
            // Read chunk size (hex)
            const char* nl = (const char*)memmem(p, end - p, "\r\n", 2);
            if (!nl) break;

            char sizeStr[16] = {};
            size_t sizeLen = nl - p;
            if (sizeLen >= sizeof(sizeStr)) break;
            memcpy(sizeStr, p, sizeLen);

            unsigned long chunkSize = strtoul(sizeStr, nullptr, 16);
            if (chunkSize == 0) break; // Final chunk

            p = nl + 2; // Skip \r\n after size
            if (p + chunkSize > end) {
                // Partial chunk — take what we have
                decoded.concat(p, end - p);
                break;
            }
            decoded.concat(p, chunkSize);
            p += chunkSize + 2; // Skip chunk data + \r\n
        }
        return decoded;
    }

    // Non-chunked: return body directly
    return String(body, bodyLen);
}

// Extract text from Gemini generateContent JSON response
static String parseGeminiTextResponse(const String& body)
{
    // Find the JSON object start
    int jsonStart = body.indexOf('{');
    if (jsonStart < 0) {
        Serial.println("No JSON found in response");
        return "";
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body.c_str() + jsonStart);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        #if VERBOSITY >= 2
        Serial.printf("Body preview: %.500s\n", body.c_str());
        #endif
        return "";
    }

    // Check for API error
    if (doc["error"].is<JsonObject>()) {
        const char* msg = doc["error"]["message"] | "unknown error";
        Serial.printf("Gemini API error: %s\n", msg);
        return "";
    }

    const char* text = doc["candidates"][0]["content"]["parts"][0]["text"];
    if (!text) {
        Serial.println("No text found in Gemini response");
        #if VERBOSITY >= 2
        String dbg;
        serializeJsonPretty(doc, dbg);
        Serial.println(dbg.c_str());
        #endif
        return "";
    }

    return String(text);
}

// Extract base64 audio data from Gemini TTS JSON response
static String parseGeminiTtsResponse(const String& body)
{
    int jsonStart = body.indexOf('{');
    if (jsonStart < 0) {
        Serial.println("No JSON found in TTS response");
        return "";
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body.c_str() + jsonStart);
    if (!err) {
        if (doc["error"].is<JsonObject>()) {
            const char* msg = doc["error"]["message"] | "unknown error";
            Serial.printf("Gemini TTS error: %s\n", msg);
            return "";
        }

        // Try camelCase first (standard Gemini REST response), then snake_case
        const char* b64data = doc["candidates"][0]["content"]["parts"][0]["inlineData"]["data"];
        if (!b64data) {
            b64data = doc["candidates"][0]["content"]["parts"][0]["inline_data"]["data"];
        }
        if (b64data) {
            return String(b64data);
        }

        #if VERBOSITY >= 2
        Serial.println("JSON parsed but audio path not found — dumping structure:");
        String dbg;
        serializeJsonPretty(doc, dbg);
        Serial.println(dbg.c_str());
        #else
        Serial.println("JSON parsed but audio path not found (set VERBOSITY 2 for dump)");
        #endif
    } else {
        Serial.printf("TTS JSON parse error: %s\n", err.c_str());
    }

    // Fallback: extract base64 data via string search (handles wrong path or truncated JSON)
    int dataIdx = body.indexOf("\"data\":\"");
    if (dataIdx < 0) dataIdx = body.indexOf("\"data\": \"");
    if (dataIdx >= 0) {
        int start = body.indexOf('"', dataIdx + 6) + 1;
        int end = body.indexOf('"', start);
        if (end < 0) {
            // Response was truncated — no closing quote; take up to end of body,
            // trimmed to a valid base64 boundary (multiple of 4 chars)
            end = (int)body.length();
            end -= end % 4;
            #if VERBOSITY >= 1
            Serial.printf("Truncated response: extracting partial audio (%d base64 chars)\n",
                          end - start);
            #endif
        }
        if (end > start) {
            #if VERBOSITY >= 1
            Serial.println("Extracted audio via string search fallback");
            #endif
            return body.substring(start, end);
        }
    }

    Serial.println("No audio data found in TTS response");
    return "";
}

// Build the raw HTTP POST request (header + body)
static char* buildHttpRequest(const char* uri, const char* jsonBody, size_t bodyLen, size_t* outLen)
{
    // Build header
    char header[512];
    int headerLen = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "Accept: application/json\r\n"
        "\r\n",
        uri, GEMINI_HOST, (unsigned)bodyLen);

    size_t totalLen = headerLen + bodyLen;
    char* request = (char*)ps_malloc(totalLen + 1);
    if (!request) {
        Serial.println("PSRAM alloc failed for HTTP request");
        return nullptr;
    }

    memcpy(request, header, headerLen);
    memcpy(request + headerLen, jsonBody, bodyLen);
    request[totalLen] = '\0';

    if (outLen) *outLen = totalLen;
    return request;
}

String audioToText(const char* audioPath, const char* prompt)
{
    Serial.println("\n--- Audio to Text ---");

    // Step 1: Load audio file
    uint8_t* audioBuf = nullptr;
    size_t audioLen = 0;
    if (!loadAudioFile(audioPath, &audioBuf, &audioLen)) {
        return "";
    }

    const char* mimeType = detectMimeType(audioBuf, audioLen);
    Serial.printf("Audio: %s, %u bytes\n", mimeType, (unsigned)audioLen);

    // Step 2: Base64 encode
    size_t b64Len = 0;
    char* b64Data = base64Encode(audioBuf, audioLen, &b64Len);
    free(audioBuf); // Free raw audio immediately
    audioBuf = nullptr;

    if (!b64Data) {
        return "";
    }

    // Step 3: Build JSON payload manually to avoid ArduinoJson copying the large b64 string
    // (ArduinoJson silently drops fields it can't allocate, which would omit the audio data)
    size_t jsonCapacity = b64Len + 512;
    char* jsonBuf = (char*)ps_malloc(jsonCapacity);
    if (!jsonBuf) {
        Serial.println("PSRAM alloc failed for JSON");
        free(b64Data);
        return "";
    }

    {
        int jsonLen = snprintf(jsonBuf, jsonCapacity,
            "{\"contents\":[{\"parts\":["
            "{\"inline_data\":{\"mime_type\":\"%s\",\"data\":\"%s\"}},"
            "{\"text\":\"%s\"}"
            "]}],\"generationConfig\":{\"maxOutputTokens\":512,\"temperature\":0.7}}",
            mimeType, b64Data, prompt);

        Serial.printf("JSON payload: %d bytes\n", jsonLen);

        // Free base64 data now that it's embedded in JSON
        free(b64Data);
        b64Data = nullptr;

        // Step 4: Build HTTP request
        char uri[256];
        snprintf(uri, sizeof(uri),
                 "/v1beta/models/%s:generateContent?key=%s",
                 GEMINI_STT_MODEL, GEMINI_API_KEY);

        size_t requestLen = 0;
        char* request = buildHttpRequest(uri, jsonBuf, (size_t)jsonLen, &requestLen);
        free(jsonBuf); // Free JSON buffer
        jsonBuf = nullptr;

        if (!request) {
            return "";
        }

        // Step 5: Setup TLS socket and send
        resetResponseBuffer(64 * 1024); // 64KB for STT response

        if (!connectSocket(1, GEMINI_HOST, GEMINI_PORT)) {
            free(request);
            freeResponseBuffer();
            return "";
        }

        Serial.println("Sending audio to Gemini...");
        unsigned long uploadStartMs = millis();

        if (!sendChunked(1, (const uint8_t*)request, requestLen)) {
            free(request);
            closeSocket(1);
            freeResponseBuffer();
            return "";
        }
        free(request);
        unsigned long uploadDoneMs = millis();

        // Step 6: Wait for response
        if (!waitForResponse(120000)) { // 2 minute timeout
            Serial.printf("waitForResponse failed (%u bytes in buffer)\n", (unsigned)responseLen);
            if (responseLen > 0) {
                String body = extractHttpBody();
                Serial.printf("Response body (%u chars):\n%.2000s\n", body.length(), body.c_str());
            }
            closeSocket(1);
            freeResponseBuffer();
            return "";
        }
        unsigned long downloadDoneMs = millis();

        unsigned long uploadMs   = uploadDoneMs - uploadStartMs;
        unsigned long downloadMs = firstByteMs > 0 ? (downloadDoneMs - firstByteMs) : 0;
        Serial.printf("[STT] Upload:   %lu ms, %u bytes -> %.1f kbps\n",
                      uploadMs, (unsigned)requestLen,
                      uploadMs > 0 ? (requestLen * 8.0f / uploadMs) : 0.0f);
        Serial.printf("[STT] Download: %lu ms, %u bytes -> %.1f kbps\n",
                      downloadMs, (unsigned)responseLen,
                      downloadMs > 0 ? (responseLen * 8.0f / downloadMs) : 0.0f);

        // Step 7: Parse response
        String body = extractHttpBody();
        closeSocket(1);
        freeResponseBuffer();

        if (body.isEmpty()) {
            Serial.println("Empty response body");
            return "";
        }

        String text = parseGeminiTextResponse(body);
        if (text.isEmpty()) {
            Serial.printf("Failed to extract text from response body:\n%.2000s\n", body.c_str());
            return "";
        }

        Serial.printf("Transcription: %s\n", text.c_str());
        return text;
    }
}

bool textToAudio(const char* text, uint8_t** outWav, size_t* outWavLen)
{
    Serial.println("\n--- Text to Audio (TTS) ---");

    // Step 1: Build TTS JSON payload
    JsonDocument doc;
    JsonArray contents = doc["contents"].to<JsonArray>();
    JsonObject content = contents.add<JsonObject>();
    JsonArray parts = content["parts"].to<JsonArray>();
    JsonObject textPart = parts.add<JsonObject>();
    textPart["text"] = text;

    JsonObject genConfig = doc["generationConfig"].to<JsonObject>();
    JsonArray modalities = genConfig["responseModalities"].to<JsonArray>();
    modalities.add("AUDIO");

    JsonObject speechConfig = genConfig["speechConfig"].to<JsonObject>();
    JsonObject voiceConfig = speechConfig["voiceConfig"].to<JsonObject>();
    JsonObject prebuiltVoice = voiceConfig["prebuiltVoiceConfig"].to<JsonObject>();
    prebuiltVoice["voiceName"] = TTS_VOICE;

    // Serialize JSON
    size_t jsonCapacity = 1024 + strlen(text);
    char* jsonBuf = (char*)ps_malloc(jsonCapacity);
    if (!jsonBuf) {
        Serial.println("PSRAM alloc failed for TTS JSON");
        return false;
    }

    size_t jsonLen = serializeJson(doc, jsonBuf, jsonCapacity);
    jsonBuf[jsonLen] = '\0';

    Serial.printf("TTS payload: %u bytes\n", (unsigned)jsonLen);

    // Step 2: Build HTTP request
    char uri[256];
    snprintf(uri, sizeof(uri),
             "/v1beta/models/%s:generateContent?key=%s",
             GEMINI_TTS_MODEL, GEMINI_API_KEY);

    size_t requestLen = 0;
    char* request = buildHttpRequest(uri, jsonBuf, jsonLen, &requestLen);
    free(jsonBuf);

    if (!request) {
        return false;
    }

    // Step 3: Setup TLS socket and send
    resetResponseBuffer(512 * 1024); // 512KB for TTS response (large audio)

    if (!connectSocket(1, GEMINI_HOST, GEMINI_PORT)) {
        free(request);
        freeResponseBuffer();
        return false;
    }

    Serial.println("Sending TTS request to Gemini...");
    unsigned long uploadStartMs = millis();

    if (!sendChunked(1, (const uint8_t*)request, requestLen)) {
        free(request);
        closeSocket(1);
        freeResponseBuffer();
        return false;
    }
    free(request);
    unsigned long uploadDoneMs = millis();

    // Step 4: Wait for response (longer timeout for TTS)
    if (!waitForResponse(300000, 30000)) { // 5 min overall, 30s idle (TTS streams slowly)
        closeSocket(1);
        freeResponseBuffer();
        return false;
    }
    unsigned long downloadDoneMs = millis();

    unsigned long uploadMs   = uploadDoneMs - uploadStartMs;
    unsigned long downloadMs = firstByteMs > 0 ? (downloadDoneMs - firstByteMs) : 0;
    Serial.printf("[TTS] Upload:   %lu ms, %u bytes -> %.1f kbps\n",
                  uploadMs, (unsigned)requestLen,
                  uploadMs > 0 ? (requestLen * 8.0f / uploadMs) : 0.0f);
    Serial.printf("[TTS] Download: %lu ms, %u bytes -> %.1f kbps\n",
                  downloadMs, (unsigned)responseLen,
                  downloadMs > 0 ? (responseLen * 8.0f / downloadMs) : 0.0f);

    // Step 5: Parse response — extract base64 audio
    String body = extractHttpBody();
    closeSocket(1);

    if (body.isEmpty()) {
        Serial.println("Empty TTS response body");
        freeResponseBuffer();
        return false;
    }

    // Free the response buffer since body is extracted
    freeResponseBuffer();

    // Print first and last 300 chars of body to inspect actual JSON structure
    Serial.printf("Body length: %u\n", body.length());
    Serial.println("--- Body start ---");
    Serial.println(body.substring(0, 300));
    Serial.println("--- Body end ---");
    Serial.println(body.substring(body.length() > 300 ? body.length() - 300 : 0));
    Serial.println("------------------");

    String b64Audio = parseGeminiTtsResponse(body);
    body = ""; // Free the body string

    if (b64Audio.isEmpty()) {
        Serial.println("Failed to extract audio from TTS response");
        return false;
    }

    Serial.printf("Got %u base64 chars of audio\n", b64Audio.length());

    // Step 6: Decode base64 -> PCM
    size_t pcmLen = 0;
    uint8_t* pcmData = base64Decode(b64Audio.c_str(), b64Audio.length(), &pcmLen);
    b64Audio = ""; // Free

    if (!pcmData) {
        return false;
    }

    // Step 7: Build WAV and return to caller
    uint8_t* wavData = buildWavFromPcm(pcmData, pcmLen, outWavLen);
    free(pcmData);

    if (!wavData) {
        return false;
    }

    *outWav = wavData;
    return true;
}

} // namespace gemini
