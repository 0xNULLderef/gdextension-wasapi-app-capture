#include "wasapi_capture.hpp"

#include <stdexcept>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>

class WASAPIActivateAudioInterfaceCompletionHandler : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
	WASAPIActivateAudioInterfaceCompletionHandler() :
		activatedInteraface { nullptr },
		activationResult { E_NOINTERFACE }
	{
		activationSignal = CreateEvent(nullptr, false, false, nullptr);
	}

	~WASAPIActivateAudioInterfaceCompletionHandler() {
		CloseHandle(activationSignal);
	}

	virtual HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* activateOperation) override {
		HRESULT innerActivationResult { };
		HRESULT innerRetrieveResult = activateOperation->GetActivateResult(&innerActivationResult, &activatedInteraface);
		activationResult = SUCCEEDED(innerRetrieveResult) ? innerActivationResult : innerRetrieveResult;

		SetEvent(activationSignal);
		return activationResult;
	}

	HRESULT GetActivateResult(IAudioClient** client) {
		WaitForSingleObject(activationSignal, INFINITE);
		*client = static_cast<IAudioClient*>(activatedInteraface);
		return activationResult;
	}

private:
	IUnknown* activatedInteraface;
	HRESULT activationResult;
	HANDLE activationSignal;
};

WASAPICapture::WASAPICapture(WASAPICaptureReceiver* receiver, DWORD processId) :
	receiver { receiver },
	processId { processId },
	startCaptureCallback { this },
	sampleReadyCallback { this },
	restartCallback { this },
	stopSignal { INVALID_HANDLE_VALUE },
	receiveSignal { INVALID_HANDLE_VALUE },
	restartSignal { INVALID_HANDLE_VALUE },
	audioClient { },
	audioCaptureClient { }
{
	HRESULT result;

	stopSignal = CreateEvent(nullptr, true, false, nullptr);
	if(stopSignal == INVALID_HANDLE_VALUE) throw std::runtime_error("failed to create stopSignal event");

	receiveSignal = CreateEvent(nullptr, false, false, nullptr);
	if(receiveSignal == INVALID_HANDLE_VALUE) throw std::runtime_error("failed to create receiveSignal event");

	restartSignal = CreateEvent(nullptr, true, false, nullptr);
	if(restartSignal == INVALID_HANDLE_VALUE) throw std::runtime_error("failed to create restartSignal event");

	// old man crackhead compiler yells at nullptr
	// MSDN says it's optional so i do not care
#pragma warning(disable:6387)
	result = RtwqCreateAsyncResult(nullptr, &startCaptureCallback, nullptr, &startCaptureAsyncResult);
	if(FAILED(result)) throw std::runtime_error("failed to create startCaptureAsyncResult");

	result = RtwqCreateAsyncResult(nullptr, &sampleReadyCallback, nullptr, &sampleReadyAsyncResult);
	if(FAILED(result)) throw std::runtime_error("failed to create sampleReadyAsyncResult");

	result = RtwqCreateAsyncResult(nullptr, &restartCallback, nullptr, &restartAsyncResult);
	if(FAILED(result)) throw std::runtime_error("failed to create restartAsyncResult");
#pragma warning(default:6387)

	DWORD taskId { };
	DWORD queueId { };
	result = RtwqLockSharedWorkQueue(L"Capture", 0, &taskId, &queueId);
	if(FAILED(result)) throw std::runtime_error("failed to LockSharedWorkQueue");

	startCaptureCallback.SetQueueId(queueId);
	sampleReadyCallback.SetQueueId(queueId);
	restartCallback.SetQueueId(queueId);
}

WASAPICapture::~WASAPICapture() {
	Stop();

	CloseHandle(stopSignal);
	CloseHandle(receiveSignal);
	CloseHandle(restartSignal);
}

void WASAPICapture::Start() {
	RtwqPutWorkItem(startCaptureCallback.GetQueueId(), 0, startCaptureAsyncResult.Get());
}

void WASAPICapture::Stop() {
	SetEvent(stopSignal);
	SetEvent(receiveSignal);

	RtwqUnlockWorkQueue(sampleReadyCallback.GetQueueId());
}

void WASAPICapture::Initialize() {
	HRESULT result;

	ResetEvent(receiveSignal);

	const WORD channelCount = 2;
	const DWORD samplesPerSecond = 44100; // godot internal
	constexpr WORD bitsPerSample = 32; // float bits
	const WORD blockAlign = channelCount * bitsPerSample / 8;

	WAVEFORMATEXTENSIBLE waveFormatExtensible { };
	waveFormatExtensible.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	waveFormatExtensible.Format.nChannels = channelCount;
	waveFormatExtensible.Format.nSamplesPerSec = samplesPerSecond;
	waveFormatExtensible.Format.nAvgBytesPerSec = samplesPerSecond * blockAlign;
	waveFormatExtensible.Format.nBlockAlign = blockAlign;
	waveFormatExtensible.Format.wBitsPerSample = bitsPerSample;
	waveFormatExtensible.Format.cbSize = sizeof(waveFormatExtensible) - sizeof(waveFormatExtensible.Format);
	waveFormatExtensible.Samples.wValidBitsPerSample = bitsPerSample;
	waveFormatExtensible.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
	waveFormatExtensible.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

	AUDIOCLIENT_ACTIVATION_PARAMS audioClientActivationParams { };
	audioClientActivationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
	audioClientActivationParams.ProcessLoopbackParams.TargetProcessId = processId;
	audioClientActivationParams.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

	PROPVARIANT activateParams { };
	activateParams.vt = VT_BLOB;
	activateParams.blob.cbSize = sizeof(audioClientActivationParams);
	activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&audioClientActivationParams);

	auto completionHandler = Microsoft::WRL::Make<WASAPIActivateAudioInterfaceCompletionHandler>();
	Microsoft::WRL::ComPtr<IActivateAudioInterfaceAsyncOperation> activationOperation { };
	result = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activateParams, completionHandler.Get(), &activationOperation);
	if(FAILED(result)) throw std::runtime_error("failed to activate audio interface");

	Microsoft::WRL::ComPtr<IAudioClient> tempAudioClient { };
	result = completionHandler->GetActivateResult(&tempAudioClient);
	if(FAILED(result)) throw std::runtime_error("failed to get activation result");

	result = tempAudioClient->Initialize(
		AUDCLNT_SHAREMODE_SHARED,
		AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_LOOPBACK,
		5 * 10000000,
		0,
		&waveFormatExtensible.Format,
		nullptr
	);
	if(FAILED(result)) throw std::runtime_error("failed to initialize audioClient");

	Microsoft::WRL::ComPtr<IAudioCaptureClient> tempAudioCaptureClient { };
	result = tempAudioClient->GetService(__uuidof(IAudioCaptureClient), &tempAudioCaptureClient);
	if(FAILED(result)) throw std::runtime_error("failed to get audioCaptureClient service");

	result = tempAudioClient->SetEventHandle(receiveSignal);
	if(FAILED(result)) throw std::runtime_error("failed to set event handle");

	result = tempAudioClient->Start();
	if(FAILED(result)) throw std::runtime_error("failed to start audio client");

	audioClient = std::move(tempAudioClient);
	audioCaptureClient = std::move(tempAudioCaptureClient);

	result = RtwqPutWaitingWorkItem(receiveSignal, 0, sampleReadyAsyncResult.Get(), nullptr);
	if(FAILED(result)) {
		audioClient.Reset();
		audioCaptureClient.Reset();
		throw std::runtime_error("failed to PutWaitingWorkItem sampleReady");
	}

	result = RtwqPutWaitingWorkItem(restartSignal, 0, restartAsyncResult.Get(), nullptr);
	if(FAILED(result)) {
		audioClient.Reset();
		audioCaptureClient.Reset();
		throw std::runtime_error("failed to PutWaitingWorkItem restart");
	}
}

bool WASAPICapture::ProcessCaptureData() {
	HRESULT result;

	while(true) {
		UINT32 packetSize { };
		result = audioCaptureClient->GetNextPacketSize(&packetSize);
		if(FAILED(result)) return false;
		if(packetSize == 0) break;

		BYTE* frames { };
		UINT32 frameCount { };
		DWORD frameFlags { };
		result = audioCaptureClient->GetBuffer(&frames, &frameCount, &frameFlags, nullptr, nullptr);
		if(FAILED(result)) return false;

		receiver->OnPacket(frames, frameCount);

		result = audioCaptureClient->ReleaseBuffer(frameCount);
		if(FAILED(result)) return false;
	}

	return true;
}

void WASAPICapture::OnStartCapture() {
	const DWORD waitStopSignal = WaitForSingleObject(stopSignal, 0);
	if(waitStopSignal == WAIT_OBJECT_0) {
		return;
	}

	try {
		Initialize();
	} catch(const std::exception& ex) {
		fprintf(stderr, "%s\n", ex.what());
	}
}

void WASAPICapture::OnSampleReady() {
	bool stop = false;

	if(!ProcessCaptureData()) {
		stop = true;
	}

	if(WaitForSingleObject(stopSignal, 0) == WAIT_OBJECT_0) {
		stop = true;
	}

	if(!stop) {
		RtwqPutWaitingWorkItem(receiveSignal, 0, sampleReadyAsyncResult.Get(), nullptr);
	} else {
		audioClient->Stop();
		audioCaptureClient.Reset();
		audioClient.Reset();
	}
}

void WASAPICapture::OnRestart() {

}
