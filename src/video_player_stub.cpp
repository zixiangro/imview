// Image-only build. The normal video_player.cpp links FFmpeg and miniaudio; this
// implementation keeps the Player API available so the shared UI code compiles,
// while media_paths.cpp reports no video extensions and the player is inert.

#include "video_player.h"

#include <memory>

namespace imview::video {

struct Player::Impl {};

Player::Player() : impl_(std::make_unique<Impl>()) {}
Player::~Player() = default;

void Player::open(const std::string&) {}
void Player::close() {}
std::shared_ptr<core::render::ImageStream> Player::stream() const { return nullptr; }
std::shared_ptr<core::render::ImageStream> Player::previewStream() const { return nullptr; }
Status Player::status() const { return Status{}; }
void Player::togglePlayPause() {}
void Player::seekBy(double) {}
void Player::seekTo(double) {}
void Player::stepFrames(int) {}
void Player::setVideoTrack(int) {}
void Player::setAudioTrack(int) {}
void Player::setSubtitleTrack(int) {}
void Player::setVolume(float) {}
void Player::setMuted(bool) {}
void Player::toggleMute() {}
void Player::toggleSubtitles() {}
void Player::requestPreview(double) {}

} // namespace imview::video
