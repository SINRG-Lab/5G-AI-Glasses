#include <Arduino.h>
#include <driver/i2s.h>
#include <FFat.h>

#include "config.h"
#include "modem_setup.h"
#include "audio_utils.h"
#include "deepgram_api.h"
#include "openai_realtime_api.h"

// ================================================================
//  AUDIO TEST  — single file, full latency breakdown, serial dump
// ================================================================

void runAudioTest()
{
    Serial.println("\n==================================================");
    Serial.println("Walter - Audio Test");
    Serial.println("==================================================\n");

    if (!audioFsInit()) { Serial.println("FATAL: filesystem init failed"); return; }
    if (!modemInit())   { Serial.println("FATAL: modem init failed");      return; }

    uint8_t* wav = nullptr;
    size_t wavLen = 0;

    unsigned long pipelineStart = millis();
    bool ok = deepgram::audioToAudio(AUDIO_FILE, &wav, &wavLen,
                                     DG_AGENT_PROMPT, DG_AGENT_VOICE);
    unsigned long pipelineMs = millis() - pipelineStart;

    Serial.printf("\nTotal pipeline time: %.2f s\n", pipelineMs / 1000.0f);

    if (!ok || !wav) {
        Serial.println("FATAL: pipeline failed");
        if (wav) free(wav);
        return;
    }

    // Dump audio to serial for extraction via save_serial_wav.py
    size_t b64Len = 0;
    char*  b64    = base64Encode(wav, wavLen, &b64Len);
    free(wav);

    if (b64) {
        Serial.println("---BEGIN WAV BASE64---");
        Serial.println(b64);
        Serial.println("---END WAV BASE64---");
        free(b64);
    }
}

// ================================================================
//  LATENCY BENCHMARK  — n trials across 3 audio files, no dump
// ================================================================

// Utterance set matched to the baseline devices' short spoken queries
// (Appendix A style). The 9.7 s carwash clip was dropped: its 83.7 KB upload
// repeatedly destabilized the LTE-M link (re-registration storms) and is far
// longer than any baseline query. Smallest clip first so early trials bank
// data before the link is stressed.
// president_question.wav removed: Deepgram returns the transcript and a text
// answer for it but closes without audio (endpointing turn disruption), so it
// produces no usable trials. Two slots for msg_tiny to keep n divisible by 3.
// All three slots use msg_tiny to get 30 trials of the working clip
// (president_question fails server-side; msg_small dropped for this run).
static const char* BENCH_FILES[3] = {
    "/msg_tiny.wav",
    "/msg_tiny.wav",
    "/msg_tiny.wav"
};

static const char* BENCH_LABELS[3] = {
    "msg_tiny.wav",
    "msg_tiny.wav",
    "msg_tiny.wav"
};

void runLatencyBenchmark(int n)
{
    if (n % 3 != 0) {
        Serial.printf("\nERROR: n must be divisible by 3 (got %d)\n", n);
        return;
    }

    int trialsPerFile = n / 3;

    Serial.println("\n==================================================");
    Serial.printf("Walter - Latency Benchmark  (%d trials, %d per file)\n", n, trialsPerFile);
    Serial.println("==================================================\n");

    if (!audioFsInit()) { Serial.println("FATAL: filesystem init failed"); return; }
    if (!modemInit())   { Serial.println("FATAL: modem init failed");      return; }

#if BENCH_USE_OPENAI
    // OpenAI Realtime backend (no upload pacing implemented yet — uploads
    // run at link speed, so first_resp is not directly live-equivalent).
    Serial.println("BENCH_BACKEND,openai_realtime");
    Serial.println("BENCH_MODE,unpaced");
#else
    // Pace uploads at the live mic rate so trials reproduce live streaming
    // timing (end of speech = upload end - 800 ms silence pad, wall-clock).
    deepgram::paceRealtime = true;
    Serial.println("BENCH_BACKEND,deepgram");
    Serial.println("BENCH_MODE,paced_realtime");
#endif

    // Machine-parseable header (CSV)
    // MATLAB parses lines starting with "DATA," — everything else is ignored.
    // first_resp_ms / last_resp_ms are relative to audio-send-start, so they
    // capture the overlap between upload and Deepgram's streaming response.
    Serial.println("DATA,file,trial,enc_ms,up_ms,first_resp_ms,last_resp_ms,dec_ms,up_bytes,dl_bytes");

    int globalTrial = 0;

    for (int fi = 0; fi < 3; fi++) {
        Serial.printf("\n=== File %d/3: %s ===\n", fi + 1, BENCH_LABELS[fi]);

        for (int t = 0; t < trialsPerFile; t++) {
            globalTrial++;
            Serial.printf("\n[Trial %d/%d]\n", t + 1, trialsPerFile);

            uint8_t*         wav    = nullptr;
            size_t           wavLen = 0;

#if BENCH_USE_OPENAI
            openai_realtime::Timing timing;
            bool ok = openai_realtime::audioToAudio(BENCH_FILES[fi], &wav, &wavLen,
                                                    AGENT_PROMPT,
                                                    &timing, /*printBreakdown=*/false);
#else
            deepgram::Timing timing;
            bool ok = deepgram::audioToAudio(BENCH_FILES[fi], &wav, &wavLen,
                                             DG_AGENT_PROMPT, DG_AGENT_VOICE,
                                             &timing, /*printBreakdown=*/false);
#endif

            if (wav) { free(wav); wav = nullptr; }

            if (!ok) {
                Serial.println("  [TRIAL FAILED]");
                continue;
            }

            // Machine-parseable data row
            Serial.printf("DATA,%s,%d,%lu,%lu,%lu,%lu,%lu,%u,%u\n",
                          BENCH_LABELS[fi], globalTrial,
                          timing.encodeMs, timing.uploadMs,
                          timing.firstResponseMs, timing.lastResponseMs,
                          timing.decodeMs,
                          (unsigned)timing.uploadBytes,
                          (unsigned)timing.downloadBytes);
        }
    }

    Serial.println("\nDATA,END");
    Serial.println("==================================================");
    Serial.println("Benchmark complete.");
    Serial.println("==================================================\n");
}

// ================================================================
//  MIC TEST  — button-triggered record → playback loop
// ================================================================

// Max recording length: 10 seconds at 16kHz 16-bit mono
static const size_t MIC_MAX_BYTES = 16000 * 2 * 10;

static uint8_t* micRecBuf  = nullptr; // PSRAM recording buffer
static size_t   micRecLen  = 0;       // bytes captured so far

typedef enum { MIC_IDLE, MIC_RECORDING, MIC_PLAYING } MicState;
static MicState micState = MIC_IDLE;
static unsigned long micRecStartMs = 0;
static const unsigned long MIC_MIN_REC_MS = 500; // ignore button for first 500ms of recording

// Analog threshold for button press detection.
// ESP32-S3 ADC: 0–4095 maps to 0–3.3V.  Require > ~2.0V to reject
// the ~0.745V idle bias and RF-coupled noise spikes on GPIO 13.
static const int BTN_ADC_THRESHOLD = 2400;

static bool btnDebounced()
{
    int a = analogRead(BTN_PIN);
    delay(10);
    int b = analogRead(BTN_PIN);
    return (a > BTN_ADC_THRESHOLD) && (b > BTN_ADC_THRESHOLD);
}

static void i2sMicInit()
{
    // L/R LOW = left channel output, matches I2S_CHANNEL_FMT_ONLY_LEFT
    pinMode(MIC_LR_PIN, OUTPUT);
    digitalWrite(MIC_LR_PIN, LOW);

    i2s_driver_uninstall(I2S_NUM_1);
    delay(50);

    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate          = 16000;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT; // INMP441 packs in 32-bit frames
    cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = 8;
    cfg.dma_buf_len          = 256;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = false;
    cfg.fixed_mclk           = 0;

    i2s_pin_config_t pins = {};
    pins.bck_io_num   = MIC_SCK_PIN;
    pins.ws_io_num    = MIC_WS_PIN;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num  = MIC_DIN_PIN;

    esp_err_t e = i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL);
    if (e != ESP_OK) Serial.printf("[MIC] i2s_driver_install failed: %d\n", (int)e);

    e = i2s_set_pin(I2S_NUM_1, &pins);
    if (e != ESP_OK) Serial.printf("[MIC] i2s_set_pin failed: %d\n", (int)e);
    else             Serial.println("[MIC] I2S ready");

    i2s_zero_dma_buffer(I2S_NUM_1);
}

// Mono→stereo interleave buffer (512 mono samples → 1024 stereo samples)
static const size_t STEREO_CHUNK_MONO = 512;
static int16_t stereoChunk[STEREO_CHUNK_MONO * 2];

// Write mono PCM to I2S with stereo interleaving (L=R).
// Matches the approach in the reference AIGlasses scripts that fixed
// audio quality issues with the MAX98357A amp.
static void i2sWriteMono(const int16_t* mono, size_t monoBytes)
{
    size_t offset = 0;
    while (offset < monoBytes) {
        size_t chunkBytes = min((size_t)(STEREO_CHUNK_MONO * 2), monoBytes - offset);
        size_t samples    = chunkBytes / 2;
        const int16_t* src = (const int16_t*)((const uint8_t*)mono + offset);

        for (size_t i = 0; i < samples; i++) {
            stereoChunk[2 * i]     = src[i]; // LEFT
            stereoChunk[2 * i + 1] = src[i]; // RIGHT
        }

        size_t written = 0;
        i2s_write(I2S_NUM_0, stereoChunk, samples * 4, &written, portMAX_DELAY);
        offset += chunkBytes;
    }
}

static void i2sSpkInit(uint32_t sampleRate = 16000)
{
    i2s_driver_uninstall(I2S_NUM_0);
    delay(50);

    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = sampleRate;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = 16;
    cfg.dma_buf_len          = 1024;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = true;
    cfg.fixed_mclk           = 0;

    i2s_pin_config_t pins = {};
    pins.bck_io_num   = I2S_BCLK_PIN;
    pins.ws_io_num    = I2S_LRC_PIN;
    pins.data_out_num = I2S_DOUT_PIN;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;

    esp_err_t e = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    if (e != ESP_OK) Serial.printf("[SPK] i2s_driver_install failed: %d\n", (int)e);

    e = i2s_set_pin(I2S_NUM_0, &pins);
    if (e != ESP_OK) Serial.printf("[SPK] i2s_set_pin failed: %d\n", (int)e);
    else             Serial.println("[SPK] I2S ready");

    i2s_zero_dma_buffer(I2S_NUM_0);
}

void runMicTest()
{
    Serial.println("\n==================================================");
    Serial.println("Walter - Mic/Speaker/Button Test");
    Serial.println("==================================================\n");

    // Enable 3V3-OUT rail
    pinMode(0, OUTPUT);
    digitalWrite(0, HIGH);
    delay(100);

    // Button: input with pulldown (active HIGH — wire button to 3.3V, not GND)
    pinMode(BTN_PIN, INPUT_PULLDOWN);

    // Allocate PSRAM recording buffer
    micRecBuf = (uint8_t*)ps_malloc(MIC_MAX_BYTES);
    if (!micRecBuf) {
        Serial.println("FATAL: PSRAM alloc failed for mic buffer");
        return;
    }

    // Start both I2S peripherals
    i2sMicInit();
    i2sSpkInit();

    micState = MIC_IDLE;
    Serial.println("Ready. Press button to start recording.");
}

// 32-bit read buffer for INMP441 (shifts to 16-bit)
static int32_t micReadBuf32[256];

void micTestLoop()
{
    switch (micState) {

    case MIC_IDLE:
        if (btnDebounced()) {
            while (analogRead(BTN_PIN) > BTN_ADC_THRESHOLD) delay(10);
            micRecLen    = 0;
            micRecStartMs = millis();
            micState     = MIC_RECORDING;
            Serial.println("[REC] Recording... press button to stop.");
        }
        break;

    case MIC_RECORDING: {
        // Read a chunk from mic
        size_t bytesRead = 0;
        i2s_read(I2S_NUM_1, micReadBuf32, sizeof(micReadBuf32),
                 &bytesRead, 20 / portTICK_PERIOD_MS);

        // Convert 32-bit INMP441 frames to 16-bit and store
        size_t samples = bytesRead / 4;
        int16_t* dst = (int16_t*)(micRecBuf + micRecLen);
        size_t spaceLeft = (MIC_MAX_BYTES - micRecLen) / 2;
        size_t toStore = min(samples, spaceLeft);
        for (size_t i = 0; i < toStore; i++) {
            dst[i] = (int16_t)(micReadBuf32[i] >> 16);
        }
        micRecLen += toStore * 2;

        // Check button (only after min recording time) or buffer full
        bool minTimeElapsed = (millis() - micRecStartMs) >= MIC_MIN_REC_MS;
        if ((minTimeElapsed && btnDebounced()) || micRecLen >= MIC_MAX_BYTES) {
            while (analogRead(BTN_PIN) > BTN_ADC_THRESHOLD) delay(10);
            Serial.printf("[REC] Captured %u bytes (%.2f s)\n",
                          (unsigned)micRecLen, micRecLen / (16000.0f * 2.0f));

                micState = MIC_PLAYING;
        }
        break;
    }

    case MIC_PLAYING: {
        // Apply gain to recorded samples in-place before playback
        int16_t* samples = (int16_t*)micRecBuf;
        size_t totalSamples = micRecLen / 2;
        for (size_t i = 0; i < totalSamples; i++) {
            int32_t s = (int32_t)(samples[i] * SOFTWARE_GAIN);
            if      (s >  32767) s =  32767;
            else if (s < -32768) s = -32768;
            samples[i] = (int16_t)s;
        }

        // Play recorded audio from PSRAM (stereo interleaved)
        Serial.printf("[PLAY] Starting playback of %u bytes\n", (unsigned)micRecLen);
        i2sWriteMono((const int16_t*)micRecBuf, micRecLen);
        i2s_zero_dma_buffer(I2S_NUM_0);
        delay(300);
        Serial.println("[PLAY] Done. Press button to record again.");

        // Wait for button press to go back to recording
        while (!btnDebounced()) delay(20);
        while (analogRead(BTN_PIN) <= BTN_ADC_THRESHOLD) delay(10);
        micRecLen     = 0;
        micRecStartMs = millis();
        micState      = MIC_RECORDING;
        Serial.println("[REC] Recording... press button to stop.");
        break;
    }
    }
}

// ================================================================
//  SPEAKER TEST  — play sine tone via I2S in a loop
// ================================================================

static bool speakerReady = false;
static uint8_t* wavPlayBuf = nullptr;
static size_t wavPlayLen = 0;

void runSpeakerTest()
{
    Serial.println("\n==================================================");
    Serial.println("Walter - Speaker Test (sine loop)");
    Serial.println("==================================================\n");

    // Enable 3V3-OUT rail (pin 26) by driving IO0 high
    pinMode(0, OUTPUT);
    digitalWrite(0, HIGH);
    delay(100);
    Serial.println("3V3-OUT enabled (IO0 -> HIGH)");

    // --- Stream carwash_question.wav from FFat directly to I2S ---
    if (!audioFsInit()) { Serial.println("FATAL: filesystem init failed"); return; }

    File f = FFat.open("/carwash_question.wav", "r");
    if (!f) { Serial.println("FATAL: could not open carwash_question.wav"); return; }

    // Read RIFF header (12 bytes)
    uint8_t riffHdr[12];
    if (f.read(riffHdr, 12) != 12 ||
        memcmp(riffHdr, "RIFF", 4) != 0 || memcmp(riffHdr + 8, "WAVE", 4) != 0) {
        Serial.println("FATAL: not a valid WAV file");
        f.close();
        return;
    }

    // Walk chunks to find fmt and data
    uint16_t numChannels   = 0;
    uint32_t wavSampleRate = 0;
    uint16_t bitsPerSample = 0;
    uint16_t audioFormat   = 0;
    uint32_t dataLen       = 0;
    bool     foundData     = false;

    uint8_t chunkHdr[8];
    while (f.read(chunkHdr, 8) == 8) {
        uint32_t chunkSize;
        memcpy(&chunkSize, chunkHdr + 4, 4);

        if (memcmp(chunkHdr, "fmt ", 4) == 0 && chunkSize >= 16) {
            uint8_t fmt[16];
            f.read(fmt, 16);
            memcpy(&audioFormat,   fmt + 0,  2);
            memcpy(&numChannels,   fmt + 2,  2);
            memcpy(&wavSampleRate, fmt + 4,  4);
            memcpy(&bitsPerSample, fmt + 14, 2);
            // skip any remaining fmt bytes
            if (chunkSize > 16) f.seek(chunkSize - 16, SeekCur);
        } else if (memcmp(chunkHdr, "data", 4) == 0) {
            dataLen    = chunkSize;
            foundData  = true;
            break; // file position is now at start of PCM data
        } else {
            f.seek(chunkSize, SeekCur); // skip unknown chunk
        }
    }

    if (!foundData) {
        Serial.println("FATAL: no data chunk found");
        f.close();
        return;
    }

    Serial.printf("WAV: %u Hz, %u-bit, %u ch, fmt %u, data %u bytes\n",
                  wavSampleRate, bitsPerSample, numChannels, audioFormat, dataLen);

    // Configure I2S to match WAV parameters
    i2s_config_t i2sConfig = {};
    i2sConfig.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    i2sConfig.sample_rate          = wavSampleRate;
    i2sConfig.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    i2sConfig.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
    i2sConfig.communication_format = I2S_COMM_FORMAT_I2S;
    i2sConfig.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    i2sConfig.dma_buf_count        = 8;
    i2sConfig.dma_buf_len          = 512;
    i2sConfig.use_apll             = false;
    i2sConfig.tx_desc_auto_clear   = true;
    i2sConfig.fixed_mclk           = 0;

    i2s_pin_config_t pinConfig = {
        .bck_io_num   = I2S_BCLK_PIN,
        .ws_io_num    = I2S_LRC_PIN,
        .data_out_num = I2S_DOUT_PIN,
        .data_in_num  = I2S_PIN_NO_CHANGE,
    };

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2sConfig, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("FATAL: i2s_driver_install failed: %d\n", err);
        f.close();
        return;
    }
    err = i2s_set_pin(I2S_NUM_0, &pinConfig);
    if (err != ESP_OK) {
        Serial.printf("FATAL: i2s_set_pin failed: %d\n", err);
        i2s_driver_uninstall(I2S_NUM_0);
        f.close();
        return;
    }
    i2s_zero_dma_buffer(I2S_NUM_0);
    Serial.printf("I2S ready: %u Hz, BCLK=GPIO%d, LRC=GPIO%d, DOUT=GPIO%d\n",
                  wavSampleRate, I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DOUT_PIN);

    // Load entire PCM data into PSRAM before playback to avoid FFat read
    // latency causing DMA underruns (choppy audio)
    uint8_t* audioBuf = (uint8_t*)ps_malloc(dataLen);
    if (!audioBuf) {
        Serial.println("FATAL: PSRAM alloc failed for audio buffer");
        f.close();
        i2s_driver_uninstall(I2S_NUM_0);
        return;
    }

    size_t bytesLoaded = f.read(audioBuf, dataLen);
    f.close();
    Serial.printf("Loaded %u bytes into PSRAM\n", (unsigned)bytesLoaded);

    // Apply software gain and clamp
    size_t totalSamples = bytesLoaded / 2;
    int16_t* samples = (int16_t*)audioBuf;
    for (size_t i = 0; i < totalSamples; i++) {
        int32_t s = (int32_t)(samples[i] * SOFTWARE_GAIN);
        if      (s >  32767) s =  32767;
        else if (s < -32768) s = -32768;
        samples[i] = (int16_t)s;
    }

    Serial.printf("Playing %u bytes (gain=%.1fx)...\n", (unsigned)bytesLoaded, (float)SOFTWARE_GAIN);
    unsigned long startMs = millis();

    size_t offset = 0;
    while (offset < bytesLoaded) {
        size_t chunk = min((size_t)2048, bytesLoaded - offset);
        size_t written = 0;
        i2s_write(I2S_NUM_0, audioBuf + offset, chunk, &written, portMAX_DELAY);
        offset += written;
    }

    // Keep buffer in PSRAM for looped playback
    wavPlayBuf = audioBuf;
    wavPlayLen = bytesLoaded;

    i2s_zero_dma_buffer(I2S_NUM_0);
    delay(100);
    Serial.printf("WAV playback complete in %.2f s — looping...\n", (millis() - startMs) / 1000.0f);
    speakerReady = true;
}

// ================================================================
//  AGENT MODE — live mic → Deepgram → speaker
// ================================================================

// MicReadFn callback: reads one chunk from mic, converts 32→16 bit
static int32_t agentRecBuf32[512];

static size_t agentMicRead(int16_t* pcmBuf, size_t maxSamples)
{
    size_t toRead = min(maxSamples, (size_t)512);
    size_t bytesRead = 0;
    i2s_read(I2S_NUM_1, agentRecBuf32, toRead * 4,
             &bytesRead, 50 / portTICK_PERIOD_MS);
    size_t samples = bytesRead / 4;
    for (size_t i = 0; i < samples; i++)
        pcmBuf[i] = (int16_t)(agentRecBuf32[i] >> 16);
    return samples;
}

// StopFn callback: debounced button check (analog threshold)
static bool agentCheckStop()
{
    int a = analogRead(BTN_PIN);
    delay(10);
    int b = analogRead(BTN_PIN);
    return (a > BTN_ADC_THRESHOLD) && (b > BTN_ADC_THRESHOLD);
}

void runAgentMode()
{
    Serial.println("\n==================================================");
    Serial.println("Walter - AI Glasses Agent");
    Serial.println("==================================================\n");

    // Keep amp silent while I2S is not active — drive DIN pin LOW
    pinMode(I2S_DOUT_PIN, OUTPUT);
    digitalWrite(I2S_DOUT_PIN, LOW);

    // Connect to LTE-M FIRST, before enabling 3V3-OUT.
    // The modem draws peak current during RF power-on and network
    // registration (500mA+ bursts). Enabling 3V3-OUT at the same time
    // overloads the shared 3.3V rail, sagging it to ~0.75V.
    // After registration the modem drops to idle/DRX power — safe to
    // turn on peripherals.
    if (!modemInit()) {
        Serial.println("FATAL: modem init failed");
        return;
    }

    // Now enable 3V3-OUT for mic, amp, and button
    pinMode(0, OUTPUT);
    digitalWrite(0, HIGH);
    delay(200);
    Serial.printf("[AGENT] 3V3-OUT enabled (post-modem). BTN ADC = %d\n",
                  analogRead(BTN_PIN));

    // Button: pulldown (active HIGH — button wired to 3.3V)
    pinMode(BTN_PIN, INPUT_PULLDOWN);

    // Wait for button pin to settle
    delay(200);
    while (analogRead(BTN_PIN) > BTN_ADC_THRESHOLD) {
        Serial.println("Waiting for button to be released...");
        delay(100);
    }

    Serial.println("Ready. Press button to speak.");
}

void runAgentLoop()
{
    // Wait for button press (non-blocking)
    if (!agentCheckStop()) { delay(20); return; }
    Serial.println("[AGENT] Button pressed");
    while (analogRead(BTN_PIN) > BTN_ADC_THRESHOLD) delay(10); // wait for release
    Serial.println("[AGENT] Button released");
    Serial.flush();

    Serial.printf("[AGENT] Free heap: %u  Free PSRAM: %u\n",
                  esp_get_free_heap_size(), esp_get_free_internal_heap_size());
    Serial.flush();

    // Init mic I2S now (only while actively recording/streaming)
    i2sMicInit();
    Serial.println("[AGENT] Calling liveToAudio...");
    Serial.flush();

    uint8_t* pcm16    = nullptr;
    size_t   pcm16Len = 0;

    bool ok = deepgram::liveToAudio(agentMicRead, agentCheckStop,
                                    &pcm16, &pcm16Len,
                                    DG_AGENT_PROMPT, DG_AGENT_VOICE);

    // Tear down mic I2S — modem and I2S should not run simultaneously
    i2s_driver_uninstall(I2S_NUM_1);

    if (!ok || !pcm16) {
        Serial.println("[AGENT] Pipeline failed");
        if (pcm16) free(pcm16);
        delay(1000);
        return;
    }

    Serial.printf("[AGENT] Response: %u bytes PCM16 @ 8kHz — playing\n",
                  (unsigned)pcm16Len);

    // Apply software gain (lower than mic path — Deepgram TTS is already normalized)
    int16_t* samples     = (int16_t*)pcm16;
    size_t   totalSamples = pcm16Len / 2;
    for (size_t i = 0; i < totalSamples; i++) {
        int32_t s = (int32_t)(samples[i] * AGENT_PLAYBACK_GAIN);
        if      (s >  32767) s =  32767;
        else if (s < -32768) s = -32768;
        samples[i] = (int16_t)s;
    }

    // Init speaker I2S at 8kHz for playback (stereo interleaved)
    i2sSpkInit(8000);
    i2sWriteMono((const int16_t*)pcm16, pcm16Len);
    i2s_zero_dma_buffer(I2S_NUM_0);
    delay(100);
    i2s_driver_uninstall(I2S_NUM_0);

    // Drive DIN low so amp stays silent while modem is idle
    pinMode(I2S_DOUT_PIN, OUTPUT);
    digitalWrite(I2S_DOUT_PIN, LOW);

    free(pcm16);
    Serial.println("[AGENT] Done. Press button to speak again.");
}

// ================================================================
//  ENTRY POINT
// ================================================================

void setup()
{
    Serial.begin(115200);
    delay(3000);

#if RUN_MODE == MODE_TEST
    runAudioTest();
#elif RUN_MODE == MODE_BENCHMARK
    runLatencyBenchmark(30);
#elif RUN_MODE == MODE_SPEAKER_TEST
    runSpeakerTest();
#elif RUN_MODE == MODE_MIC_TEST
    runMicTest();
#elif RUN_MODE == MODE_AGENT
    runAgentMode();
#else
    Serial.println("ERROR: Unknown RUN_MODE in config.h");
#endif
}

void loop()
{
#if RUN_MODE == MODE_SPEAKER_TEST
    if (speakerReady && wavPlayBuf) {
        Serial.println("Playing carwash_question.wav...");
        size_t offset = 0;
        while (offset < wavPlayLen) {
            size_t chunk = min((size_t)2048, wavPlayLen - offset);
            size_t written = 0;
            i2s_write(I2S_NUM_0, wavPlayBuf + offset, chunk, &written, portMAX_DELAY);
            offset += written;
        }
        i2s_zero_dma_buffer(I2S_NUM_0);
        delay(500);
    } else {
        delay(10000);
    }
#elif RUN_MODE == MODE_MIC_TEST
    micTestLoop();
#elif RUN_MODE == MODE_AGENT
    runAgentLoop();
#else
    delay(10000);
#endif
}
