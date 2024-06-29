#ifndef WASAPI_CAPTURE_HPP
#define WASAPI_CAPTURE_HPP

// windows.h before other headers
#include <Windows.h>
#include <atomic>
#include <Audioclient.h>
#include <RTWorkQ.h>
#include <wrl/implements.h>

class WASAPICaptureReceiver {
public:
	virtual void OnPacket(BYTE* frames, UINT32 frameCount) = 0;
};

class WASAPICapture {
public:
	WASAPICapture(WASAPICaptureReceiver* receiver, DWORD processId);
	~WASAPICapture();

	void Start();
	void Stop();

private:
	void Initialize();
	bool ProcessCaptureData();

	// helper class for Rtwq callbacks
	template<class Class, typename void(Class::*Member)(void)>
	class RtwqCallback : public IRtwqAsyncCallback {
	public:
		RtwqCallback(Class* clazz) :
			clazz { clazz },
			refCount { 1 },
			queueId { }
		{ }

		STDMETHOD_(ULONG, AddRef)() { return ++refCount; }
		STDMETHOD_(ULONG, Release)() { return --refCount; }
		STDMETHOD(QueryInterface)(REFIID riid, void** object) {
			HRESULT result = E_NOINTERFACE;

			if(riid == __uuidof(IRtwqAsyncCallback) ||
				riid == __uuidof(IUnknown)) {
				*object = this;
				AddRef();
				result = S_OK;
			} else {
				*object = nullptr;
			}

			return result;
		}
		STDMETHOD(GetParameters)(DWORD* flags, DWORD* queue) {
			*flags = 0;
			*queue = queueId;
			return S_OK;
		}

		STDMETHOD(Invoke)(IRtwqAsyncResult*) override {
			(clazz->*Member)();
			return S_OK;
		}

		DWORD GetQueueId() { return queueId; }
		void SetQueueId(DWORD queue) { queueId = queue; }

	protected:
		Class* clazz;
		std::atomic<ULONG> refCount;
		DWORD queueId;
	};

	void OnStartCapture();
	RtwqCallback<WASAPICapture, &OnStartCapture> startCaptureCallback;
	Microsoft::WRL::ComPtr<IRtwqAsyncResult> startCaptureAsyncResult;

	void OnSampleReady();
	RtwqCallback<WASAPICapture, &OnSampleReady> sampleReadyCallback;
	Microsoft::WRL::ComPtr<IRtwqAsyncResult> sampleReadyAsyncResult;

	void OnRestart();
	RtwqCallback<WASAPICapture, &OnRestart> restartCallback;
	Microsoft::WRL::ComPtr<IRtwqAsyncResult> restartAsyncResult;

private:
	WASAPICaptureReceiver* receiver;
	DWORD processId;

	HANDLE stopSignal;
	HANDLE receiveSignal;
	HANDLE restartSignal;

	Microsoft::WRL::ComPtr<IAudioClient> audioClient;
	Microsoft::WRL::ComPtr<IAudioCaptureClient> audioCaptureClient;
};

#endif // WASAPI_CAPTURE_HPP
