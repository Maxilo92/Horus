#ifndef AUDIO_ENGINE_HPP
#define AUDIO_ENGINE_HPP

// ---------------------------------------------------------------------------
// AudioEngine — Project Horus
//
// Military-grade audible feedback module.
// Design constraints (per GEMINI.md):
//  • Zero heap allocation in the render/worker hot path.
//  • PCM samples are synthesised once during init()/applyConfig().
//  • Playback is non-blocking and thread-safe on all platforms.
// ---------------------------------------------------------------------------

#ifdef __APPLE__
#  include <AudioToolbox/AudioToolbox.h>
#elif defined(_WIN32)
#  include <windows.h>
#  include <mmdeviceapi.h>
#  include <audioclient.h>
#  include <audiopolicy.h>
#endif
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
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
        float motionFreqHz           = 660.0f;  // Lower = less piercing at high repetition rates
        float motionDurationMs       = 100.0f;  // Slightly longer for the Gaussian bell to breathe
        float motionCooldownSec      = 1.0f;   // Min. seconds between two motion beeps

        // Alarm Zone entry
        bool  alarmEntryEnabled      = true;
        float alarmEntryFreqHz       = 1100.0f; // Root frequency; the alternating fifth does the heavy lifting
        float alarmEntryDurMs        = 200.0f;  // Long enough for 4 tone-switches to register

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
        float lockLostFreqHz         = 420.0f;  // Higher start = chirp sweeps through a wider, more dramatic range
        float lockLostDurMs          = 220.0f;  // Longer sweep = the descent reads clearly

        // Lock Pulse — continuous targeting tone that speeds up with lock confidence
        bool  lockPulseEnabled         = true;
        float lockPulseFreqHz          = 920.0f;  // Beep pitch
        float lockPulseDurMs           = 45.0f;   // Each beep duration
        float lockPulseMinIntervalMs   = 110.0f;  // Interval at full lock (fastest)
        float lockPulseMaxIntervalMs   = 750.0f;  // Interval at weak lock (slowest)
        float lockPulseSolutionThresh  = 0.82f;   // Lock strength above this → solution tone
        float lockPulseSolutionFreqHz  = 1400.0f; // "System ready" confirmation pitch
        float lockPulseSolutionDurMs   = 450.0f;  // Duration of the solution tone
    };

    // Sound event identifiers
    enum class Event {
        MOTION_ALERT,
        ALARM_ENTRY,
        ALARM_EXIT,
        LOCK_ACQUIRED,
        LOCK_LOST,
        LOCK_PULSE,     // Short beep fired repeatedly while a target is locked
        LOCK_SOLUTION   // One-shot "target ready" tone at full lock confidence
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

    // ── Lock Pulse API ──────────────────────────────────────────────────────
    // lockStrength in [0.0, 1.0]; drives the pulse interval and solution detection.
    void startLockPulse(float lockStrength);
    void updateLockStrength(float lockStrength);
    void stopLockPulse();

    // Runs a self-contained test: slow pulse → accelerating → solution tone → stop.
    // Fire-and-forget; safe to call from the UI thread.
    void startLockPulseTest();

    // Current lock strength in [0, 1]; used by the audio visualizer.
    float getInstantIntensity() const { return m_lockStrength.load(); }

    bool isPulseEnabled() const { return m_cfg.lockPulseEnabled; }

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
#ifdef __APPLE__
        std::vector<uint8_t>*  aiffBlob = nullptr; // Heap-allocated AIFF container
        std::string            tempFilePath;
        SystemSoundID          soundID  = 0;
#elif defined(_WIN32)
        IAudioClient*          audioClient  = nullptr;
        IAudioRenderClient*    renderClient = nullptr;
        HANDLE                 eventHandle  = nullptr;
        std::atomic<bool>      playing{false};
        std::thread            drainThread;
#endif
        bool                   valid    = false;

        void dispose();
    };

    // Synthesises a custom waveform based on the event type and registers it as a SystemSoundID.
    static SoundBuffer buildSound(Event type, float freqHz, float durationMs, float volume);

    void lockPulseWorker();

    Config      m_cfg;
    SoundBuffer m_motionSound;
    SoundBuffer m_alarmEntrySound;
    SoundBuffer m_alarmExitSound;
    SoundBuffer m_lockAcquiredSound;
    SoundBuffer m_lockLostSound;
    SoundBuffer m_lockPulseSound;
    SoundBuffer m_lockSolutionSound;

    // Lock pulse thread state
    std::thread               m_lockPulseThread;
    std::atomic<bool>         m_lockPulseActive{false};
    std::atomic<bool>         m_lockPulseStop{false};
    std::atomic<float>        m_lockStrength{0.0f};
    std::atomic<bool>         m_solutionFired{false};
    std::atomic<bool>         m_testRunning{false};
    std::mutex                m_lockPulseMutex;
    std::condition_variable   m_lockPulseCv;

    std::chrono::steady_clock::time_point m_lastMotionBeep;
    bool m_initialised = false;
};

#endif // AUDIO_ENGINE_HPP
