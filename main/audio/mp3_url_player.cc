#include "mp3_url_player.h"

#include <vector>

#include <esp_log.h>
#include <esp_ae_rate_cvt.h>

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"

#define TAG "Mp3UrlPlayer"

namespace {

constexpr int kHttpConnectionId = 3;
constexpr int kHttpTimeoutMs = 30000;
constexpr size_t kReadBufferSize = 4096;
constexpr size_t kMaxMp3DownloadSize = 6 * 1024 * 1024;
constexpr size_t kInitialPcmBufferSize = 8192;
constexpr uint32_t kPlayerTaskStackSize = 16384;
constexpr size_t kPlaybackChunkMs = 2000;
constexpr size_t kPlaybackPrebufferMs = 4000;
constexpr size_t kPlaybackMaxPendingMs = 10000;

std::once_flag g_decoder_register_once;
bool g_decoder_register_ok = false;

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)        \
    (esp_ae_rate_cvt_cfg_t)                                  \
    {                                                        \
        .src_rate        = (uint32_t)(_src_rate),            \
        .dest_rate       = (uint32_t)(_dest_rate),           \
        .channel         = (uint8_t)(_channel),              \
        .bits_per_sample = ESP_AUDIO_BIT16,                  \
        .complexity      = 2,                                \
        .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,  \
    }

std::vector<int16_t> DownmixToMono(const int16_t* samples, size_t sample_count, uint8_t channels) {
    if (channels <= 1) {
        return std::vector<int16_t>(samples, samples + sample_count);
    }

    size_t frame_count = sample_count / channels;
    std::vector<int16_t> mono(frame_count);
    for (size_t frame = 0; frame < frame_count; ++frame) {
        int32_t sum = 0;
        for (uint8_t ch = 0; ch < channels; ++ch) {
            sum += samples[frame * channels + ch];
        }
        mono[frame] = static_cast<int16_t>(sum / channels);
    }
    return mono;
}

std::vector<int16_t> ExpandChannels(const std::vector<int16_t>& mono, uint8_t output_channels) {
    if (output_channels <= 1) {
        return mono;
    }

    std::vector<int16_t> expanded;
    expanded.reserve(mono.size() * output_channels);
    for (auto sample : mono) {
        for (uint8_t ch = 0; ch < output_channels; ++ch) {
            expanded.push_back(sample);
        }
    }
    return expanded;
}

bool EnsureMp3DecoderRegistered() {
    std::call_once(g_decoder_register_once, []() {
        auto dec_ret = esp_audio_dec_register_default();
        auto simple_ret = esp_audio_simple_dec_register_default();
        if (dec_ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Failed to register audio decoders, ret=%d", dec_ret);
            return;
        }
        if (simple_ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Failed to register simple audio decoders, ret=%d", simple_ret);
            return;
        }
        g_decoder_register_ok = true;
    });
    return g_decoder_register_ok;
}

bool DownloadMp3ToMemory(Http* http, std::vector<uint8_t>& mp3_data) {
    size_t content_length = http->GetBodyLength();
    if (content_length > kMaxMp3DownloadSize) {
        ESP_LOGE(TAG, "MP3 file too large: %u bytes", static_cast<unsigned>(content_length));
        return false;
    }

    if (content_length > 0) {
        mp3_data.reserve(content_length);
    }

    std::vector<uint8_t> read_buffer(kReadBufferSize);
    while (true) {
        int read_size = http->Read(reinterpret_cast<char*>(read_buffer.data()), read_buffer.size());
        if (read_size < 0) {
            ESP_LOGE(TAG, "Failed to read mp3 data, last_error=%d", http->GetLastError());
            return false;
        }
        if (read_size == 0) {
            break;
        }
        if (mp3_data.size() + read_size > kMaxMp3DownloadSize) {
            ESP_LOGE(TAG, "MP3 download exceeded limit: %u bytes",
                static_cast<unsigned>(mp3_data.size() + read_size));
            return false;
        }
        mp3_data.insert(mp3_data.end(), read_buffer.begin(), read_buffer.begin() + read_size);
    }

    ESP_LOGI(TAG, "Downloaded MP3: %u bytes", static_cast<unsigned>(mp3_data.size()));
    return !mp3_data.empty();
}

}  // namespace

Mp3UrlPlayer& Mp3UrlPlayer::GetInstance() {
    static Mp3UrlPlayer instance;
    return instance;
}

bool Mp3UrlPlayer::Play(const std::string& url, const std::string& music_name) {
    Stop();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_url_ = url;
        pending_music_name_ = music_name;
        stop_requested_.store(false);
    }

    BaseType_t ret = xTaskCreate(
        &Mp3UrlPlayer::PlaybackTaskEntry,
        "mp3_player",
        kPlayerTaskStackSize,
        this,
        3,
        &task_handle_);
    if (ret != pdPASS) {
        std::lock_guard<std::mutex> lock(mutex_);
        task_handle_ = nullptr;
        return false;
    }
    return true;
}

void Mp3UrlPlayer::Stop() {
    TaskHandle_t task_handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        task_handle = task_handle_;
    }
    if (task_handle == nullptr) {
        stop_requested_.store(false);
        return;
    }

    stop_requested_.store(true);
    Application::GetInstance().GetAudioService().ResetDecoder();

    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (task_handle_ == nullptr) {
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void Mp3UrlPlayer::PlaybackTaskEntry(void* arg) {
    auto* player = static_cast<Mp3UrlPlayer*>(arg);
    player->PlaybackTask();

    {
        std::lock_guard<std::mutex> lock(player->mutex_);
        player->task_handle_ = nullptr;
    }
    player->stop_requested_.store(false);
    vTaskDelete(nullptr);
}

void Mp3UrlPlayer::PlaybackTask() {
    std::string url;
    std::string music_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = pending_url_;
        music_name = pending_music_name_;
    }

    auto& app = Application::GetInstance();
    app.Schedule([music_name]() {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus("Playing music");
        display->SetEmotion("happy");
        display->SetChatMessage("assistant", music_name.empty() ? "Playing selected music" : music_name.c_str());
    });

    auto& audio_service = app.GetAudioService();
    bool restore_wake_word = audio_service.IsWakeWordRunning();
    if (restore_wake_word) {
        audio_service.EnableWakeWordDetection(false);
    }

    bool success = DecodeAndPlay(url);
    if (restore_wake_word && app.GetDeviceState() == kDeviceStateIdle) {
        audio_service.EnableWakeWordDetection(true);
    }
    if (stop_requested_.load()) {
        return;
    }

    app.Schedule([music_name, success]() {
        auto& app = Application::GetInstance();
        auto display = Board::GetInstance().GetDisplay();
        if (success) {
            if (app.GetDeviceState() == kDeviceStateIdle) {
                display->SetStatus("Standby");
                display->SetEmotion("neutral");
            }
            if (!music_name.empty()) {
                display->SetChatMessage("assistant", music_name.c_str());
            }
        } else {
            if (app.GetDeviceState() == kDeviceStateIdle) {
                display->SetStatus("Music error");
                display->SetEmotion("sad");
            }
            display->SetChatMessage("system", "Failed to play music");
        }
    });
}

bool Mp3UrlPlayer::DecodeAndPlay(const std::string& url) {
    if (!EnsureMp3DecoderRegistered()) {
        ESP_LOGE(TAG, "MP3 decoder is unavailable");
        return false;
    }

    auto& app = Application::GetInstance();
    auto& audio_service = app.GetAudioService();
    auto codec = Board::GetInstance().GetAudioCodec();
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(kHttpConnectionId);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create http client");
        return false;
    }

    http->SetTimeout(kHttpTimeoutMs);
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open mp3 stream url: %s, last_error=%d", url.c_str(), http->GetLastError());
        return false;
    }
    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Unexpected mp3 stream status code %d for url: %s", http->GetStatusCode(), url.c_str());
        http->Close();
        return false;
    }

    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = nullptr,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t decoder = nullptr;
    auto ret = esp_audio_simple_dec_open(&dec_cfg, &decoder);
    if (ret != ESP_AUDIO_ERR_OK || decoder == nullptr) {
        ESP_LOGE(TAG, "Failed to open mp3 decoder, ret=%d", ret);
        http->Close();
        return false;
    }

    esp_ae_rate_cvt_handle_t resampler = nullptr;
    int resampler_src_rate = 0;
    std::vector<uint8_t> mp3_data;
    if (!DownloadMp3ToMemory(http.get(), mp3_data)) {
        http->Close();
        return false;
    }
    http->Close();

    std::vector<uint8_t> pcm_buffer(kInitialPcmBufferSize);
    bool info_ready = false;
    esp_audio_simple_dec_info_t dec_info = {};
    const size_t playback_chunk_samples =
        static_cast<size_t>(codec->output_sample_rate()) * codec->output_channels() * kPlaybackChunkMs / 1000;
    const size_t playback_prebuffer_samples =
        static_cast<size_t>(codec->output_sample_rate()) * codec->output_channels() * kPlaybackPrebufferMs / 1000;
    const size_t playback_max_pending_samples =
        static_cast<size_t>(codec->output_sample_rate()) * codec->output_channels() * kPlaybackMaxPendingMs / 1000;
    std::vector<int16_t> pending_playback_pcm;
    pending_playback_pcm.reserve(playback_prebuffer_samples);
    size_t pending_playback_offset = 0;
    bool playback_started = false;

    audio_service.ResetDecoder();

    bool success = true;
    bool end_of_stream = false;
    bool decoded_any_frame = false;
    auto pending_sample_count = [&]() -> size_t {
        return pending_playback_pcm.size() - pending_playback_offset;
    };
    auto compact_pending_pcm = [&]() {
        if (pending_playback_offset == 0) {
            return;
        }
        if (pending_playback_offset >= pending_playback_pcm.size()) {
            pending_playback_pcm.clear();
            pending_playback_offset = 0;
            return;
        }
        if (pending_playback_offset >= playback_chunk_samples ||
            pending_playback_offset > pending_playback_pcm.size() / 2) {
            pending_playback_pcm.erase(
                pending_playback_pcm.begin(),
                pending_playback_pcm.begin() + pending_playback_offset);
            pending_playback_offset = 0;
        }
    };
    auto flush_pending_pcm = [&](bool flush_all, bool wait_for_queue) -> bool {
        while (pending_sample_count() > 0) {
            if (!playback_started) {
                if (!flush_all && pending_sample_count() < playback_prebuffer_samples) {
                    break;
                }
                playback_started = true;
                ESP_LOGI(TAG, "Start buffered playback: %u samples queued",
                    static_cast<unsigned>(pending_sample_count()));
            }

            if (!flush_all && pending_sample_count() < playback_chunk_samples) {
                break;
            }

            size_t chunk_samples = flush_all ?
                std::min(pending_sample_count(), playback_chunk_samples) :
                playback_chunk_samples;
            std::vector<int16_t> playback_pcm(
                pending_playback_pcm.begin() + pending_playback_offset,
                pending_playback_pcm.begin() + pending_playback_offset + chunk_samples);
            if (!audio_service.PushPcmToPlaybackQueue(std::move(playback_pcm), 0, wait_for_queue)) {
                if (!wait_for_queue) {
                    break;
                }
                ESP_LOGE(TAG, "Failed to enqueue playback pcm");
                return false;
            }
            pending_playback_offset += chunk_samples;
            compact_pending_pcm();
        }
        return true;
    };

    size_t mp3_offset = 0;
    while (!stop_requested_.load() && !end_of_stream && mp3_offset < mp3_data.size()) {
        size_t read_size = std::min(kReadBufferSize, mp3_data.size() - mp3_offset);
        bool is_last_chunk = mp3_offset + read_size >= mp3_data.size();

        esp_audio_simple_dec_raw_t raw = {
            .buffer = mp3_data.data() + mp3_offset,
            .len = static_cast<uint32_t>(read_size),
            .eos = is_last_chunk,
            .consumed = 0,
            .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
        };

        while (!stop_requested_.load() && (raw.len > 0 || raw.eos)) {
            esp_audio_simple_dec_out_t out_frame = {
                .buffer = pcm_buffer.data(),
                .len = static_cast<uint32_t>(pcm_buffer.size()),
                .needed_size = 0,
                .decoded_size = 0,
            };
            raw.consumed = 0;

            ret = esp_audio_simple_dec_process(decoder, &raw, &out_frame);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                pcm_buffer.resize(out_frame.needed_size);
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "Failed to decode mp3 stream, ret=%d", ret);
                success = false;
                end_of_stream = true;
                break;
            }

            if (out_frame.decoded_size > 0) {
                if (!info_ready) {
                    ret = esp_audio_simple_dec_get_info(decoder, &dec_info);
                    if (ret != ESP_AUDIO_ERR_OK) {
                        ESP_LOGE(TAG, "Failed to get mp3 info, ret=%d", ret);
                        success = false;
                        end_of_stream = true;
                        break;
                    }
                    info_ready = true;
                }

                if (dec_info.bits_per_sample != 16) {
                    ESP_LOGE(TAG, "Unsupported bits per sample: %u", dec_info.bits_per_sample);
                    success = false;
                    end_of_stream = true;
                    break;
                }

                size_t sample_count = out_frame.decoded_size / sizeof(int16_t);
                auto mono_pcm = DownmixToMono(reinterpret_cast<int16_t*>(out_frame.buffer), sample_count, dec_info.channel);
                if (dec_info.sample_rate != static_cast<uint32_t>(codec->output_sample_rate())) {
                    if (resampler == nullptr || resampler_src_rate != static_cast<int>(dec_info.sample_rate)) {
                        if (resampler != nullptr) {
                            esp_ae_rate_cvt_close(resampler);
                            resampler = nullptr;
                        }
                        esp_ae_rate_cvt_cfg_t resampler_cfg = RATE_CVT_CFG(
                            dec_info.sample_rate, codec->output_sample_rate(), ESP_AUDIO_MONO);
                        auto resampler_ret = esp_ae_rate_cvt_open(&resampler_cfg, &resampler);
                        if (resampler_ret != ESP_AE_ERR_OK || resampler == nullptr) {
                            ESP_LOGE(TAG, "Failed to open resampler, ret=%d", resampler_ret);
                            success = false;
                            end_of_stream = true;
                            break;
                        }
                        resampler_src_rate = dec_info.sample_rate;
                    }

                    uint32_t max_output_samples = 0;
                    esp_ae_rate_cvt_get_max_out_sample_num(resampler, mono_pcm.size(), &max_output_samples);
                    std::vector<int16_t> resampled_pcm(max_output_samples);
                    uint32_t actual_output_samples = max_output_samples;
                    esp_ae_rate_cvt_process(
                        resampler,
                        reinterpret_cast<esp_ae_sample_t>(mono_pcm.data()),
                        mono_pcm.size(),
                        reinterpret_cast<esp_ae_sample_t>(resampled_pcm.data()),
                        &actual_output_samples);
                    resampled_pcm.resize(actual_output_samples);
                    mono_pcm = std::move(resampled_pcm);
                }

                auto playback_pcm = ExpandChannels(mono_pcm, codec->output_channels());
                pending_playback_pcm.insert(
                    pending_playback_pcm.end(),
                    playback_pcm.begin(),
                    playback_pcm.end());
                if (!flush_pending_pcm(false, false)) {
                    success = false;
                    end_of_stream = true;
                    break;
                }
                compact_pending_pcm();
                if (pending_sample_count() > playback_max_pending_samples &&
                    !flush_pending_pcm(false, true)) {
                    success = false;
                    end_of_stream = true;
                    break;
                }
                decoded_any_frame = true;
            }

            if (raw.consumed > raw.len) {
                raw.consumed = raw.len;
            }
            raw.len -= raw.consumed;
            raw.buffer += raw.consumed;

            if (raw.len == 0 && raw.eos && out_frame.decoded_size == 0 && raw.consumed == 0) {
                end_of_stream = true;
                break;
            }
            if (raw.len == 0 && !raw.eos) {
                break;
            }
        }
        mp3_offset += read_size;
    }

    if (success && !stop_requested_.load() && !flush_pending_pcm(true, true)) {
        success = false;
    }

    if (success && !decoded_any_frame) {
        ESP_LOGE(TAG, "MP3 stream opened but produced no audio frames");
        success = false;
    }

    if (!stop_requested_.load() && success) {
        audio_service.WaitForPlaybackQueueEmpty();
    }

    if (resampler != nullptr) {
        esp_ae_rate_cvt_close(resampler);
    }
    esp_audio_simple_dec_close(decoder);
    return success;
}
