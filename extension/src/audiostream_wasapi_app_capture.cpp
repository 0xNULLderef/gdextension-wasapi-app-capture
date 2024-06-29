#include "audiostream_wasapi_app_capture.h"

#include <godot_cpp/classes/audio_frame.hpp>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <Psapi.h>
#include <TlHelp32.h>
#include <Audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl/implements.h>

enum {
    // Native Godot sample rate (use AudioStreamPlaybackResampled for other values)
    MIX_RATE=44100,
    // A buffer of about 93ms (at 44100 mix rate)
    PCM_BUFFER_SIZE = 4096,
    // TODO Document this (see core implementations). Note that 4096=2^13
    MIX_FRAC_BITS = 13
};

// TODO: these misc methods should be in some class...
std::string getWindowExeName(HWND window) {
	DWORD processId;
	GetWindowThreadProcessId(window, &processId);
	if(processId == GetCurrentProcessId()) return "";

	HANDLE processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, processId);
	if(processHandle == nullptr) return "";

	char exeName[MAX_PATH];
	DWORD exeNameLength = GetProcessImageFileNameA(processHandle, exeName, MAX_PATH);
	CloseHandle(processHandle);

	if(exeNameLength == 0) return "";

	char* lastSlash = strrchr(exeName, '\\');
	if(lastSlash == nullptr) return "";

	return std::string(lastSlash + 1);
}

bool checkWindowValid(HWND window) {
	return window != nullptr &&
		IsWindowVisible(window) &&
		!(GetWindowLongPtr(window, GWL_STYLE) & WS_CHILD) &&
		!(GetWindowLongPtr(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW);
}

HWND findWindowByExeName(std::string targetExeName) {
	HWND window = FindWindowExA(GetDesktopWindow(), nullptr, nullptr, nullptr);

	while(window != nullptr) {
		if(checkWindowValid(window) && getWindowExeName(window) == targetExeName) return window;
		window = FindWindowExA(GetDesktopWindow(), window, nullptr, nullptr);
	}

	return 0;
}

AudioStreamWasapiAppCapture::AudioStreamWasapiAppCapture()
    : mix_rate(MIX_RATE), audioBuffer { 2048 } {
    auto hwnd = findWindowByExeName("Spotify.exe");
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);

    capture = new WASAPICapture(this, pid);
}

Ref<AudioStreamPlayback> AudioStreamWasapiAppCapture::_instantiate_playback() const {
    Ref<AudioStreamPlaybackWasapiAppCapture> playback;
    playback.instantiate();
    playback->audioStream = Ref<AudioStreamWasapiAppCapture>(this);
    return playback;
}

void AudioStreamWasapiAppCapture::OnPacket(BYTE* frames, UINT32 frameCount) {
    // frames are 2 floats coz stereo lol
    audioBuffer.Write(reinterpret_cast<float*>(frames), frameCount * 2);
}

String AudioStreamWasapiAppCapture::_get_stream_name() const {
    return "WASAPI App Capture: " + target_app_name;
}

void AudioStreamWasapiAppCapture::set_target_app_name(const String &target_app_name) {
    this->target_app_name = target_app_name;
}

void AudioStreamWasapiAppCapture::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_target_app_name"), &AudioStreamWasapiAppCapture::set_target_app_name);
}

#define zeromem(to, count) memset(to, 0, count)

AudioStreamPlaybackWasapiAppCapture::AudioStreamPlaybackWasapiAppCapture()
    : active(false) {
    // TODO Is locking actually required?
    AudioServer::get_singleton()->lock();
    pcm_buffer = memalloc(PCM_BUFFER_SIZE);
    zeromem(pcm_buffer, PCM_BUFFER_SIZE);

    // get buffer stufff

    AudioServer::get_singleton()->unlock();
}

AudioStreamPlaybackWasapiAppCapture::~AudioStreamPlaybackWasapiAppCapture() {
    if(pcm_buffer) {
        memfree(pcm_buffer);
        pcm_buffer = NULL;
    }
}

void AudioStreamPlaybackWasapiAppCapture::_bind_methods() {
    // Required by GDCLASS macro
}

void AudioStreamPlaybackWasapiAppCapture::_start(double from_pos) {
    active = true;
    audioStream->capture->Start();
}

void AudioStreamPlaybackWasapiAppCapture::_stop() {
    active = false;
}

void AudioStreamPlaybackWasapiAppCapture::_seek(double position) {
}

bool AudioStreamPlaybackWasapiAppCapture::_is_playing() const {
    return active;
}

int32_t AudioStreamPlaybackWasapiAppCapture::_mix(AudioFrame *buffer, double rate_scale, int32_t frames) {
    ERR_FAIL_COND_V(!active, 0);

    // TODO What is the max possible value for "frames"?
    ERR_FAIL_COND_V(frames > PCM_BUFFER_SIZE, 0);

    // // Generate 16 bits PCM samples in "buf"
    // zeromem(pcm_buffer, PCM_BUFFER_SIZE);
    // int16_t *buf = (int16_t *)pcm_buffer;

    // // Convert samples to Godot format (floats in [-1; 1])
    // for(int i = 0; i < frames; i++) {
    //     float sample = float(buf[i]) / 32767.0;
    //     buffer[i] = { sample, sample };
    // }
    audioStream->audioBuffer.Read(reinterpret_cast<float*>(buffer), frames);

    return frames;
}
