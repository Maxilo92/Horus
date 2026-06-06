// ---------------------------------------------------------------------------
// AudioEngine.cpp — Project Horus
//
// Synthesises short sine-burst PCM samples and registers them with macOS
// AudioToolbox as SystemSoundIDs.  Playback is fully asynchronous and
// non-blocking.  All heap allocation happens only during init/applyConfig,
// never in the real-time loop.
// ---------------------------------------------------------------------------

#include "AudioEngine.hpp"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <memory>
#include <fstream>
#include <atomic>
#include <cstdio>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr uint32_t kSampleRate    = 44100;
static constexpr uint32_t kBitsPerSample = 16;
static constexpr uint32_t kNumChannels   = 1;
static constexpr float    kPi            = 3.14159265358979323846f;

// ---------------------------------------------------------------------------
// AIFF in-memory helpers
// ---------------------------------------------------------------------------

// Write a big-endian 32-bit value into a byte buffer
static void writeBE32(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >>  8) & 0xFF;
    p[3] =  v        & 0xFF;
}

// Write a big-endian 16-bit value into a byte buffer
static void writeBE16(uint8_t* p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF;
    p[1] =  v       & 0xFF;
}

// Encode a double as IEEE 754 80-bit extended (AIFF COMM sampleRate field)
static void writeExtended80(double value, uint8_t* out) {
    int exponent;
    double fraction = std::frexp(value, &exponent);
    exponent += 16382;
    out[0] = static_cast<uint8_t>((exponent >> 8) & 0x7F);
    out[1] = static_cast<uint8_t>(exponent & 0xFF);
    uint64_t mantissa = static_cast<uint64_t>(fraction * (static_cast<double>(1ULL << 63)) * 2.0);
    for (int i = 0; i < 8; ++i)
        out[2 + i] = static_cast<uint8_t>((mantissa >> (56 - i * 8)) & 0xFF);
}

// Build a minimal valid AIFF file blob from 16-bit mono PCM samples.
// The AIFF format is big-endian; AudioToolbox parses it natively on macOS.
static std::vector<uint8_t> buildAIFFBlob(const std::vector<int16_t>& pcm) {
    const uint32_t numSamples  = static_cast<uint32_t>(pcm.size());
    const uint32_t pcmBytes    = numSamples * sizeof(int16_t);
    const uint32_t commSize    = 18;
    const uint32_t ssndPayload = 8 + pcmBytes;
    const uint32_t formPayload = 4 + 8 + commSize + 8 + ssndPayload;

    std::vector<uint8_t> buf(12 + formPayload, 0);
    uint8_t* p = buf.data();

    // FORM header
    std::memcpy(p, "FORM", 4); p += 4;
    writeBE32(p, formPayload); p += 4;
    std::memcpy(p, "AIFF", 4); p += 4;

    // COMM chunk
    std::memcpy(p, "COMM", 4); p += 4;
    writeBE32(p, commSize); p += 4;
    writeBE16(p, static_cast<uint16_t>(kNumChannels)); p += 2;
    writeBE32(p, numSamples); p += 4;
    writeBE16(p, static_cast<uint16_t>(kBitsPerSample)); p += 2;
    writeExtended80(static_cast<double>(kSampleRate), p); p += 10;

    // SSND chunk
    std::memcpy(p, "SSND", 4); p += 4;
    writeBE32(p, ssndPayload); p += 4;
    writeBE32(p, 0); p += 4;  // offset
    writeBE32(p, 0); p += 4;  // blockSize

    // PCM samples: host int16 → big-endian AIFF bytes
    for (uint32_t i = 0; i < numSamples; ++i) {
        uint16_t s = static_cast<uint16_t>(pcm[i]);
        *p++ = static_cast<uint8_t>((s >> 8) & 0xFF);
        *p++ = static_cast<uint8_t>( s        & 0xFF);
    }

    return buf;
}

// ---------------------------------------------------------------------------
// SoundBuffer::dispose
// ---------------------------------------------------------------------------
void AudioEngine::SoundBuffer::dispose() {
    if (soundID) {
        AudioServicesRemoveSystemSoundCompletion(soundID);
        AudioServicesDisposeSystemSoundID(soundID);
        soundID = 0;
    }
    if (!tempFilePath.empty()) {
        std::remove(tempFilePath.c_str());
        tempFilePath.clear();
    }
    // Free the AIFF blob that was kept alive for AudioToolbox
    delete aiffBlob;
    aiffBlob = nullptr;
    samples.clear();
    valid = false;
}

// ---------------------------------------------------------------------------
// AudioEngine::buildSound — synthesises a custom waveform based on the event type
// ---------------------------------------------------------------------------
AudioEngine::SoundBuffer AudioEngine::buildSound(Event type, float freqHz, float durationMs, float volume) {
    SoundBuffer sb;

    // Clamp to sane ranges
    freqHz     = std::max(20.0f,   std::min(freqHz,     20000.0f));
    durationMs = std::max(10.0f,   std::min(durationMs,  2000.0f));
    volume     = std::max(0.0f,    std::min(volume,         1.0f));

    const uint32_t numSamples = static_cast<uint32_t>(kSampleRate * durationMs / 1000.0f);
    if (numSamples == 0) return sb;

    sb.samples.resize(numSamples);

    const float durationSec = durationMs / 1000.0f;

    for (uint32_t i = 0; i < numSamples; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
        float val = 0.0f;
        float env = 1.0f;

        switch (type) {
            case Event::MOTION_ALERT: {
                // Sonar Radar Ping: Downward frequency sweep from 1.5 * freqHz to 0.8 * freqHz
                const float f_start = freqHz * 1.5f;
                const float f_end   = freqHz * 0.8f;
                const float phi = 2.0f * kPi * (f_start * t + (f_end - f_start) * t * t / (2.0f * durationSec));
                
                // Add second harmonic (30% amplitude)
                val = std::sin(phi) + 0.3f * std::sin(2.0f * phi);
                val /= 1.3f; // Normalize

                // Exponential decay envelope
                constexpr float attackTime = 0.002f; // 2ms click avoidance
                if (t < attackTime) {
                    env = t / attackTime;
                } else {
                    env = std::exp(-5.0f * (t - attackTime) / durationSec);
                }
                break;
            }
            case Event::ALARM_ENTRY: {
                // Threat Warning: Base frequency + tritone harmonic (1.414 * freqHz) modulated with 20 Hz tremolo
                const float f1 = freqHz;
                const float f2 = freqHz * 1.414f;
                val = std::sin(2.0f * kPi * f1 * t) + 0.5f * std::sin(2.0f * kPi * f2 * t);
                val /= 1.5f; // Normalize

                // 20 Hz tremolo (amplitude oscillation between 0.4 and 1.0)
                const float tremolo = 0.7f + 0.3f * std::sin(2.0f * kPi * 20.0f * t);
                val *= tremolo;

                // Linear attack/decay envelope (8ms)
                const float fadeLenSec = std::min(durationSec / 4.0f, 0.008f);
                if (t < fadeLenSec) {
                    env = t / fadeLenSec;
                } else if (t >= durationSec - fadeLenSec) {
                    env = (durationSec - t) / fadeLenSec;
                }
                break;
            }
            case Event::ALARM_EXIT: {
                // Descending clearance chime: frequency sweep from 1.2 * freqHz to 0.8 * freqHz with odd harmonics
                const float f_start = freqHz * 1.2f;
                const float f_end   = freqHz * 0.8f;
                const float phi = 2.0f * kPi * (f_start * t + (f_end - f_start) * t * t / (2.0f * durationSec));

                // Add odd harmonics (square-like mix for woodwind-like richness)
                val = std::sin(phi) + 0.3f * std::sin(3.0f * phi) + 0.1f * std::sin(5.0f * phi);
                val /= 1.4f; // Normalize

                // Linear decay envelope: fully open for first 20%, then linear decay to 0
                const float attackTime = 0.005f; // 5ms attack
                if (t < attackTime) {
                    env = t / attackTime;
                } else {
                    const float holdTime = 0.2f * durationSec;
                    if (t < holdTime) {
                        env = 1.0f;
                    } else {
                        env = (durationSec - t) / (durationSec - holdTime);
                    }
                }
                break;
            }
            case Event::LOCK_ACQUIRED: {
                // Double pip: bip-BIP
                // Pip 1: 0% to 35% of duration, frequency 0.9 * freqHz
                // Silent gap: 35% to 45% of duration
                // Pip 2: 45% to 100% of duration, frequency sweeps 1.3 * freqHz to 1.5 * freqHz
                const float t1_end = 0.35f * durationSec;
                const float t2_start = 0.45f * durationSec;
                
                constexpr float clickFade = 0.005f; // 5ms fades for pips

                if (t < t1_end) {
                    const float f1 = freqHz * 0.9f;
                    const float phi = 2.0f * kPi * f1 * t;
                    val = std::sin(phi) + 0.25f * std::sin(2.0f * phi);
                    val /= 1.25f;

                    // Envelope for pip 1
                    if (t < clickFade) {
                        env = t / clickFade;
                    } else if (t > t1_end - clickFade) {
                        env = (t1_end - t) / clickFade;
                    } else {
                        env = 1.0f;
                    }
                } else if (t < t2_start) {
                    val = 0.0f;
                    env = 0.0f;
                } else {
                    const float t_rel = t - t2_start;
                    const float dur2 = durationSec - t2_start;
                    const float f_start = freqHz * 1.3f;
                    const float f_end = freqHz * 1.5f;
                    const float phi = 2.0f * kPi * (f_start * t_rel + (f_end - f_start) * t_rel * t_rel / (2.0f * dur2));
                    
                    val = std::sin(phi) + 0.25f * std::sin(2.0f * phi);
                    val /= 1.25f;

                    // Envelope for pip 2
                    if (t_rel < clickFade) {
                        env = t_rel / clickFade;
                    } else if (t > durationSec - clickFade) {
                        env = (durationSec - t) / clickFade;
                    } else {
                        env = 1.0f;
                    }
                }
                break;
            }
            case Event::LOCK_LOST: {
                // Downward buzzing error chime: frequency sweep 1.0 * freqHz to 0.5 * freqHz with buzzing harmonics
                const float f_start = freqHz;
                const float f_end   = freqHz * 0.5f;
                const float phi = 2.0f * kPi * (f_start * t + (f_end - f_start) * t * t / (2.0f * durationSec));

                // Rich odd harmonics (simulating square-like warning buzz)
                val = std::sin(phi) + 0.5f * std::sin(3.0f * phi) + 0.25f * std::sin(5.0f * phi) + 0.125f * std::sin(7.0f * phi);
                val /= 1.875f; // Normalize

                // Linear decay envelope: starts decaying after 30% of the duration
                const float attackTime = 0.005f; // 5ms attack
                if (t < attackTime) {
                    env = t / attackTime;
                } else {
                    const float decayStart = 0.3f * durationSec;
                    if (t < decayStart) {
                        env = 1.0f;
                    } else {
                        env = (durationSec - t) / (durationSec - decayStart);
                    }
                }
                break;
            }
        }

        float sampleVal = val * env * volume;
        sampleVal = std::max(-1.0f, std::min(sampleVal, 1.0f));
        sb.samples[i] = static_cast<int16_t>(sampleVal * 32767.0f);
    }

    // Build AIFF blob — heap-allocated, kept alive until dispose()
    sb.aiffBlob = new std::vector<uint8_t>(buildAIFFBlob(sb.samples));

    // Save to temp file
    static std::atomic<int> s_soundCounter{0};
    std::string tempPath = "/tmp/horus_audio_" + std::to_string(s_soundCounter.fetch_add(1)) + "_" + std::to_string(static_cast<int>(freqHz)) + ".aiff";
    
    std::ofstream out(tempPath, std::ios::binary);
    if (!out) {
        delete sb.aiffBlob;
        sb.aiffBlob = nullptr;
        return sb;
    }
    out.write(reinterpret_cast<const char*>(sb.aiffBlob->data()), sb.aiffBlob->size());
    out.close();

    // Create CFURLRef
    CFStringRef pathRef = CFStringCreateWithCString(kCFAllocatorDefault, tempPath.c_str(), kCFStringEncodingUTF8);
    if (!pathRef) {
        delete sb.aiffBlob;
        sb.aiffBlob = nullptr;
        std::remove(tempPath.c_str());
        return sb;
    }

    CFURLRef fileURL = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, pathRef, kCFURLPOSIXPathStyle, false);
    CFRelease(pathRef);
    if (!fileURL) {
        delete sb.aiffBlob;
        sb.aiffBlob = nullptr;
        std::remove(tempPath.c_str());
        return sb;
    }

    // Register the SystemSoundID
    OSStatus err = AudioServicesCreateSystemSoundID(fileURL, &sb.soundID);
    CFRelease(fileURL);

    if (err != noErr) {
        delete sb.aiffBlob;
        sb.aiffBlob = nullptr;
        std::remove(tempPath.c_str());
        return sb;
    }

    sb.tempFilePath = tempPath;
    sb.valid = true;
    return sb;
}

// ---------------------------------------------------------------------------
// AudioEngine public API
// ---------------------------------------------------------------------------

AudioEngine::AudioEngine()
    : m_initialised(false) {
    // Initialise last-motion timestamp far in the past so first beep fires immediately
    m_lastMotionBeep = std::chrono::steady_clock::now() - std::chrono::seconds(3600);
}

AudioEngine::~AudioEngine() {
    shutdown();
}

void AudioEngine::init(const Config& cfg) {
    m_cfg = cfg;
    applyConfig(cfg);
    m_initialised = true;
}

void AudioEngine::applyConfig(const Config& cfg) {
    m_cfg = cfg;
    // Amplitude is baked into the PCM at synthesis time; master switch zeroes
    // the volume so we regenerate all sounds.
    const float vol = cfg.masterEnabled ? cfg.masterVolume : 0.0f;

    m_motionSound.dispose();
    m_alarmEntrySound.dispose();
    m_alarmExitSound.dispose();
    m_lockAcquiredSound.dispose();
    m_lockLostSound.dispose();

    m_motionSound       = buildSound(Event::MOTION_ALERT,   cfg.motionFreqHz,       cfg.motionDurationMs,    vol);
    m_alarmEntrySound   = buildSound(Event::ALARM_ENTRY,    cfg.alarmEntryFreqHz,   cfg.alarmEntryDurMs,     vol);
    m_alarmExitSound    = buildSound(Event::ALARM_EXIT,     cfg.alarmExitFreqHz,    cfg.alarmExitDurMs,      vol);
    m_lockAcquiredSound = buildSound(Event::LOCK_ACQUIRED,  cfg.lockAcquiredFreqHz, cfg.lockAcquiredDurMs,   vol);
    m_lockLostSound     = buildSound(Event::LOCK_LOST,      cfg.lockLostFreqHz,     cfg.lockLostDurMs,       vol);
}

void AudioEngine::playEvent(Event e) {
    if (!m_cfg.masterEnabled) return;

    switch (e) {
        case Event::MOTION_ALERT:
            if (m_cfg.motionEnabled && m_motionSound.valid)
                AudioServicesPlaySystemSound(m_motionSound.soundID);
            break;
        case Event::ALARM_ENTRY:
            if (m_cfg.alarmEntryEnabled && m_alarmEntrySound.valid)
                AudioServicesPlaySystemSound(m_alarmEntrySound.soundID);
            break;
        case Event::ALARM_EXIT:
            if (m_cfg.alarmExitEnabled && m_alarmExitSound.valid)
                AudioServicesPlaySystemSound(m_alarmExitSound.soundID);
            break;
        case Event::LOCK_ACQUIRED:
            if (m_cfg.lockAcquiredEnabled && m_lockAcquiredSound.valid)
                AudioServicesPlaySystemSound(m_lockAcquiredSound.soundID);
            break;
        case Event::LOCK_LOST:
            if (m_cfg.lockLostEnabled && m_lockLostSound.valid)
                AudioServicesPlaySystemSound(m_lockLostSound.soundID);
            break;
    }
}

bool AudioEngine::motionCooldownElapsed() {
    const float elapsed = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - m_lastMotionBeep).count();
    return elapsed >= m_cfg.motionCooldownSec;
}

void AudioEngine::recordMotionBeep() {
    m_lastMotionBeep = std::chrono::steady_clock::now();
}

void AudioEngine::shutdown() {
    m_motionSound.dispose();
    m_alarmEntrySound.dispose();
    m_alarmExitSound.dispose();
    m_lockAcquiredSound.dispose();
    m_lockLostSound.dispose();
    m_initialised = false;
}
