// Skateboard sounds and SK8-FM's music, played with XAudio2 beside the
// game's own audio. WAV clips play once or loop; songs (.m4a) stream from a
// background thread through Windows' Media Foundation. Both are loaded at
// runtime: a PC without them just stays quiet.
#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

class Sound {
public:
    // Starts XAudio2 (2.9, else 2.8). Returns an empty string, or why not.
    std::string Start() {
        for (const wchar_t* name : {L"xaudio2_9.dll", L"xaudio2_8.dll"}) {
            dll_ = LoadLibraryW(name);
            if (dll_) break;
        }
        if (!dll_) return "XAudio2 is not available";
        using CreateFn = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
        auto create = reinterpret_cast<CreateFn>(GetProcAddress(dll_, "XAudio2Create"));
        if (!create || FAILED(create(&xa_, 0, XAUDIO2_DEFAULT_PROCESSOR))) return "XAudio2Create failed";
        if (FAILED(xa_->CreateMasteringVoice(&master_))) return "no audio output device";
        if (FAILED(xa_->CreateSubmixVoice(&sfx_, 2, 48000)) || FAILED(xa_->CreateSubmixVoice(&music_, 2, 48000))) {
            return "could not create the mix";
        }
        return {};
    }

    bool Ready() const { return music_ != nullptr; }

    // A 16-bit PCM WAV; returns its clip number, or -1.
    int Load(const std::wstring& path) {
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
        if (d.size() < 44 || memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "WAVE", 4)) return -1;
        Clip clip{};
        bool fmt = false;
        for (size_t p = 12; p + 8 <= d.size();) {
            uint32_t size;
            memcpy(&size, &d[p + 4], 4);
            if (!memcmp(&d[p], "fmt ", 4) && size >= 16) {
                memcpy(&clip.format, &d[p + 8], 16);
                clip.format.cbSize = 0;
                fmt = clip.format.wFormatTag == WAVE_FORMAT_PCM && clip.format.wBitsPerSample == 16;
            } else if (!memcmp(&d[p], "data", 4) && fmt) {
                size = std::min<uint32_t>(size, static_cast<uint32_t>(d.size() - p - 8));
                clip.data.assign(d.begin() + p + 8, d.begin() + p + 8 + size);
                break;
            }
            p += 8 + size + (size & 1);
        }
        if (clip.data.empty()) return -1;
        clips_.push_back(std::move(clip));
        return static_cast<int>(clips_.size()) - 1;
    }

    // Plays a clip once. volume 0..1, pitch as a frequency ratio.
    void Play(int clip, float volume, float pitch = 1.f) {
        if (!sfx_ || clip < 0 || clip >= static_cast<int>(clips_.size()) || volume <= 0.001f) return;
        IXAudio2SourceVoice* v = Voice(clips_[clip], false);
        if (!v) return;
        v->SetVolume(volume);
        v->SetFrequencyRatio(std::clamp(pitch, 0.25f, 2.f));
        v->Start();
        oneShots_.push_back(v);
    }

    // A looping voice for a clip, silent until SetLoop gives it volume.
    int AddLoop(int clip) {
        if (!sfx_ || clip < 0 || clip >= static_cast<int>(clips_.size())) return -1;
        IXAudio2SourceVoice* v = Voice(clips_[clip], true);
        if (!v) return -1;
        v->SetVolume(0.f);
        loops_.push_back({v, false});
        return static_cast<int>(loops_.size()) - 1;
    }

    // A loop runs while it has volume and stops (rewound) when it has none.
    void SetLoop(int loop, float volume, float pitch) {
        if (loop < 0 || loop >= static_cast<int>(loops_.size())) return;
        Loop& l = loops_[loop];
        if (volume > 0.002f) {
            l.voice->SetVolume(volume);
            l.voice->SetFrequencyRatio(std::clamp(pitch, 0.25f, 2.f));
            if (!l.running) l.voice->Start();
            l.running = true;
        } else if (l.running) {
            l.voice->SetVolume(0.f);
            l.voice->Stop();
            l.running = false;
        }
    }

    void SetVolumes(float sfx, float music) {
        if (sfx_) sfx_->SetVolume(sfx);
        if (music_) music_->SetVolume(music);
    }

    // The whole mix holds still while the game is paused.
    void SetPaused(bool paused) {
        if (!xa_ || paused == paused_) return;
        paused_ = paused;
        if (paused) xa_->StopEngine();
        else xa_->StartEngine();
    }

    // Frees the voices of one-shots that have finished.
    void Update() {
        for (size_t i = 0; i < oneShots_.size();) {
            XAUDIO2_VOICE_STATE s{};
            oneShots_[i]->GetState(&s, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (s.BuffersQueued == 0) {
                oneShots_[i]->DestroyVoice();
                oneShots_[i] = oneShots_.back();
                oneShots_.pop_back();
            } else {
                i++;
            }
        }
    }

    IXAudio2* Engine() { return xa_; }
    IXAudio2SubmixVoice* MusicBus() { return music_; }

private:
    struct Clip {
        WAVEFORMATEX format;
        std::vector<uint8_t> data;
    };
    struct Loop {
        IXAudio2SourceVoice* voice;
        bool running;
    };

    IXAudio2SourceVoice* Voice(const Clip& clip, bool loop) {
        XAUDIO2_SEND_DESCRIPTOR send{0, sfx_};
        XAUDIO2_VOICE_SENDS sends{1, &send};
        IXAudio2SourceVoice* v = nullptr;
        if (FAILED(xa_->CreateSourceVoice(&v, &clip.format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr, &sends))) return nullptr;
        XAUDIO2_BUFFER b{};
        b.Flags = XAUDIO2_END_OF_STREAM;
        b.AudioBytes = static_cast<UINT32>(clip.data.size());
        b.pAudioData = clip.data.data();
        if (loop) b.LoopCount = XAUDIO2_LOOP_INFINITE;
        if (FAILED(v->SubmitSourceBuffer(&b))) {
            v->DestroyVoice();
            return nullptr;
        }
        return v;
    }

    HMODULE dll_ = nullptr;
    IXAudio2* xa_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SubmixVoice *sfx_ = nullptr, *music_ = nullptr;
    std::vector<Clip> clips_;
    std::vector<IXAudio2SourceVoice*> oneShots_;
    std::vector<Loop> loops_;
    bool paused_ = false;
};


// A radio station of songs that keeps time like GTA's own stations: the
// first tuning-in starts a random song at a random point, as if it had been
// playing all along; tuning away and back finds it where it would be now,
// the same song further on (or the songs after it). Songs play in a
// shuffled order. Decoding runs on its own thread.
class Station {
public:
    ~Station() { Stop(0); }

    // Media Foundation, loaded at runtime. Returns an empty string, or why not.
    static std::string Load() {
        HMODULE plat = LoadLibraryW(L"mfplat.dll"), rw = LoadLibraryW(L"mfreadwrite.dll");
        if (!plat || !rw) return "Windows Media Foundation is not installed";
        mf_.startup = reinterpret_cast<decltype(mf_.startup)>(GetProcAddress(plat, "MFStartup"));
        mf_.mediaType = reinterpret_cast<decltype(mf_.mediaType)>(GetProcAddress(plat, "MFCreateMediaType"));
        mf_.reader = reinterpret_cast<decltype(mf_.reader)>(GetProcAddress(rw, "MFCreateSourceReaderFromURL"));
        if (!mf_.startup || !mf_.mediaType || !mf_.reader || FAILED(mf_.startup(MF_VERSION, MFSTARTUP_LITE))) {
            mf_ = {};
            return "Media Foundation could not start";
        }
        return {};
    }

    // Tunes in. `nowMs` is game time: how far the station has moved on
    // since it was last tuned away from.
    void Start(Sound& sound, const std::vector<std::wstring>& songs, uint32_t nowMs) {
        Stop(nowMs);
        if (!mf_.reader || songs.empty() || !sound.Ready()) return;
        WAVEFORMATEX f{WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0};
        XAUDIO2_SEND_DESCRIPTOR send{0, sound.MusicBus()};
        XAUDIO2_VOICE_SENDS sends{1, &send};
        if (FAILED(sound.Engine()->CreateSourceVoice(&voice_, &f, 0, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr, &sends))) {
            voice_ = nullptr;
            return;
        }
        if (songs != songs_ || order_.empty()) { // a new list: start afresh
            songs_ = songs;
            order_.resize(songs_.size());
            for (size_t i = 0; i < order_.size(); i++) order_[i] = static_cast<int>(i);
            std::shuffle(order_.begin(), order_.end(), rng_);
            lengths_.assign(songs_.size(), 0);
            at_ = 0;
            positionMs_ = -1; // a random point
        } else if (positionMs_ >= 0) {
            positionMs_ += nowMs >= stoppedAtMs_ ? nowMs - stoppedAtMs_ : 0;
        }
        stop_ = false;
        voice_->Start();
        thread_ = std::thread([this] { Run(); });
    }

    // Tunes away, remembering where the station was at game time `nowMs`.
    void Stop(uint32_t nowMs) {
        if (!voice_) return;
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        XAUDIO2_VOICE_STATE state{};
        voice_->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        // What was decoded but not yet heard is still to come.
        positionMs_ = std::max<int64_t>(0, decodedMs_ - static_cast<int64_t>(state.BuffersQueued) * kChunkMs);
        stoppedAtMs_ = nowMs;
        voice_->DestroyVoice(); // before its buffers go
        voice_ = nullptr;
    }

    bool Playing() const { return voice_ != nullptr; }
    int Song() const { return song_; } // index into the list, -1 between songs
    int64_t Position() const { return decodedMs_; } // into the song, ms (a little ahead of what's heard)

private:
    static constexpr int64_t kChunkMs = 200;
    static constexpr size_t kChunk = 44100 * 4 * kChunkMs / 1000; // 16-bit stereo
    static constexpr int kBuffers = 4;

    struct Mf {
        HRESULT(WINAPI* startup)(ULONG, DWORD) = nullptr;
        HRESULT(WINAPI* mediaType)(IMFMediaType**) = nullptr;
        HRESULT(WINAPI* reader)(LPCWSTR, IMFAttributes*, IMFSourceReader**) = nullptr;
    };
    static inline Mf mf_;

    // Opens a song for 16-bit stereo PCM and notes its length (ms).
    IMFSourceReader* Open(int song) {
        IMFSourceReader* reader = nullptr;
        IMFMediaType* pcm = nullptr;
        if (FAILED(mf_.reader(songs_[song].c_str(), nullptr, &reader))) return nullptr;
        bool ok = SUCCEEDED(mf_.mediaType(&pcm));
        if (ok) {
            pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
            pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
            pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 44100 * 4);
            ok = SUCCEEDED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, pcm));
            pcm->Release();
        }
        if (!ok) {
            reader->Release();
            return nullptr;
        }
        PROPVARIANT length;
        PropVariantInit(&length);
        if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &length)) &&
            length.vt == VT_UI8) {
            lengths_[song] = static_cast<int64_t>(length.uhVal.QuadPart / 10000);
        }
        PropVariantClear(&length);
        return reader;
    }

    void Next() {
        if (++at_ >= order_.size()) {
            std::shuffle(order_.begin(), order_.end(), rng_);
            at_ = 0;
        }
    }

    // Opens the song the station is on now and seeks to its point in it,
    // stepping past songs that would have ended meanwhile.
    IMFSourceReader* Tune() {
        for (size_t tries = 0; tries < order_.size() * 2 && !stop_; tries++) {
            int song = order_[at_];
            IMFSourceReader* reader = Open(song);
            if (!reader) {
                Next();
                positionMs_ = 0;
                continue;
            }
            int64_t length = lengths_[song];
            if (positionMs_ < 0) positionMs_ = static_cast<int64_t>(std::uniform_real_distribution<double>(0.05, 0.75)(rng_) * length);
            if (length > 0 && positionMs_ >= length) {
                positionMs_ -= length;
                reader->Release();
                Next();
                continue;
            }
            if (positionMs_ > 0) {
                PROPVARIANT at;
                PropVariantInit(&at);
                at.vt = VT_I8;
                at.hVal.QuadPart = positionMs_ * 10000;
                reader->SetCurrentPosition(GUID_NULL, at);
            }
            decodedMs_ = positionMs_;
            song_ = song;
            return reader;
        }
        return nullptr;
    }

    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (auto& b : buffers_) b.reserve(kChunk + 65536);
        int fill = 0;
        IMFSourceReader* reader = Tune();
        while (!stop_ && reader) {
            XAUDIO2_VOICE_STATE state{};
            voice_->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (state.BuffersQueued >= kBuffers - 1) {
                Sleep(10);
                continue;
            }
            std::vector<uint8_t>& buffer = buffers_[fill];
            buffer.clear();
            bool ended = false;
            while (buffer.size() < kChunk && !stop_) {
                DWORD flags = 0;
                IMFSample* sample = nullptr;
                if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, &sample)) ||
                    (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
                    if (sample) sample->Release();
                    ended = true;
                    break;
                }
                if (!sample) continue;
                IMFMediaBuffer* media = nullptr;
                if (SUCCEEDED(sample->ConvertToContiguousBuffer(&media))) {
                    BYTE* data = nullptr;
                    DWORD size = 0;
                    if (SUCCEEDED(media->Lock(&data, nullptr, &size))) {
                        buffer.insert(buffer.end(), data, data + size);
                        media->Unlock();
                    }
                    media->Release();
                }
                sample->Release();
            }
            if (!buffer.empty()) {
                XAUDIO2_BUFFER b{};
                b.AudioBytes = static_cast<UINT32>(buffer.size() & ~3u);
                b.pAudioData = buffer.data();
                voice_->SubmitSourceBuffer(&b);
                decodedMs_ += static_cast<int64_t>(b.AudioBytes) * 1000 / (44100 * 4);
                fill = (fill + 1) % kBuffers;
            }
            if (ended) {
                reader->Release();
                Next();
                positionMs_ = 0;
                reader = Tune();
            }
        }
        if (reader) reader->Release();
        CoUninitialize();
    }

    std::vector<std::wstring> songs_;
    // The station's timeline, kept between tunings: the shuffled order, the
    // song it's on and how far into it (ms; -1: pick a random point).
    std::vector<int> order_;
    std::vector<int64_t> lengths_; // per song, ms (0: not known yet)
    size_t at_ = 0;
    int64_t positionMs_ = -1;
    std::atomic<int64_t> decodedMs_{0}; // how far into the song decoding has got
    uint32_t stoppedAtMs_ = 0;
    std::mt19937 rng_{static_cast<unsigned>(GetTickCount64())};
    // Queued buffers: they outlive the thread until Stop destroys the voice
    // that may still be reading them.
    std::vector<uint8_t> buffers_[kBuffers];
    IXAudio2SourceVoice* voice_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> song_{-1};
};
