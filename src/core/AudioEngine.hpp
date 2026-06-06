#ifndef AUDIO_ENGINE_HPP
#define AUDIO_ENGINE_HPP

// ---------------------------------------------------------------------------
// AudioEngine — Project Horus
//
// Military-grade audible feedback module.
// Design constraints (per GEMINI.md):
//  • Zero heap allocation in the render/worker hot path.
//  • PCM samples are synthesised once during init()/applyConfig() and stored
//    as registered SystemSoundIDs (macOS AudioToolbox).
//  • AudioServicesPlaySystemSound() is non-blocking and thread-safe.
// ---------------------------------------------------------------------------

#include <AudioToolbox/AudioToolbox.h>
#include <chrono>
#include <cstdint>
#include <vector>

class AudioEngine {
public:
    // -----------------------------------------------------------------------
    // Configuration — apply via applyConfig() to re-synthesise sound buffers.
    // -----------------------------------------------------------------------
    struct Config {
        bool  masterEnabled          = true;
        float masterVolume           = 0.7f;   // [0.0 – 1.0] baked into PCM amplitude

        // Motion Detection alert
        bool  motionEnabled          = true;
        float motionFreqHz           = 880.0f;
        float motionDurationMs       = 80.0f;
        float motionCooldownSec      = 1.0f;   // Min. seconds between two motion beeps

        // Alarm Zone entry
        bool  alarmEntryEnabled      = true;
        float alarmEntryFreqHz       = 1200.0f;
        float alarmEntryDurMs        = 120.0f;

        // Alarm Zone exit
        bool  alarmExitEnabled       = true;
        float alarmExitFreqHz        = 440.0f;
        float alarmExitDurMs         = 80.0f;

        // Target Lock acquired
        bool  lockAcquiredEnabled    = true;
        float lockAcquiredFreqHz     = 1000.0f;
        float lockAcquiredDurMs      = 150.0f;

        // Target Lock lost
        bool  lockLostEnabled        = true;
        float lockLostFreqHz         = 300.0f;
        float lockLostDurMs          = 200.0f;
    };

    // Sound event identifiers
    enum class Event {
        MOTION_ALERT,
        ALARM_ENTRY,
        ALARM_EXIT,
        LOCK_ACQUIRED,
        LOCK_LOST
    };

    AudioEngine();
    ~AudioEngine();

    // Must be called once from the main thread before any play* call.
    void init(const Config& cfg);

    // Re-synthesises all sound buffers from the new config.
    // Call from main/render thread only (not from the worker hot-path).
    void applyConfig(const Config& cfg);

    // Read-only access to the active config.
    const Config& config() const { return m_cfg; }

    // Non-blocking, fire-and-forget playback — safe to call from any thread.
    void playEvent(Event e);

    // Convenience wrappers
    void playMotionAlert()  { playEvent(Event::MOTION_ALERT);  }
    void playAlarmEntry()   { playEvent(Event::ALARM_ENTRY);   }
    void playAlarmExit()    { playEvent(Event::ALARM_EXIT);    }
    void playLockAcquired() { playEvent(Event::LOCK_ACQUIRED); }
    void playLockLost()     { playEvent(Event::LOCK_LOST);     }

    // Returns true if enough time has passed since the last motion beep.
    bool motionCooldownElapsed();

    // Record the timestamp of a played motion beep.
    void recordMotionBeep();

    void shutdown();

private:
    // One sound buffer: synthesised PCM + registered SystemSoundID.
    // The aiffBlob is kept alive until dispose() since AudioToolbox holds a
    // reference to the raw data pointer for the lifetime of the AudioFileID.
    struct SoundBuffer {
        std::vector<int16_t>   samples;          // PCM data (host byte order)
        std::vector<uint8_t>*  aiffBlob = nullptr; // Heap-allocated AIFF container
        std::string            tempFilePath;
        SystemSoundID          soundID  = 0;
        bool                   valid    = false;

        void dispose();
    };

    // Synthesises a custom waveform based on the event type and registers it as a SystemSoundID.
    static SoundBuffer buildSound(Event type, float freqHz, float durationMs, float volume);

    Config      m_cfg;
    SoundBuffer m_motionSound;
    SoundBuffer m_alarmEntrySound;
    SoundBuffer m_alarmExitSound;
    SoundBuffer m_lockAcquiredSound;
    SoundBuffer m_lockLostSound;

    std::chrono::steady_clock::time_point m_lastMotionBeep;
    bool m_initialised = false;
};

#endif // AUDIO_ENGINE_HPP
