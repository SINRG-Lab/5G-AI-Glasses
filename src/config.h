#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
//  RUN MODE  — pick which program runs (see src/main.cpp)
// ============================================================
#define MODE_TEST          0   // one Deepgram round trip, dumps reply WAV over serial
#define MODE_BENCHMARK     1   // N latency trials across 3 files -> CSV
#define MODE_SPEAKER_TEST  2   // stream a WAV from flash to the amp
#define MODE_MIC_TEST      3   // button: record -> local playback
#define MODE_AGENT         4   // live mic -> Deepgram -> speaker

#define RUN_MODE            MODE_BENCHMARK    // <-- change to pick the mode

// Benchmark backend: 1 = OpenAI Realtime, 0 = Deepgram Voice Agent
#define BENCH_USE_OPENAI    1

// ============================================================
//  API KEYS
// ============================================================
#define DEEPGRAM_API_KEY   "36cdfaef6c75edeb984dbc58cb0c79bf6f6e2961"   // <-- REQUIRED
#define GEMINI_API_KEY     "YOUR_GEMINI_API_KEY_HERE"     // only used if you call gemini::
#define OPENAI_API_KEY     "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"

// ============================================================
//  OPENAI REALTIME (active benchmark + live agent)
// ============================================================
#define OPENAI_REALTIME_MODEL            "gpt-realtime-2.1-mini"
#define OPENAI_REALTIME_REASONING_EFFORT "high"
#define OPENAI_REALTIME_VOICE            "marin"
#define AGENT_PROMPT                     "You are a helpful voice assistant. Answer naturally and briefly."

// ============================================================
//  DEEPGRAM VOICE AGENT
// ============================================================
#define DG_AGENT_PROMPT    "You are a helpful voice assistant. Be concise."
#define DG_AGENT_VOICE     "aura-2-asteria-en"

// ============================================================
//  CELLULAR NETWORK
// ============================================================
#define CELL_APN           ""   // <-- from your SIM provider
#define APN_USERNAME       ""                // leave empty if not required
#define APN_PASSWORD       ""                // leave empty if not required
#define SIM_PIN            ""                // leave empty if no PIN

// ============================================================
//  GEMINI (dormant in current firmware, but must be defined to compile)
// ============================================================
#define GEMINI_STT_MODEL   "gemini-2.5-flash"
#define GEMINI_TTS_MODEL   "gemini-2.5-flash-preview-tts"
#define TTS_VOICE          "Kore"
#define AUDIO_FILE         "/msg_tiny.wav"   // file used by MODE_TEST
#define AUDIO_PROMPT       "Listen to this audio and respond naturally."

// ============================================================
//  I2S / GPIO PINS  — MUST match your physical wiring!
//  These are PLACEHOLDERS. Set them to the GPIOs you actually
//  soldered the mic, amp, and button to (see walter_datasheet.pdf).
// ============================================================
#define I2S_BCLK_PIN       5    // speaker amp (MAX98357A) bit clock  (BCLK)
#define I2S_LRC_PIN        6    // speaker amp word select            (LRC/WS)
#define I2S_DOUT_PIN       7    // speaker amp data out                (DIN)
#define MIC_SCK_PIN        9    // mic (INMP441) bit clock            (SCK)
#define MIC_WS_PIN         10   // mic word select                    (WS)
#define MIC_DIN_PIN        11   // mic data in                        (SD)
#define MIC_LR_PIN         12   // mic L/R channel select             (L/R)
#define BTN_PIN            13   // push button (must be ADC-capable)

// ============================================================
//  AUDIO GAIN  (software volume multipliers, clamped to int16)
// ============================================================
#define SOFTWARE_GAIN        4.0f   // mic-path / speaker-test playback gain
#define AGENT_PLAYBACK_GAIN  2.0f   // agent reply playback gain

// ============================================================
//  DEBUG VERBOSITY: 0=minimal, 1=normal, 2=debug, 3=UART trace
// ============================================================
#define VERBOSITY          1

#endif
