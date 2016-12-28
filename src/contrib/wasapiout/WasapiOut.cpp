//////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2007-2016 musikcube team
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    * Redistributions of source code must retain the above copyright notice,
//      this list of conditions and the following disclaimer.
//
//    * Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//    * Neither the name of the author nor the names of other contributors may
//      be used to endorse or promote products derived from this software
//      without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
//////////////////////////////////////////////////////////////////////////////

#include "WasapiOut.h"
#include <AudioSessionTypes.h>
#include <iostream>
#include <chrono>
#include <thread>

#define MAX_BUFFERS_PER_OUTPUT 16

#define BUFFER_TO_TIME(b) \
    (UINT64) round(((UINT64) buffer->Samples() * 10000000LL) / (UINT64) buffer->SampleRate());

/* NOTE! device init and deinit logic was stolen and modified from
QMMP's WASABI output plugin! http://qmmp.ylsoftware.com/ */

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif

using Lock = std::unique_lock<std::recursive_mutex>;

WasapiOut::WasapiOut()
: enumerator(nullptr)
, device(nullptr)
, audioClient(nullptr)
, renderClient(nullptr)
, simpleAudioVolume(nullptr)
, audioStreamVolume(nullptr)
, audioClock(nullptr)
, outputBufferFrames(0)
, state(StateStopped)
, latency(0)
, volume(1.0f) {
    ZeroMemory(&waveFormat, sizeof(WAVEFORMATEXTENSIBLE));
}

WasapiOut::~WasapiOut() {
}

void WasapiOut::Destroy() {
    this->Reset();
    delete this;
}

void WasapiOut::Pause() {
    this->state = StatePaused;

    Lock lock(this->stateMutex);

    if (this->audioClient) {
        this->audioClient->Stop();
    }
}

void WasapiOut::Resume() {
    this->state = StatePlaying;

    Lock lock(this->stateMutex);

    if (this->audioClient) {
        this->audioClient->Start();
    }
}

void WasapiOut::SetVolume(double volume) {
    Lock lock(this->stateMutex);

    this->volume = volume;
    if (this->simpleAudioVolume) {
        /* master volume is always 1.0 for mixing purposes */
        simpleAudioVolume->SetMasterVolume(1.0f, 0);
        simpleAudioVolume->SetMute(false, 0);

        /* volume for this particular output's stream */
        if (this->audioStreamVolume) {
            UINT32 count = 0;
            this->audioStreamVolume->GetChannelCount(&count);

            for (UINT32 i = 0; i < count; i++) {
                this->audioStreamVolume
                    ->SetChannelVolume(i, (float) this->volume);
            }
        }
    }
}

double WasapiOut::GetVolume() {
    return this->volume;
}

void WasapiOut::Stop() {
    this->state = StateStopped;

    Lock lock(this->stateMutex);

    if (this->audioClient) {
        this->audioClient->Stop();
        this->audioClient->Reset();
        this->audioClient->Start();
    }
}

void WasapiOut::Drain() {
    int sleepMs = (int) (round(this->Latency()) * 1000.0f);

    /* not sure of a better way to ensure the final buffer is
    flushed other than to use this heuristic: given the latency
    size in seconds, sleep for 50 milliseconds at a time while
    it's still playing. */
    while (this->state != StateStopped && sleepMs > 0) {
        Sleep(50);
        if (this->state == StatePlaying) {
            sleepMs -= 50;
        }
    }
}

bool WasapiOut::Play(IBuffer *buffer, IBufferProvider *provider) {
    IAudioRenderClient *renderClient = nullptr;
    IAudioClient *audioClient = nullptr;

    /* reduce lock contention by snagging the references to the
    COM interfaces we care about and calling AddRef(), then operating
    on the local copies. */
    {
        Lock lock(this->stateMutex);

        if (this->state == StatePaused) {
            return false;
        }

        if (!this->Configure(buffer)) {
            this->Reset();
            return false;
        }

        renderClient = this->renderClient;
        renderClient->AddRef();

        audioClient = this->audioClient;
        audioClient->AddRef();
    }

    HRESULT result;
    UINT32 availableFrames = 0;
    UINT32 frameOffset = 0;
    UINT32 samples = (UINT32) buffer->Samples();
    UINT32 framesToWrite = samples / (UINT32) buffer->Channels();
    int channels = buffer->Channels();
    bool bufferWitten = false;

    do {
        audioClient->GetCurrentPadding(&frameOffset);
        availableFrames = (this->outputBufferFrames - frameOffset);

        if (availableFrames < framesToWrite) {
            UINT32 delta = framesToWrite - availableFrames;
            REFERENCE_TIME sleepTime = (delta * 1000 * 1000 * 10) / buffer->SampleRate();
            std::this_thread::sleep_for(std::chrono::microseconds(sleepTime));
        }
    } while (this->state == StatePlaying && availableFrames < framesToWrite);

    if (state == StatePlaying) {
        if (availableFrames >= framesToWrite) {
            BYTE *data = 0;

            result = this->renderClient->GetBuffer(framesToWrite, &data);
            if (result == S_OK) {
                memcpy(data, buffer->BufferPointer(), sizeof(float) * samples);
                renderClient->ReleaseBuffer(framesToWrite, 0);
                bufferWitten = true;
            }
        }
    }

    renderClient->Release();
    audioClient->Release();

    if (bufferWitten) {
        provider->OnBufferProcessed(buffer);
    }

    return bufferWitten;
}

void WasapiOut::Reset() {
    if (this->audioClient) {
        this->audioClient->Stop();
        this->audioClient->Release();
        this->audioClient = nullptr;
    }

    if (this->enumerator) {
        this->enumerator->Release();
        this->enumerator = nullptr;
    }

    if (this->device) {
        this->device->Release();
        this->device = 0;
    }

    if (this->renderClient) {
        this->renderClient->Release();
        this->renderClient = nullptr;
    }

    if (this->simpleAudioVolume) {
        this->simpleAudioVolume->Release();
        this->simpleAudioVolume = nullptr;
    }

    if (this->audioStreamVolume) {
        this->audioStreamVolume->Release();
        this->audioStreamVolume = nullptr;
    }

    if (this->audioClock) {
        this->audioClock->Release();
        this->audioClock = nullptr;
    }

    ZeroMemory(&waveFormat, sizeof(WAVEFORMATEXTENSIBLE));
}

double WasapiOut::Latency() {
    return this->latency;
}

bool WasapiOut::Configure(IBuffer *buffer) {
    HRESULT result;

    if (!this->audioClient) {
        CoInitialize(nullptr);

        result = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            NULL,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            (void**) &this->enumerator);

        if (result != S_OK) {
            std::cerr << "WasapiOut: CoCreateInstance failed, error code = " << result << "\n";
            return false;
        }

        if ((result = this->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &this->device)) != S_OK) {
            std::cerr << "WasapiOut: IMMDeviceEnumerator::GetDefaultAudioEndpoint failed, error code = " << result << "\n";
            return false;
        }

        if ((result = this->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**) &this->audioClient)) != S_OK) {
            std::cerr << "WasapiOut: IMMDevice::Activate failed, error code = " << result << "\n";
            return false;
        }
    }

    if (waveFormat.Format.nChannels == buffer->Channels() &&
        waveFormat.Format.nSamplesPerSec == buffer->SampleRate())
    {
        return true;
    }

    DWORD speakerConfig = 0;
    switch (buffer->Channels()) {
        case 1:
            speakerConfig = KSAUDIO_SPEAKER_MONO;
            break;
        case 2:
            speakerConfig = KSAUDIO_SPEAKER_STEREO;
            break;
        case 4:
            speakerConfig = KSAUDIO_SPEAKER_QUAD;
            break;
        case 5:
            speakerConfig = (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT);
            break;
        case 6:
            speakerConfig = KSAUDIO_SPEAKER_5POINT1;
            break;
    }

    WAVEFORMATEXTENSIBLE &wf = this->waveFormat;
    wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE);
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = (WORD) buffer->Channels();
    wf.Format.wBitsPerSample = 8 * sizeof(float);
    wf.Format.nSamplesPerSec = (DWORD) buffer->SampleRate();
    wf.Samples.wValidBitsPerSample = 8 * sizeof(float);
    wf.Format.nBlockAlign = (wf.Format.wBitsPerSample / 8) * wf.Format.nChannels;
    wf.Format.nAvgBytesPerSec = wf.Format.nSamplesPerSec * wf.Format.nBlockAlign;
    wf.dwChannelMask = speakerConfig;
    wf.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    DWORD streamFlags = 0;

    if (this->audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, (WAVEFORMATEX *) &wf, 0) != S_OK) {
        streamFlags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
        std::cerr << "WasapiOut: format is not supported, using converter\n";
    }

    long totalMillis = (long) round((buffer->Samples() / buffer->Channels() * 1000) / buffer->SampleRate()) * MAX_BUFFERS_PER_OUTPUT;
    REFERENCE_TIME hundredNanos = totalMillis * 1000 * 10;

    if ((result = this->audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, hundredNanos, 0, (WAVEFORMATEX *) &wf, NULL)) != S_OK) {
        std::cerr << "WasapiOut: IAudioClient::Initialize failed, error code = " << result << "\n";
        return false;
    }

    if ((result = this->audioClient->GetBufferSize(&this->outputBufferFrames)) != S_OK) {
        std::cerr << "WasapiOut: IAudioClient::GetBufferSize failed, error code = " << result << "\n";
        return false;
    }

    this->latency = (float) outputBufferFrames / (float) buffer->SampleRate();

    if ((result = this->audioClient->GetService(__uuidof(IAudioRenderClient), (void**) &this->renderClient)) != S_OK) {
        std::cerr << "WasapiOut: IAudioClient::GetService failed, error code = " << result << "\n";
        return false;
    }

    if ((result = this->audioClient->GetService(__uuidof(ISimpleAudioVolume), (void**) &this->simpleAudioVolume)) != S_OK) {
        std::cerr << "WasapiOut: IAudioClient::GetService failed, error code = " << result << "\n";
        return false;
    }

    if ((result = this->audioClient->GetService(__uuidof(IAudioStreamVolume), (void**) &this->audioStreamVolume)) != S_OK) {
        std::cerr << "WasapiOut: IAudioClient::GetService failed, error code = " << result << "\n";
        return false;
    }

    if ((result = this->audioClient->GetService(__uuidof(IAudioClock), (void**) &this->audioClock)) != S_OK) {
        std::cerr << "WasapiOut: IAudioClient::GetService failed, error code = " << result << "\n";
        return false;
    }

    if ((result = this->audioClient->Start()) != S_OK) {
        std::cerr << "WasapiOut: IAudioClient::Start failed, error code = " << result << "\n";
        return false;
    }

    this->state = StatePlaying;
    this->SetVolume(this->volume);

    return true;
}