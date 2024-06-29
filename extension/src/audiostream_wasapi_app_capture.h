#ifndef AUDIOSTREAM_SIMPLE_H
#define AUDIOSTREAM_SIMPLE_H

// We don't need windows.h in this plugin but many others do and it throws up on itself all the time
// So best to include it and make sure CI warns us when we use something Microsoft took for their own goals....
#define WIN32
#ifdef WIN32
#include <windows.h>
#endif

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>
#include <godot_cpp/variant/string.hpp>

// Required as per https://github.com/godotengine/godot-cpp/issues/1207
#include <godot_cpp/classes/audio_frame.hpp>

#include "wasapi_capture.hpp"
#include <mutex>

using namespace godot;

/**
 * "All audio resources require two audio based classes: AudioStream and AudioStreamPlayback.
 *  As a data container, AudioStream contains the resource and exposes itself to GDScript.
 *  AudioStream references its own internal custom AudioStreamPlayback which translates
 *  AudioStream into PCM data."
 */
class AudioStreamWasapiAppCapture : public AudioStream, public WASAPICaptureReceiver {
    GDCLASS(AudioStreamWasapiAppCapture, AudioStream)
    friend class AudioStreamPlaybackWasapiAppCapture;

private:
    // A position / phase of the signal to generate (unit: samples)
    // TODO Should this state be in Playback instead?
    uint64_t pos; 
  
    int mix_rate;

public:
    AudioStreamWasapiAppCapture();
    Ref<AudioStreamPlayback> _instantiate_playback() const override;
    String _get_stream_name() const override;

    void set_target_app_name(const String &target_app_name);

    virtual void OnPacket(BYTE* frames, UINT32 frameCount) override;

    class CircularBuffer {
    public:
        CircularBuffer(size_t bufferSize) : bufferMutex { }, bufferSize { bufferSize }, readCursor { 0 }, writeCursor { 0 } {
            buffer = new float[bufferSize];
        }

        ~CircularBuffer() {
            delete[] buffer;
            buffer = nullptr;
        }

        void Read(float* samps, size_t nSamps) {
            bufferMutex.lock();
            if(readCursor + nSamps <= bufferSize) {
                memcpy(samps, buffer + readCursor, nSamps * sizeof(float));
            } else {
                size_t copied = bufferSize - readCursor;
                memcpy(samps, buffer + readCursor, copied * sizeof(float));
                memcpy(samps + copied, buffer, (nSamps -  copied) * sizeof(float));
            }
            readCursor = (readCursor + nSamps) % bufferSize;
            bufferMutex.unlock();
        }

        void Write(float* samps, size_t nSamps) {
            bufferMutex.lock();
            if(writeCursor + nSamps <= bufferSize) {
                memcpy(buffer + writeCursor, samps, nSamps * sizeof(float));
            } else {
                size_t copied = bufferSize - writeCursor;
                memcpy(buffer + writeCursor, samps, copied * sizeof(float));
                memcpy(buffer, samps + copied, (nSamps -  copied) * sizeof(float));
            }
            writeCursor = (writeCursor + nSamps) % bufferSize;
            bufferMutex.unlock();
        }

    private:
        std::mutex bufferMutex;
        float* buffer;
        size_t bufferSize;
        size_t readCursor;
        size_t writeCursor;
    } audioBuffer;

protected:
    static void _bind_methods();

private:

    WASAPICapture* capture;
    String target_app_name;
};

class AudioStreamPlaybackWasapiAppCapture : public AudioStreamPlayback {
    GDCLASS(AudioStreamPlaybackWasapiAppCapture, AudioStreamPlayback)
    friend class AudioStreamWasapiAppCapture;

private:
    Ref<AudioStreamWasapiAppCapture> audioStream; // Keep track of the AudioStream which instantiated us
    bool active; // Are we currently playing?
    void *pcm_buffer;

public:
    AudioStreamPlaybackWasapiAppCapture();
    ~AudioStreamPlaybackWasapiAppCapture();

    /**
     * "AudioStreamPlayer uses mix callback to obtain PCM data.
     *  The callback must match sample rate and fill the buffer.
     *  Since AudioStreamPlayback is controlled by the audio thread,
     *  i/o and dynamic memory allocation are forbidden."
    */
    int32_t _mix(AudioFrame *buffer, double rate_scale, int32_t frames) override;

    bool _is_playing() const override;
    void _start(double from_pos) override;
    void _seek(double position) override;
    void _stop() override;

protected:
    static void _bind_methods();
};

#endif // AUDIOSTREAM_SIMPLE_H