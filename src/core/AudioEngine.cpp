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
                // Sonar Radar Tick: Crisp high-frequency tick at freqHz with steep exponential decay
                val = std::sin(2.0f * kPi * freqHz * t);
                
                // Extremely fast decay (12ms decay time scale)
                constexpr float attackTime = 0.001f; // 1ms fade-in to prevent initial pop
                if (t < attackTime) {
                    env = t / attackTime;
                } else {
                    env = std::exp(-15.0f * (t - attackTime) / durationSec);
                }
                break;
            }
            case Event::ALARM_ENTRY: {
                // Threat Warning: Rapid triple-tick warning (15ms tick pulse, 20ms gap)
                constexpr float pulseDur = 0.015f;
                constexpr float gapDur = 0.020f;
                constexpr float cycle = pulseDur + gapDur;
                
                const float local_t = std::fmod(t, cycle);
                if (local_t < pulseDur) {
                    val = std::sin(2.0f * kPi * freqHz * local_t);
                    env = std::exp(-8.0f * local_t / pulseDur);
                    if (local_t < 0.001f) env *= local_t / 0.001f;
                } else {
                    val = 0.0f;
                    env = 0.0f;
                }
                break;
            }
            case Event::ALARM_EXIT: {
                // Clearance Pop: Single low-frequency muted status pop with extremely fast decay
                val = std::sin(2.0f * kPi * freqHz * t);
                
                constexpr float attackTime = 0.002f;
                if (t < attackTime) {
                    env = t / attackTime;
                } else {
                    env = std::exp(-12.0f * (t - attackTime) / durationSec);
                }
                break;
            }
            case Event::LOCK_ACQUIRED: {
                // Lock Confirmation: High-tech double-tick ("tick-TOCK")
                // Scaled relative to durationSec so duration slider controls speed
                const float t1 = 0.010f * (durationSec / 0.045f);
                const float gap = 0.015f * (durationSec / 0.045f);
                const float t2 = 0.020f * (durationSec / 0.045f);

                constexpr float attackTime = 0.001f;

                if (t < t1) {
                    // Lower pitch first tick (20% lower than target frequency)
                    val = std::sin(2.0f * kPi * (freqHz * 0.8f) * t);
                    env = std::exp(-8.0f * t / t1);
                    if (t < attackTime) env *= t / attackTime;
                } else if (t < t1 + gap) {
                    val = 0.0f;
                    env = 0.0f;
                } else {
                    // Higher pitch second tick (12% higher than target frequency)
                    const float t_rel = t - (t1 + gap);
                    val = std::sin(2.0f * kPi * (freqHz * 1.12f) * t_rel);
                    env = std::exp(-8.0f * t_rel / t2);
                    if (t_rel < attackTime) env *= t_rel / attackTime;
                }
                break;
            }
            case Event::LOCK_LOST: {
                // Telemetry Loss: Descending double-pop failure warning
                const float t1 = 0.020f * (durationSec / 0.065f);
                const float gap = 0.025f * (durationSec / 0.065f);
                const float t2 = 0.020f * (durationSec / 0.065f);

                constexpr float attackTime = 0.001f;

                if (t < t1) {
                    // First pop (30% higher pitch warning)
                    val = std::sin(2.0f * kPi * (freqHz * 1.3f) * t);
                    env = std::exp(-6.0f * t / t1);
                    if (t < attackTime) env *= t / attackTime;
                } else if (t < t1 + gap) {
                    val = 0.0f;
                    env = 0.0f;
                } else {
                    // Second pop (base pitch)
                    const float t_rel = t - (t1 + gap);
                    val = std::sin(2.0f * kPi * freqHz * t_rel);
                    env = std::exp(-6.0f * t_rel / t2);
                    if (t_rel < attackTime) env *= t_rel / attackTime;
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
