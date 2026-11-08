#pragma once

#include <memory>
#include <string>

namespace core::render {
class ImageStream;
}

namespace imview::video {

struct Status {
    bool active = false;
    bool failed = false;
    bool paused = false;
    bool ended = false;
    bool hasFrame = false;
    bool hasAudio = false;
    bool muted = false;
    bool hasSubtitles = false;
    bool subtitlesEnabled = true;
    std::string name;
    std::string error;
    std::string subtitle;
    double position = 0.0;
    double duration = 0.0;
    int width = 0;
    int height = 0;
    float volume = 0.5f;
    // Track counts and the selected zero-based track. Video always has at
    // least one track; audio and subtitles use -1 when the file has none.
    int videoTrack = 0;
    int videoTrackCount = 0;
    int audioTrack = -1;
    int audioTrackCount = 0;
    int subtitleTrack = -1;
    int subtitleTrackCount = 0;
};

// One video cell. The object owns the decoder/audio/preview threads and is
// destroyed when the cell closes; all methods are safe from the UI thread.
class Player {
public:
    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    void open(const std::string& path);
    void close();

    std::shared_ptr<core::render::ImageStream> stream() const;
    std::shared_ptr<core::render::ImageStream> previewStream() const;
    Status status() const;

    void togglePlayPause();
    void seekBy(double seconds);
    void seekTo(double seconds);
    void stepFrames(int delta);
    void setVideoTrack(int track);
    void setAudioTrack(int track);
    void setSubtitleTrack(int track);
    void setVolume(float volume);
    void setMuted(bool muted);
    void toggleMute();
    void toggleSubtitles();
    void requestPreview(double seconds);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace imview::video
