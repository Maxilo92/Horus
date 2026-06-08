#ifndef AUDIO_CAPTURE_HPP
#define AUDIO_CAPTURE_HPP

#ifdef __APPLE__
#  include <AudioToolbox/AudioToolbox.h>
#elif defined(_WIN32)
#  include <windows.h>
#  include <mmdeviceapi.h>
#  include <audioclient.h>
#endif
#include <vector>
#include <string>
#include <atomic>
#include <functional>
#include <thread>

struct AudioDevice {
    uint32_t    id;   // index into enumeration result on all platforms
    std::string name;
};

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    static std::vector<AudioDevice> listDevices();

    bool start(uint32_t deviceIndex, std::function<void(float)> intensityCallback);
    void stop();

    bool isActive() const { return m_active; }

private:
#ifdef __APPLE__
    static void audioQueueCallback(void* userData, AudioQueueRef queue, AudioQueueBufferRef buffer,
                                   const AudioTimeStamp* startTime, UInt32 numPackets,
                                   const AudioStreamPacketDescription* packetDesc);

    AudioQueueRef m_queue = nullptr;
    std::vector<AudioQueueBufferRef> m_buffers;
#elif defined(_WIN32)
    void captureWorker();
    IAudioClient*        m_audioClient   = nullptr;
    IAudioCaptureClient* m_captureClient = nullptr;
    std::thread          m_captureThread;
    HANDLE               m_stopEvent     = nullptr;
#endif

    std::atomic<bool>             m_active{false};
    std::function<void(float)>    m_callback;
};

#endif // AUDIO_CAPTURE_HPP
