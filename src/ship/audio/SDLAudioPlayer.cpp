#include "ship/audio/SDLAudioPlayer.h"
#include <spdlog/spdlog.h>

namespace Ship {

static std::atomic<uint64_t> sUnderruns{ 0 };

uint64_t SDLAudioPlayer::GetUnderrunCount() {
    return sUnderruns.load(std::memory_order_relaxed);
}

SDLAudioPlayer::~SDLAudioPlayer() {
    SPDLOG_TRACE("destruct SDL audio player");
    DoClose();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void SDLAudioPlayer::DoClose() {
    if (mDevice != 0) {
        // Pause playback first
        SDL_PauseAudioDevice(mDevice, 1);
        // Clear any queued audio to prevent glitches when reopening
        SDL_ClearQueuedAudio(mDevice);
        SDL_CloseAudioDevice(mDevice);
        mDevice = 0;
    }
}

bool SDLAudioPlayer::DoInit() {
#ifdef __IOS__
    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
#endif
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        SPDLOG_ERROR("SDL init error: {}", SDL_GetError());
        return false;
    }

    // Always open with the correct number of output channels
    mNumChannels = this->GetNumOutputChannels();

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = this->GetSampleRate();
    want.format = AUDIO_S16SYS;
    want.channels = mNumChannels;
    want.samples = this->GetSampleLength();
    want.callback = NULL;

    mDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (mDevice == 0) {
        SPDLOG_ERROR("SDL_OpenAudio error: {}", SDL_GetError());
        return false;
    }

    SPDLOG_INFO("SDL Audio initialized: {} channels, {} Hz", mNumChannels, this->GetSampleRate());

    SDL_PauseAudioDevice(mDevice, 0);
    return true;
}

int SDLAudioPlayer::Buffered() {
    return SDL_GetQueuedAudioSize(mDevice) / (sizeof(int16_t) * mNumChannels);
}

void SDLAudioPlayer::SetPaused(bool paused) {
    mPaused = paused;
    mStarted = false;
    if (mDevice == 0) {
        return;
    }
    SDL_PauseAudioDevice(mDevice, paused ? 1 : 0);
    if (paused) {
        SDL_ClearQueuedAudio(mDevice);
    }
}

void SDLAudioPlayer::DoPlay(const uint8_t* buf, size_t len) {
    if (mPaused) {
        return;
    }
    const int buffered = Buffered();
    if (buffered == 0 && mStarted) {
        sUnderruns.fetch_add(1, std::memory_order_relaxed);
    }
    mStarted = true;
    if (buffered < 6000) {
        // Don't fill the audio buffer too much in case this happens
        SDL_QueueAudio(mDevice, buf, len);
    }
}
} // namespace Ship
