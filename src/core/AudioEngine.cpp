// ---------------------------------------------------------------------------
// AudioEngine.cpp — Project Horus
//
// Synthesises short sine-burst PCM samples and plays them via the platform
// audio API.  Playback is fully asynchronous and non-blocking.  All heap
// allocation happens only during init/applyConfig, never in the real-time loop.
// ---------------------------------------------------------------------------

#include "AudioEngine.hpp"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <atomic>
#include <cstdio>

#ifdef _WIN32
#  include <ole2.h>
#  pragma comment(lib, "ole32.lib")
#  pragma comment(lib, "oleaut32.lib")

static void ensureCoInit() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
}
#endif

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
#ifdef __APPLE__
    if (soundID) {
        AudioServicesRemoveSystemSoundCompletion(soundID);
        AudioServicesDisposeSystemSoundID(soundID);
        soundID = 0;
    }
    if (!tempFilePath.empty()) {
        std::remove(tempFilePath.c_str());
        tempFilePath.clear();
    }
    delete aiffBlob;
    aiffBlob = nullptr;
#elif defined(_WIN32)
    if (audioClient) {
        audioClient->Stop();
    }
    // Closing the event handle signals the drain thread's WaitForSingleObject to
    // return WAIT_FAILED, causing the thread loop to exit.
    if (eventHandle) {
        CloseHandle(eventHandle);
        eventHandle = nullptr;
    }
    if (drainThread.joinable()) {
        drainThread.join();
    }
    if (renderClient) { renderClient->Release(); renderClient = nullptr; }
    if (audioClient)  { audioClient->Release();  audioClient  = nullptr; }
#endif
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
                // Sonar Ping: Gaussian-enveloped sine — sounds clean and musical at any
                // repetition rate; much less fatiguing than a hard-edged burst.
                // The bell peaks at 30% of the duration and fades naturally.
                const float center = durationSec * 0.30f;
                const float sigma  = durationSec * 0.13f;
                val = std::sin(2.0f * kPi * freqHz * t);
                env = std::exp(-0.5f * ((t - center) / sigma) * ((t - center) / sigma));
                break;
            }
            case Event::ALARM_ENTRY: {
                // Threat Warning: Alternating two-tone alarm with added octave harmonic.
                // High tone / lower-fifth alternation every 25 ms — far more alarming
                // than single-frequency ticks, and impossible to miss.
                constexpr float halfCycle = 0.025f;
                const float phase = std::fmod(t, halfCycle * 2.0f);
                const float localFreq = (phase < halfCycle) ? freqHz : (freqHz * 0.667f);

                // Sine fundamental + octave overtone for richer alarm character
                val  = std::sin(2.0f * kPi * localFreq * t);
                val += 0.28f * std::sin(2.0f * kPi * localFreq * 2.0f * t);
                val /= 1.28f; // keep within [-1, 1]

                // Fast 5 ms attack, flat sustain, 10 ms release
                constexpr float attackTime  = 0.005f;
                const float     releaseStart = durationSec - 0.010f;
                if (t < attackTime) {
                    env = t / attackTime;
                } else if (t > releaseStart) {
                    env = (durationSec - t) / 0.010f;
                } else {
                    env = 1.0f;
                }
                break;
            }
            case Event::ALARM_EXIT: {
                // Clearance Pop: Single low-frequency muted status pop with fast decay
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
                    val = std::sin(2.0f * kPi * (freqHz * 0.8f) * t);
                    env = std::exp(-8.0f * t / t1);
                    if (t < attackTime) env *= t / attackTime;
                } else if (t < t1 + gap) {
                    val = 0.0f;
                    env = 0.0f;
                } else {
                    const float t_rel = t - (t1 + gap);
                    val = std::sin(2.0f * kPi * (freqHz * 1.12f) * t_rel);
                    env = std::exp(-8.0f * t_rel / t2);
                    if (t_rel < attackTime) env *= t_rel / attackTime;
                }
                break;
            }
            case Event::LOCK_LOST: {
                // Telemetry Loss: Descending frequency chirp — sweeps from 1.6× to 0.5×
                // freqHz so the pitch drop is unmistakable.
                const float freqStart = freqHz * 1.6f;
                const float freqEnd   = freqHz * 0.5f;

                val = std::sin(2.0f * kPi * (freqStart * t +
                      (freqEnd - freqStart) * t * t / (2.0f * durationSec)));
                val += 0.18f * std::sin(2.0f * kPi * (freqStart * 1.5f * t +
                       (freqEnd - freqStart) * 1.5f * t * t / (2.0f * durationSec)));
                val /= 1.18f;

                constexpr float attackTime = 0.002f;
                if (t < attackTime) {
                    env = t / attackTime;
                } else {
                    env = std::exp(-1.8f * t / durationSec);
                    const float releaseStart = durationSec - 0.015f;
                    if (t > releaseStart)
                        env *= (durationSec - t) / 0.015f;
                }
                break;
            }
            case Event::LOCK_PULSE: {
                // Targeting ping: crisp electronic beep with a 3rd-harmonic partial that
                // gives a slightly metallic "radar" character.  Very fast decay so rapid
                // repetition stays clean and non-fatiguing.
                val  = std::sin(2.0f * kPi * freqHz * t);
                val += 0.15f * std::sin(2.0f * kPi * freqHz * 3.0f * t);
                val /= 1.15f;

                constexpr float attackTime = 0.001f;
                if (t < attackTime) {
                    env = t / attackTime;
                } else {
                    env = std::exp(-20.0f * (t - attackTime) / durationSec);
                }
                break;
            }
            case Event::LOCK_SOLUTION: {
                // "Target ready" confirmation: ascending frequency sweep into a held tone.
                // The first 35% of the duration sweeps from 0.75× to 1.0× freqHz;
                // the remainder sustains at freqHz with a slow fade.
                const float sweepEnd = durationSec * 0.35f;
                const float f0 = freqHz * 0.75f;
                const float f1 = freqHz;

                float phase;
                if (t <= sweepEnd) {
                    // Phase-correct integration of linear frequency ramp
                    phase = 2.0f * kPi * (f0 * t + (f1 - f0) * t * t / (2.0f * sweepEnd));
                } else {
                    float phaseAtEnd = 2.0f * kPi * (f0 * sweepEnd + (f1 - f0) * sweepEnd / 2.0f);
                    phase = phaseAtEnd + 2.0f * kPi * f1 * (t - sweepEnd);
                }
                val = std::sin(phase);
                // Octave partial for a fuller, warmer tone
                val += 0.18f * std::sin(phase * 2.0f);
                val /= 1.18f;

                constexpr float attackTime = 0.008f;
                const float releaseStart   = durationSec - 0.06f;
                if (t < attackTime) {
                    env = t / attackTime;
                } else if (t > releaseStart) {
                    env = (durationSec - t) / 0.06f;
                } else {
                    env = std::exp(-0.7f * (t - attackTime) / durationSec);
                }
                break;
            }
        }

        float sampleVal = val * env * volume;
        sampleVal = std::max(-1.0f, std::min(sampleVal, 1.0f));
        sb.samples[i] = static_cast<int16_t>(sampleVal * 32767.0f);
    }

#ifdef __APPLE__
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

#elif defined(_WIN32)
    ensureCoInit();

    // Acquire default render endpoint
    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&pEnum));
    if (FAILED(hr)) return sb;

    IMMDevice* pDevice = nullptr;
    hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    pEnum->Release();
    if (FAILED(hr)) return sb;

    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                           nullptr, reinterpret_cast<void**>(&sb.audioClient));
    pDevice->Release();
    if (FAILED(hr)) return sb;

    WAVEFORMATEX wfx = {};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 1;
    wfx.nSamplesPerSec  = kSampleRate;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = 2;
    wfx.nAvgBytesPerSec = kSampleRate * 2;

    // Buffer duration = sound duration + 50 ms headroom, in 100-ns units
    const float    durationSec = static_cast<float>(numSamples) / kSampleRate;
    REFERENCE_TIME bufDur = static_cast<REFERENCE_TIME>((durationSec + 0.05f) * 10000000.0f);

    hr = sb.audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    bufDur, 0, &wfx, nullptr);
    if (FAILED(hr)) { sb.audioClient->Release(); sb.audioClient = nullptr; return sb; }

    sb.eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!sb.eventHandle) { sb.audioClient->Release(); sb.audioClient = nullptr; return sb; }

    hr = sb.audioClient->SetEventHandle(sb.eventHandle);
    if (FAILED(hr)) {
        CloseHandle(sb.eventHandle); sb.eventHandle = nullptr;
        sb.audioClient->Release();   sb.audioClient = nullptr;
        return sb;
    }

    hr = sb.audioClient->GetService(IID_PPV_ARGS(&sb.renderClient));
    if (FAILED(hr)) {
        CloseHandle(sb.eventHandle); sb.eventHandle = nullptr;
        sb.audioClient->Release();   sb.audioClient = nullptr;
        return sb;
    }

    // Pre-fill the WASAPI buffer with our PCM samples
    UINT32 bufferFrameCount = 0;
    sb.audioClient->GetBufferSize(&bufferFrameCount);

    BYTE* pData = nullptr;
    hr = sb.renderClient->GetBuffer(bufferFrameCount, &pData);
    if (FAILED(hr)) {
        sb.renderClient->Release();  sb.renderClient  = nullptr;
        CloseHandle(sb.eventHandle); sb.eventHandle   = nullptr;
        sb.audioClient->Release();   sb.audioClient   = nullptr;
        return sb;
    }

    const UINT32 copyFrames  = std::min(numSamples, bufferFrameCount);
    const UINT32 silentFrames = bufferFrameCount - copyFrames;
    std::memcpy(pData, sb.samples.data(), copyFrames * sizeof(int16_t));
    if (silentFrames > 0)
        std::memset(pData + copyFrames * sizeof(int16_t), 0, silentFrames * sizeof(int16_t));

    hr = sb.renderClient->ReleaseBuffer(bufferFrameCount,
             silentFrames == bufferFrameCount ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
    if (FAILED(hr)) {
        sb.renderClient->Release();  sb.renderClient  = nullptr;
        CloseHandle(sb.eventHandle); sb.eventHandle   = nullptr;
        sb.audioClient->Release();   sb.audioClient   = nullptr;
        return sb;
    }

    // Drain thread: waits for the buffer-consumed event, then stops and
    // re-arms the client so it is ready for the next playEvent() call.
    // All allocation happens here at init time, not in the hot path.
    sb.drainThread = std::thread([buf = &sb]() {
        ensureCoInit();
        while (true) {
            DWORD result = WaitForSingleObject(buf->eventHandle, INFINITE);
            if (result != WAIT_OBJECT_0) break;  // handle closed — exit
            if (!buf->playing.load()) continue;  // spurious wake
            buf->audioClient->Stop();
            buf->audioClient->Reset();           // rewind for next play
            // Re-fill buffer for next call — happens after playback, not during
            UINT32 bufSize = 0;
            buf->audioClient->GetBufferSize(&bufSize);
            BYTE* pRefill = nullptr;
            if (SUCCEEDED(buf->renderClient->GetBuffer(bufSize, &pRefill))) {
                const UINT32 n      = static_cast<UINT32>(buf->samples.size());
                const UINT32 copy   = std::min(n, bufSize);
                const UINT32 silent = bufSize - copy;
                std::memcpy(pRefill, buf->samples.data(), copy * sizeof(int16_t));
                if (silent > 0)
                    std::memset(pRefill + copy * sizeof(int16_t), 0, silent * sizeof(int16_t));
                buf->renderClient->ReleaseBuffer(bufSize,
                    silent == bufSize ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
            }
            buf->playing.store(false);
        }
    });

    sb.valid = true;
#endif
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
    m_lockPulseThread = std::thread(&AudioEngine::lockPulseWorker, this);
    m_initialised = true;
}

void AudioEngine::applyConfig(const Config& cfg) {
    // Pause the pulse thread during rebuild to avoid reading a half-disposed buffer
    const bool wasPulsing = m_lockPulseActive.load();
    if (wasPulsing) {
        m_lockPulseActive.store(false);
        m_lockPulseCv.notify_all();
    }

    m_cfg = cfg;
    const float vol = cfg.masterEnabled ? cfg.masterVolume : 0.0f;

    m_motionSound.dispose();
    m_alarmEntrySound.dispose();
    m_alarmExitSound.dispose();
    m_lockAcquiredSound.dispose();
    m_lockLostSound.dispose();
    m_lockPulseSound.dispose();
    m_lockSolutionSound.dispose();

    m_motionSound       = buildSound(Event::MOTION_ALERT,   cfg.motionFreqHz,            cfg.motionDurationMs,      vol);
    m_alarmEntrySound   = buildSound(Event::ALARM_ENTRY,    cfg.alarmEntryFreqHz,        cfg.alarmEntryDurMs,       vol);
    m_alarmExitSound    = buildSound(Event::ALARM_EXIT,     cfg.alarmExitFreqHz,         cfg.alarmExitDurMs,        vol);
    m_lockAcquiredSound = buildSound(Event::LOCK_ACQUIRED,  cfg.lockAcquiredFreqHz,      cfg.lockAcquiredDurMs,     vol);
    m_lockLostSound     = buildSound(Event::LOCK_LOST,      cfg.lockLostFreqHz,          cfg.lockLostDurMs,         vol);
    m_lockPulseSound    = buildSound(Event::LOCK_PULSE,     cfg.lockPulseFreqHz,         cfg.lockPulseDurMs,        vol);
    m_lockSolutionSound = buildSound(Event::LOCK_SOLUTION,  cfg.lockPulseSolutionFreqHz, cfg.lockPulseSolutionDurMs, vol);

    if (wasPulsing && cfg.lockPulseEnabled) {
        m_lockPulseActive.store(true);
        m_lockPulseCv.notify_all();
    }
}

void AudioEngine::playEvent(Event e) {
    if (!m_cfg.masterEnabled) return;

    SoundBuffer* sb = nullptr;
    switch (e) {
        case Event::MOTION_ALERT:   if (m_cfg.motionEnabled && m_motionSound.valid)             sb = &m_motionSound;       break;
        case Event::ALARM_ENTRY:    if (m_cfg.alarmEntryEnabled && m_alarmEntrySound.valid)     sb = &m_alarmEntrySound;   break;
        case Event::ALARM_EXIT:     if (m_cfg.alarmExitEnabled && m_alarmExitSound.valid)       sb = &m_alarmExitSound;    break;
        case Event::LOCK_ACQUIRED:  if (m_cfg.lockAcquiredEnabled && m_lockAcquiredSound.valid) sb = &m_lockAcquiredSound; break;
        case Event::LOCK_LOST:      if (m_cfg.lockLostEnabled && m_lockLostSound.valid)         sb = &m_lockLostSound;     break;
        case Event::LOCK_PULSE:     if (m_lockPulseSound.valid)                                 sb = &m_lockPulseSound;    break;
        case Event::LOCK_SOLUTION:  if (m_lockSolutionSound.valid)                              sb = &m_lockSolutionSound; break;
    }
    if (!sb) return;

#ifdef __APPLE__
    AudioServicesPlaySystemSound(sb->soundID);
#elif defined(_WIN32)
    // Non-allocating; buffer was pre-loaded during buildSound().
    // If already playing, skip — same fire-and-forget semantics as AudioServices.
    bool expected = false;
    if (sb->playing.compare_exchange_strong(expected, true)) {
        (void)sb->audioClient->Start();
    }
#endif
}

bool AudioEngine::motionCooldownElapsed() {
    const float elapsed = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - m_lastMotionBeep).count();
    return elapsed >= m_cfg.motionCooldownSec;
}

void AudioEngine::recordMotionBeep() {
    m_lastMotionBeep = std::chrono::steady_clock::now();
}

void AudioEngine::startLockPulse(float lockStrength) {
    if (!m_cfg.lockPulseEnabled || !m_cfg.masterEnabled) return;
    m_lockStrength.store(std::max(0.0f, std::min(lockStrength, 1.0f)));
    m_solutionFired.store(false);
    m_lockPulseActive.store(true);
    m_lockPulseCv.notify_all();
}

void AudioEngine::updateLockStrength(float lockStrength) {
    m_lockStrength.store(std::max(0.0f, std::min(lockStrength, 1.0f)));
}

void AudioEngine::stopLockPulse() {
    m_lockPulseActive.store(false);
    m_solutionFired.store(false);
    m_lockPulseCv.notify_all();
}

void AudioEngine::startLockPulseTest() {
    if (!m_cfg.masterEnabled || !m_cfg.lockPulseEnabled) return;
    if (m_testRunning.exchange(true)) return; // already running

    // Detach a ramp thread: 0 → 1 over ~3 s, hold 600 ms for solution tone, then stop.
    std::thread([this]() {
        startLockPulse(0.0f);
        constexpr int   kSteps   = 60;
        constexpr int   kTotalMs = 3000;
        constexpr int   kStepMs  = kTotalMs / kSteps;
        for (int i = 0; i <= kSteps; ++i) {
            if (m_lockPulseStop.load()) break;
            updateLockStrength(static_cast<float>(i) / kSteps);
            std::this_thread::sleep_for(std::chrono::milliseconds(kStepMs));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        stopLockPulse();
        m_testRunning.store(false);
    }).detach();
}

void AudioEngine::lockPulseWorker() {
    while (!m_lockPulseStop.load()) {
        // Wait until the pulse is started or the engine shuts down
        {
            std::unique_lock<std::mutex> lock(m_lockPulseMutex);
            m_lockPulseCv.wait(lock, [this]() {
                return m_lockPulseActive.load() || m_lockPulseStop.load();
            });
        }
        if (m_lockPulseStop.load()) break;

        // Active pulse loop
        while (m_lockPulseActive.load()) {
            const float strength = m_lockStrength.load();

            // On first crossing of the solution threshold, fire the "ready" tone once
            if (strength >= m_cfg.lockPulseSolutionThresh) {
                if (!m_solutionFired.exchange(true))
                    playEvent(Event::LOCK_SOLUTION);
            } else {
                m_solutionFired.store(false);
            }

            // Fire the regular pulse beep
            playEvent(Event::LOCK_PULSE);

            // Interval decreases as lock strength increases
            const float t01     = std::max(0.0f, std::min(strength, 1.0f));
            const float interval = m_cfg.lockPulseMaxIntervalMs -
                                   (m_cfg.lockPulseMaxIntervalMs - m_cfg.lockPulseMinIntervalMs) * t01;

            std::unique_lock<std::mutex> lock(m_lockPulseMutex);
            m_lockPulseCv.wait_for(lock,
                std::chrono::milliseconds(static_cast<int>(interval)),
                [this]() { return !m_lockPulseActive.load() || m_lockPulseStop.load(); });
        }
    }
}

void AudioEngine::shutdown() {
    m_lockPulseStop.store(true);
    m_lockPulseActive.store(false);
    m_lockPulseCv.notify_all();
    if (m_lockPulseThread.joinable()) m_lockPulseThread.join();

    m_motionSound.dispose();
    m_alarmEntrySound.dispose();
    m_alarmExitSound.dispose();
    m_lockAcquiredSound.dispose();
    m_lockLostSound.dispose();
    m_lockPulseSound.dispose();
    m_lockSolutionSound.dispose();
    m_initialised = false;
}
