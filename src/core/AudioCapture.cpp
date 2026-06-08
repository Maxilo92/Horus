#include "AudioCapture.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>

#ifdef __APPLE__
#  include <CoreAudio/CoreAudio.h>
#elif defined(_WIN32)
#  include <functiondiscoverykeys_devpkey.h>
#  include <ole2.h>
#  pragma comment(lib, "ole32.lib")

// File-scope endpoint ID registry; index matches uint32_t device IDs returned
// by listDevices() and stored in SystemSettings::audioCaptureDeviceId.
static std::vector<std::wstring> s_endpointIds;
#endif

AudioCapture::AudioCapture() {}

AudioCapture::~AudioCapture() {
    stop();
}

std::vector<AudioDevice> AudioCapture::listDevices() {
    std::vector<AudioDevice> devices;

#ifdef __APPLE__
    AudioObjectPropertyAddress address = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size);
    if (status != noErr) return devices;

    int count = size / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> ids(count);
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, ids.data());
    if (status != noErr) return devices;

    for (auto id : ids) {
        address.mSelector = kAudioDevicePropertyStreams;
        address.mScope = kAudioDevicePropertyScopeInput;
        size = 0;
        status = AudioObjectGetPropertyDataSize(id, &address, 0, nullptr, &size);
        if (status != noErr || size == 0) continue;

        address.mSelector = kAudioDevicePropertyDeviceNameCFString;
        address.mScope = kAudioObjectPropertyScopeGlobal;
        CFStringRef nameRef = nullptr;
        size = sizeof(CFStringRef);
        status = AudioObjectGetPropertyData(id, &address, 0, nullptr, &size, &nameRef);

        if (status == noErr && nameRef) {
            char buf[256];
            if (CFStringGetCString(nameRef, buf, sizeof(buf), kCFStringEncodingUTF8)) {
                devices.push_back({static_cast<uint32_t>(id), std::string(buf)});
            }
            CFRelease(nameRef);
        }
    }

#elif defined(_WIN32)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    s_endpointIds.clear();

    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&pEnum));
    if (FAILED(hr)) return devices;

    IMMDeviceCollection* pColl = nullptr;
    hr = pEnum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pColl);
    pEnum->Release();
    if (FAILED(hr)) return devices;

    UINT count = 0;
    pColl->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
        IMMDevice* pDev = nullptr;
        if (FAILED(pColl->Item(i, &pDev))) continue;

        LPWSTR pwszId = nullptr;
        if (FAILED(pDev->GetId(&pwszId))) { pDev->Release(); continue; }
        s_endpointIds.push_back(std::wstring(pwszId));
        CoTaskMemFree(pwszId);

        IPropertyStore* pProps = nullptr;
        std::string friendlyName = "Device " + std::to_string(i);
        if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &pv))
                && pv.vt == VT_LPWSTR && pv.pwszVal) {
                int len = WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1,
                                             nullptr, 0, nullptr, nullptr);
                if (len > 0) {
                    std::vector<char> buf(len);
                    WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1,
                                        buf.data(), len, nullptr, nullptr);
                    friendlyName = std::string(buf.data());
                }
            }
            PropVariantClear(&pv);
            pProps->Release();
        }
        pDev->Release();

        devices.push_back({static_cast<uint32_t>(i), friendlyName});
    }
    pColl->Release();
#endif

    return devices;
}

bool AudioCapture::start(uint32_t deviceIndex, std::function<void(float)> intensityCallback) {
    stop();
    m_callback = intensityCallback;

#ifdef __APPLE__
    AudioDeviceID deviceID = static_cast<AudioDeviceID>(deviceIndex);

    AudioStreamBasicDescription format;
    format.mSampleRate       = 44100.0;
    format.mFormatID         = kAudioFormatLinearPCM;
    format.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    format.mBitsPerChannel   = 16;
    format.mChannelsPerFrame = 1;
    format.mFramesPerPacket  = 1;
    format.mBytesPerFrame    = 2;
    format.mBytesPerPacket   = 2;
    format.mReserved         = 0;

    OSStatus status = AudioQueueNewInput(&format, audioQueueCallback, this, nullptr, nullptr, 0, &m_queue);
    if (status != noErr) return false;

    CFStringRef uid = nullptr;
    AudioObjectPropertyAddress address = {
        kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = sizeof(CFStringRef);
    AudioObjectGetPropertyData(deviceID, &address, 0, nullptr, &size, &uid);
    if (uid) {
        AudioQueueSetProperty(m_queue, kAudioQueueProperty_CurrentDevice, &uid, sizeof(CFStringRef));
        CFRelease(uid);
    }

    const int bufferSize = 2048;
    m_buffers.resize(3);
    for (int i = 0; i < 3; ++i) {
        AudioQueueAllocateBuffer(m_queue, bufferSize * 2, &m_buffers[i]);
        AudioQueueEnqueueBuffer(m_queue, m_buffers[i], 0, nullptr);
    }

    status = AudioQueueStart(m_queue, nullptr);
    if (status == noErr) {
        m_active = true;
        return true;
    }
    return false;

#elif defined(_WIN32)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (deviceIndex >= static_cast<uint32_t>(s_endpointIds.size())) return false;

    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&pEnum));
    if (FAILED(hr)) return false;

    IMMDevice* pDevice = nullptr;
    hr = pEnum->GetDevice(s_endpointIds[deviceIndex].c_str(), &pDevice);
    pEnum->Release();
    if (FAILED(hr)) return false;

    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                           nullptr, reinterpret_cast<void**>(&m_audioClient));
    pDevice->Release();
    if (FAILED(hr)) return false;

    WAVEFORMATEX wfx = {};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 1;
    wfx.nSamplesPerSec  = 44100;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = 2;
    wfx.nAvgBytesPerSec = 44100 * 2;

    REFERENCE_TIME bufDur = 1000000LL; // 100 ms in 100-ns units
    hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufDur, 0, &wfx, nullptr);
    if (FAILED(hr)) { m_audioClient->Release(); m_audioClient = nullptr; return false; }

    hr = m_audioClient->GetService(IID_PPV_ARGS(&m_captureClient));
    if (FAILED(hr)) { m_audioClient->Release(); m_audioClient = nullptr; return false; }

    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual-reset
    if (!m_stopEvent) {
        m_captureClient->Release(); m_captureClient = nullptr;
        m_audioClient->Release();   m_audioClient   = nullptr;
        return false;
    }

    hr = m_audioClient->Start();
    if (FAILED(hr)) {
        CloseHandle(m_stopEvent); m_stopEvent = nullptr;
        m_captureClient->Release(); m_captureClient = nullptr;
        m_audioClient->Release();   m_audioClient   = nullptr;
        return false;
    }

    m_active = true;
    m_captureThread = std::thread(&AudioCapture::captureWorker, this);
    return true;
#else
    (void)deviceIndex;
    return false;
#endif
}

void AudioCapture::stop() {
#ifdef __APPLE__
    if (m_queue) {
        AudioQueueStop(m_queue, true);
        AudioQueueDispose(m_queue, true);
        m_queue = nullptr;
    }
    m_buffers.clear();
#elif defined(_WIN32)
    m_active = false;
    if (m_stopEvent) SetEvent(m_stopEvent);
    if (m_captureThread.joinable()) m_captureThread.join();
    if (m_audioClient)   { m_audioClient->Stop(); }
    if (m_captureClient) { m_captureClient->Release(); m_captureClient = nullptr; }
    if (m_audioClient)   { m_audioClient->Release();   m_audioClient   = nullptr; }
    if (m_stopEvent)     { CloseHandle(m_stopEvent);   m_stopEvent     = nullptr; }
#endif
    m_active = false;
}

#ifdef __APPLE__
void AudioCapture::audioQueueCallback(void* userData, AudioQueueRef queue, AudioQueueBufferRef buffer,
                                       const AudioTimeStamp* /*startTime*/, UInt32 /*numPackets*/,
                                       const AudioStreamPacketDescription* /*packetDesc*/) {
    auto self = static_cast<AudioCapture*>(userData);
    if (!self->m_active) return;

    const int16_t* samples = static_cast<const int16_t*>(buffer->mAudioData);
    int count = buffer->mAudioDataByteSize / 2;

    if (count > 0) {
        float sumSq = 0;
        for (int i = 0; i < count; ++i) {
            float s = samples[i] / 32768.0f;
            sumSq += s * s;
        }
        float rms = std::sqrt(sumSq / count);
        float intensity = std::min(1.0f, rms * 10.0f);
        if (self->m_callback) self->m_callback(intensity);
    }

    AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
}
#endif

#ifdef _WIN32
void AudioCapture::captureWorker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (m_active.load()) {
        if (WaitForSingleObject(m_stopEvent, 10) == WAIT_OBJECT_0) break;

        UINT32 packetSize = 0;
        HRESULT hr = m_captureClient->GetNextPacketSize(&packetSize);
        if (FAILED(hr)) break;

        while (packetSize > 0) {
            BYTE*  pData     = nullptr;
            UINT32 numFrames = 0;
            DWORD  flags     = 0;

            hr = m_captureClient->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && numFrames > 0) {
                const int16_t* samples = reinterpret_cast<const int16_t*>(pData);
                float sumSq = 0.0f;
                for (UINT32 i = 0; i < numFrames; ++i) {
                    float s = samples[i] / 32768.0f;
                    sumSq += s * s;
                }
                float rms       = std::sqrt(sumSq / static_cast<float>(numFrames));
                float intensity = std::min(1.0f, rms * 10.0f);
                if (m_callback) m_callback(intensity);
            }

            m_captureClient->ReleaseBuffer(numFrames);
            m_captureClient->GetNextPacketSize(&packetSize);
        }
    }

    CoUninitialize();
}
#endif
