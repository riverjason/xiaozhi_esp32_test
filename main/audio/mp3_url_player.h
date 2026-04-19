#ifndef MP3_URL_PLAYER_H
#define MP3_URL_PLAYER_H

#include <atomic>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class Mp3UrlPlayer {
public:
    static Mp3UrlPlayer& GetInstance();

    bool Play(const std::string& url, const std::string& music_name);
    void Stop();

private:
    Mp3UrlPlayer() = default;
    Mp3UrlPlayer(const Mp3UrlPlayer&) = delete;
    Mp3UrlPlayer& operator=(const Mp3UrlPlayer&) = delete;

    static void PlaybackTaskEntry(void* arg);
    void PlaybackTask();
    bool DecodeAndPlay(const std::string& url);

    std::mutex mutex_;
    TaskHandle_t task_handle_ = nullptr;
    std::atomic<bool> stop_requested_{false};
    std::string pending_url_;
    std::string pending_music_name_;
};

#endif // MP3_URL_PLAYER_H
