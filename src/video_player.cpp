// Video cell playback: FFmpeg video/subtitle decoding, a small audio device
// stream, and an on-demand progress-bar previewer. The UI thread only reads a
// status snapshot and binds image streams; all decoding happens on worker
// threads owned by Player.

#include "video_player.h"

#include "utf8.h"

#include "core/platform/platform.h"
#include "core/render/image_stream.h"

#include "miniaudio.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace imview::video {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kAudioSampleRate = 48000;
constexpr int kAudioChannels = 2;
// Power-of-two frame count (about 1.36 s at 48 kHz) keeps the ring index
// arithmetic cheap in the audio callback.
constexpr std::uint32_t kRingFrames = 1u << 16;

std::string ffmpegError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

struct Nv12Frame {
    std::shared_ptr<const std::vector<std::uint8_t>> y;
    std::shared_ptr<const std::vector<std::uint8_t>> uv;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t yStride = 0;
    std::uint32_t uvStride = 0;
    core::render::ImageColorSpace colorSpace = core::render::ImageColorSpace::BT709;
    core::render::ImageColorRange colorRange = core::render::ImageColorRange::Limited;
};

core::render::ImageColorSpace colorSpaceFor(const AVFrame& frame) {
    switch (frame.colorspace) {
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            return core::render::ImageColorSpace::BT601;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            return core::render::ImageColorSpace::BT2020;
        case AVCOL_SPC_BT709:
        default:
            return core::render::ImageColorSpace::BT709;
    }
}

core::render::ImageColorRange colorRangeFor(const AVFrame& frame) {
    return frame.color_range == AVCOL_RANGE_JPEG
        ? core::render::ImageColorRange::Full
        : core::render::ImageColorRange::Limited;
}

bool copyPlane(std::vector<std::uint8_t>& destination,
               std::uint32_t destinationStride,
               const std::uint8_t* source,
               int sourceStride,
               std::uint32_t bytesPerRow,
               std::uint32_t rows) {
    if (source == nullptr || sourceStride == 0 ||
        std::abs(sourceStride) < static_cast<int>(bytesPerRow)) {
        return false;
    }
    destination.resize(static_cast<std::size_t>(destinationStride) * rows);
    for (std::uint32_t row = 0; row < rows; ++row) {
        const auto* sourceRow = source + static_cast<std::ptrdiff_t>(row) * sourceStride;
        auto* destinationRow =
            destination.data() + static_cast<std::size_t>(row) * destinationStride;
        std::memcpy(destinationRow, sourceRow, bytesPerRow);
    }
    return true;
}

bool isTextSubtitle(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_ASS:
        case AV_CODEC_ID_SUBRIP:
        case AV_CODEC_ID_WEBVTT:
        case AV_CODEC_ID_MOV_TEXT:
        case AV_CODEC_ID_TEXT:
        case AV_CODEC_ID_MICRODVD:
        case AV_CODEC_ID_SAMI:
        case AV_CODEC_ID_REALTEXT:
        case AV_CODEC_ID_SUBVIEWER:
        case AV_CODEC_ID_SUBVIEWER1:
            return true;
        default:
            return false;
    }
}

std::string cleanSubtitleText(std::string value) {
    // ASS/SSA events carry override tags in braces and use \N for hard line
    // breaks. SRT/WebVTT/MOV text are converted to the same ASS event form by
    // FFmpeg, so one cleaner handles every text subtitle we enable.
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size();) {
        if (value[index] == '{') {
            const std::size_t end = value.find('}', index + 1);
            if (end != std::string::npos) {
                index = end + 1;
                continue;
            }
        }
        if (value[index] == '\\' && index + 1 < value.size()) {
            const char next = value[index + 1];
            if (next == 'N' || next == 'n') {
                result.push_back('\n');
                index += 2;
                continue;
            }
            if (next == 'h') {
                result.push_back(' ');
                index += 2;
                continue;
            }
        }
        if (value[index] != '\r') result.push_back(value[index]);
        ++index;
    }
    while (!result.empty() &&
           (result.back() == '\n' || result.back() == ' ' || result.back() == '\t')) {
        result.pop_back();
    }
    std::size_t start = 0;
    while (start < result.size() &&
           (result[start] == '\n' || result[start] == ' ' || result[start] == '\t')) {
        ++start;
    }
    return imview::utf8::fromAnsiCodePage(result.substr(start));
}

std::string assEventText(const char* event) {
    if (event == nullptr) return {};
    const char* text = event;
    for (int field = 0; field < 8 && text != nullptr; ++field) {
        text = std::strchr(text, ',');
        if (text != nullptr) ++text;
    }
    return cleanSubtitleText(text != nullptr ? text : event);
}


struct SubtitleEvent {
    double start = 0.0;
    double end = 0.0;
    std::string text;
};

struct ExternalSubtitleTrack {
    std::string label;
    std::vector<SubtitleEvent> events;
};

struct EmbeddedSubtitleTrack {
    AVStream* stream = nullptr;
    AVCodecContext* codec = nullptr;
};

// Decode one AVPacket with a text subtitle decoder. Both embedded streams and
// sidecar files use this path, so timing and ASS cleanup stay in one place.
bool decodeSubtitlePacket(AVCodecContext* codec, AVStream* stream,
                          const AVPacket* packet, double& start, double& end,
                          std::string& text) {
    if (codec == nullptr || stream == nullptr || packet == nullptr) return false;
    AVSubtitle subtitle{};
    int gotSubtitle = 0;
    const int result = avcodec_decode_subtitle2(codec, &subtitle, &gotSubtitle, packet);
    bool ok = false;
    if (result >= 0 && gotSubtitle) {
        double base = 0.0;
        if (packet->pts != AV_NOPTS_VALUE) {
            base = static_cast<double>(packet->pts) * av_q2d(stream->time_base);
        } else if (subtitle.pts != AV_NOPTS_VALUE) {
            base = static_cast<double>(subtitle.pts) / AV_TIME_BASE;
        }
        start = base + subtitle.start_display_time / 1000.0;
        end = base + subtitle.end_display_time / 1000.0;
        if (end <= start) end = start + 4.0;

        for (unsigned index = 0; index < subtitle.num_rects; ++index) {
            const AVSubtitleRect* rect = subtitle.rects[index];
            if (rect == nullptr) continue;
            std::string line;
            if (rect->type == SUBTITLE_ASS && rect->ass != nullptr) {
                line = assEventText(rect->ass);
            } else if (rect->text != nullptr) {
                line = cleanSubtitleText(rect->text);
            }
            if (!line.empty()) {
                if (!text.empty()) text.push_back('\n');
                text += line;
            }
        }
        ok = !text.empty();
    }
    avsubtitle_free(&subtitle);
    return ok;
}

std::string lowerAscii(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

bool isSidecarExtension(const std::string& extension) {
    return extension == ".srt" || extension == ".ass" || extension == ".ssa" ||
           extension == ".vtt" || extension == ".smi" || extension == ".sub" ||
           extension == ".sbv";
}

// Find same-basename sidecars. `movie.zh.srt` and `movie.en.ass` count as
// sidecars for `movie.mkv`; `movie2.srt` does not.
std::vector<std::string> externalSubtitleCandidates(const std::string& videoPath) {
    std::vector<std::string> candidates;
    std::error_code error;
    const std::filesystem::path video = std::filesystem::u8path(videoPath);
    std::filesystem::path directory = video.parent_path();
    if (directory.empty()) directory = std::filesystem::current_path(error);
    const std::string stem = lowerAscii(video.stem().u8string());
    if (stem.empty()) return candidates;

    std::filesystem::directory_iterator iterator(directory, error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        const std::filesystem::directory_entry& entry = *iterator;
        std::error_code fileError;
        if (entry.is_regular_file(fileError) && !fileError) {
            const std::filesystem::path path = entry.path();
            const std::string extension = lowerAscii(path.extension().u8string());
            const std::string candidateStem = lowerAscii(path.stem().u8string());
            const bool sameName = candidateStem == stem ||
                startsWith(candidateStem, stem + ".") ||
                startsWith(candidateStem, stem + "-") ||
                startsWith(candidateStem, stem + "_");
            if (sameName && isSidecarExtension(extension)) {
                candidates.push_back(path.u8string());
            }
        }
        iterator.increment(error);
    }
    std::sort(candidates.begin(), candidates.end());
    return candidates;
}

// Sidecars are small text files, so decoding them once before the main video
// starts keeps track switching instant and avoids a second playback clock.
ExternalSubtitleTrack loadExternalSubtitle(const std::string& path,
                                           std::atomic<bool>* stop) {
    ExternalSubtitleTrack track;
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVIOInterruptCB interrupt{};
    struct Interrupt {
        std::atomic<bool>* stop;
        static int callback(void* opaque) {
            const auto* state = static_cast<const Interrupt*>(opaque);
            return state->stop != nullptr && state->stop->load() ? 1 : 0;
        }
    } interruptState{stop};
    interrupt.callback = &Interrupt::callback;
    interrupt.opaque = &interruptState;

    const auto cleanup = [&] {
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
    };

    format = avformat_alloc_context();
    if (format == nullptr) return track;
    format->interrupt_callback = interrupt;
    if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) {
        cleanup();
        return track;
    }
    if (avformat_find_stream_info(format, nullptr) < 0) {
        cleanup();
        return track;
    }

    int subtitleIndex = -1;
    for (unsigned index = 0; index < format->nb_streams; ++index) {
        AVStream* stream = format->streams[index];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE &&
            isTextSubtitle(stream->codecpar->codec_id)) {
            subtitleIndex = static_cast<int>(index);
            break;
        }
    }
    if (subtitleIndex < 0) {
        cleanup();
        return track;
    }
    AVStream* stream = format->streams[subtitleIndex];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
        cleanup();
        return track;
    }
    codec = avcodec_alloc_context3(decoder);
    if (codec == nullptr ||
        avcodec_parameters_to_context(codec, stream->codecpar) < 0 ||
        avcodec_open2(codec, decoder, nullptr) < 0) {
        cleanup();
        return track;
    }
    packet = av_packet_alloc();
    if (packet == nullptr) {
        cleanup();
        return track;
    }

    std::filesystem::path sidecar = std::filesystem::u8path(path);
    track.label = sidecar.filename().u8string();
    while (!stop->load() && av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == subtitleIndex) {
            double start = 0.0;
            double end = 0.0;
            std::string text;
            if (decodeSubtitlePacket(codec, stream, packet, start, end, text)) {
                track.events.push_back({start, end, std::move(text)});
            }
        }
        av_packet_unref(packet);
    }
    cleanup();
    return track;
}

std::vector<ExternalSubtitleTrack> loadExternalSubtitles(
    const std::string& videoPath, std::atomic<bool>* stop) {
    std::vector<ExternalSubtitleTrack> tracks;
    for (const std::string& path : externalSubtitleCandidates(videoPath)) {
        if (stop->load()) break;
        ExternalSubtitleTrack track = loadExternalSubtitle(path, stop);
        if (!track.events.empty()) tracks.push_back(std::move(track));
    }
    return tracks;
}

// --- video decoder ------------------------------------------------------------

class Decoder {
public:
    using SubtitleSink = std::function<void(int, double, double, std::string)>;

    ~Decoder() {
        close();
    }

    bool open(const std::string& path, std::atomic<bool>* stop, SubtitleSink sink) {
        stop_ = stop;
        subtitleSink_ = std::move(sink);
        AVIOInterruptCB interrupt{&Decoder::interruptCallback, this};
        format_ = avformat_alloc_context();
        if (format_ == nullptr) {
            error_ = "cannot allocate input context";
            return false;
        }
        format_->interrupt_callback = interrupt;

        int result = avformat_open_input(&format_, path.c_str(), nullptr, nullptr);
        if (result < 0) {
            error_ = "cannot open video: " + ffmpegError(result);
            return false;
        }
        result = avformat_find_stream_info(format_, nullptr);
        if (result < 0) {
            error_ = "cannot read video stream info: " + ffmpegError(result);
            return false;
        }

        // Enumerate every video, audio and text-subtitle stream once. The
        // zero-based ordinal in each vector is what the UI cycles through.
        for (unsigned index = 0; index < format_->nb_streams; ++index) {
            AVStream* candidate = format_->streams[index];
            switch (candidate->codecpar->codec_type) {
                case AVMEDIA_TYPE_VIDEO:
                    videoStreams_.push_back(static_cast<int>(index));
                    break;
                case AVMEDIA_TYPE_AUDIO:
                    audioStreams_.push_back(static_cast<int>(index));
                    break;
                case AVMEDIA_TYPE_SUBTITLE:
                    if (isTextSubtitle(candidate->codecpar->codec_id)) {
                        const AVCodec* subtitleCodec =
                            avcodec_find_decoder(candidate->codecpar->codec_id);
                        if (subtitleCodec != nullptr) {
                            AVCodecContext* subtitleContext =
                                avcodec_alloc_context3(subtitleCodec);
                            if (subtitleContext != nullptr &&
                                avcodec_parameters_to_context(
                                    subtitleContext, candidate->codecpar) >= 0 &&
                                avcodec_open2(subtitleContext, subtitleCodec,
                                              nullptr) >= 0) {
                                embeddedSubtitleTracks_.push_back(
                                    {candidate, subtitleContext});
                            } else {
                                avcodec_free_context(&subtitleContext);
                            }
                        }
                    }
                    break;
                default:
                    break;
            }
        }

        const int bestVideo = av_find_best_stream(
            format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (bestVideo < 0 || videoStreams_.empty()) {
            error_ = "the file has no video stream";
            return false;
        }
        for (std::size_t index = 0; index < videoStreams_.size(); ++index) {
            if (videoStreams_[index] == bestVideo) {
                selectedVideoTrack_ = static_cast<int>(index);
                break;
            }
        }
        streamIndex_ = videoStreams_[static_cast<std::size_t>(selectedVideoTrack_)];
        stream_ = format_->streams[streamIndex_];
        const AVCodec* codec = avcodec_find_decoder(stream_->codecpar->codec_id);
        if (codec == nullptr) {
            error_ = "no decoder is available for this video codec";
            return false;
        }
        codec_ = avcodec_alloc_context3(codec);
        if (codec_ == nullptr ||
            avcodec_parameters_to_context(codec_, stream_->codecpar) < 0 ||
            avcodec_open2(codec_, codec, nullptr) < 0) {
            error_ = "cannot initialize the video decoder";
            return false;
        }

        const int bestAudio = av_find_best_stream(
            format_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        for (std::size_t index = 0; index < audioStreams_.size(); ++index) {
            if (audioStreams_[index] == bestAudio) {
                defaultAudioTrack_ = static_cast<int>(index);
                break;
            }
        }
        for (std::size_t index = 0; index < embeddedSubtitleTracks_.size(); ++index) {
            if ((embeddedSubtitleTracks_[index].stream->disposition &
                 AV_DISPOSITION_DEFAULT) != 0) {
                defaultSubtitleTrack_ = static_cast<int>(index);
                break;
            }
        }
        if (defaultSubtitleTrack_ < 0 && !embeddedSubtitleTracks_.empty()) {
            defaultSubtitleTrack_ = 0;
        }

        frame_ = av_frame_alloc();
        nv12_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        if (frame_ == nullptr || nv12_ == nullptr || packet_ == nullptr) {
            error_ = "cannot allocate video decoding buffers";
            return false;
        }

        double frameRate = av_q2d(stream_->avg_frame_rate);
        if (frameRate <= 0.0) frameRate = av_q2d(stream_->r_frame_rate);
        frameRate_ = std::clamp(frameRate > 0.0 ? frameRate : 30.0, 1.0, 240.0);
        duration_ = format_->duration != AV_NOPTS_VALUE
            ? static_cast<double>(format_->duration) / AV_TIME_BASE
            : (stream_->duration != AV_NOPTS_VALUE
                ? static_cast<double>(stream_->duration) * av_q2d(stream_->time_base)
                : 0.0);
        if (!std::isfinite(duration_) || duration_ < 0.0) duration_ = 0.0;
        width_ = codec_->width;
        height_ = codec_->height;
        return true;
    }

    enum class NextResult { Frame, End, Error };

    NextResult next(Nv12Frame& destination, double& presentationTime) {
        for (;;) {
            const int receiveResult = avcodec_receive_frame(codec_, frame_);
            if (receiveResult == 0) {
                const double pts = frameTime(frame_);
                const bool copied = copyDecodedFrame(destination);
                av_frame_unref(frame_);
                if (!copied) {
                    if (error_.empty()) error_ = "cannot convert the decoded frame to NV12";
                    return NextResult::Error;
                }
                presentationTime = pts;
                return NextResult::Frame;
            }
            if (receiveResult == AVERROR_EOF) return NextResult::End;
            if (receiveResult != AVERROR(EAGAIN)) {
                error_ = "cannot receive a decoded frame: " + ffmpegError(receiveResult);
                return NextResult::Error;
            }
            if (draining_) return NextResult::End;

            for (;;) {
                if (packetPending_) {
                    const int sendResult = avcodec_send_packet(codec_, packet_);
                    if (sendResult == 0) {
                        av_packet_unref(packet_);
                        packetPending_ = false;
                        break;
                    }
                    if (sendResult == AVERROR(EAGAIN)) break;
                    error_ = "cannot submit a pending video packet: " +
                             ffmpegError(sendResult);
                    return NextResult::Error;
                }

                const int readResult = av_read_frame(format_, packet_);
                if (readResult < 0) {
                    if (readResult == AVERROR_EXIT) return NextResult::End;
                    if (readResult != AVERROR_EOF) {
                        error_ = "cannot read video data: " + ffmpegError(readResult);
                        return NextResult::Error;
                    }
                    const int flushResult = avcodec_send_packet(codec_, nullptr);
                    if (flushResult < 0 && flushResult != AVERROR_EOF) {
                        error_ = "cannot flush the video decoder: " +
                                 ffmpegError(flushResult);
                        return NextResult::Error;
                    }
                    draining_ = true;
                    break;
                }
                bool decodedSubtitle = false;
                for (std::size_t track = 0;
                     track < embeddedSubtitleTracks_.size(); ++track) {
                    if (packet_->stream_index ==
                        embeddedSubtitleTracks_[track].stream->index) {
                        decodeSubtitle(static_cast<int>(track), packet_);
                        decodedSubtitle = true;
                        break;
                    }
                }
                if (decodedSubtitle) {
                    av_packet_unref(packet_);
                    continue;
                }
                if (packet_->stream_index != streamIndex_) {
                    av_packet_unref(packet_);
                    continue;
                }

                const int sendResult = avcodec_send_packet(codec_, packet_);
                if (sendResult == 0) {
                    av_packet_unref(packet_);
                } else if (sendResult == AVERROR(EAGAIN)) {
                    packetPending_ = true;
                } else {
                    av_packet_unref(packet_);
                    error_ = "cannot submit a video packet: " + ffmpegError(sendResult);
                    return NextResult::Error;
                }
                break;
            }
        }
    }

    bool seek(double seconds) {
        if (format_ == nullptr || stream_ == nullptr || codec_ == nullptr) return false;
        if (duration_ > 0.0) seconds = std::clamp(seconds, 0.0, duration_);
        else seconds = std::max(0.0, seconds);
        const int64_t timestamp = av_rescale_q(
            static_cast<int64_t>(seconds * AV_TIME_BASE), AV_TIME_BASE_Q,
            stream_->time_base);
        if (av_seek_frame(format_, streamIndex_, timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
            return false;
        }
        avcodec_flush_buffers(codec_);
        av_packet_unref(packet_);
        packetPending_ = false;
        draining_ = false;
        lastPts_ = seconds;
        for (auto& track : embeddedSubtitleTracks_) {
            if (track.codec != nullptr) avcodec_flush_buffers(track.codec);
        }
        return true;
    }

    // Switch video streams without reopening the container. The caller seeks
    // to the current playback position afterwards, so audio and subtitles keep
    // running on the same timeline.
    bool selectVideoTrack(int track, double seconds) {
        if (track < 0 || track >= static_cast<int>(videoStreams_.size())) return false;
        if (track == selectedVideoTrack_ && codec_ != nullptr) return seek(seconds);
        AVStream* nextStream = format_->streams[
            videoStreams_[static_cast<std::size_t>(track)]];
        const AVCodec* decoder =
            avcodec_find_decoder(nextStream->codecpar->codec_id);
        if (decoder == nullptr) {
            error_ = "no decoder is available for this video track";
            return false;
        }
        AVCodecContext* nextCodec = avcodec_alloc_context3(decoder);
        if (nextCodec == nullptr ||
            avcodec_parameters_to_context(nextCodec, nextStream->codecpar) < 0 ||
            avcodec_open2(nextCodec, decoder, nullptr) < 0) {
            avcodec_free_context(&nextCodec);
            error_ = "cannot initialize the selected video track";
            return false;
        }
        avcodec_free_context(&codec_);
        codec_ = nextCodec;
        stream_ = nextStream;
        streamIndex_ = static_cast<int>(nextStream->index);
        selectedVideoTrack_ = track;
        width_ = codec_->width;
        height_ = codec_->height;
        double frameRate = av_q2d(stream_->avg_frame_rate);
        if (frameRate <= 0.0) frameRate = av_q2d(stream_->r_frame_rate);
        frameRate_ = std::clamp(frameRate > 0.0 ? frameRate : 30.0, 1.0, 240.0);
        av_frame_unref(frame_);
        return seek(seconds);
    }

    double frameRate() const { return frameRate_; }
    double duration() const { return duration_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool hasSubtitles() const { return !embeddedSubtitleTracks_.empty(); }
    int videoTrackCount() const { return static_cast<int>(videoStreams_.size()); }
    int audioTrackCount() const { return static_cast<int>(audioStreams_.size()); }
    int subtitleTrackCount() const {
        return static_cast<int>(embeddedSubtitleTracks_.size());
    }
    int selectedVideoTrack() const { return selectedVideoTrack_; }
    int defaultAudioTrack() const { return defaultAudioTrack_; }
    int defaultSubtitleTrack() const { return defaultSubtitleTrack_; }
    const std::string& error() const { return error_; }

private:
    static int interruptCallback(void* opaque) {
        const auto* decoder = static_cast<const Decoder*>(opaque);
        return decoder->stop_ != nullptr && decoder->stop_->load() ? 1 : 0;
    }

    double frameTime(const AVFrame* frame) {
        const int64_t timestamp = frame->best_effort_timestamp;
        if (timestamp != AV_NOPTS_VALUE && stream_ != nullptr) {
            lastPts_ = static_cast<double>(timestamp) * av_q2d(stream_->time_base);
        } else {
            lastPts_ += 1.0 / frameRate_;
        }
        return lastPts_;
    }

    void decodeSubtitle(int track, const AVPacket* packet) {
        if (track < 0 || track >= static_cast<int>(embeddedSubtitleTracks_.size()) ||
            subtitleSink_ == nullptr) {
            return;
        }
        const EmbeddedSubtitleTrack& subtitle = embeddedSubtitleTracks_[
            static_cast<std::size_t>(track)];
        double start = 0.0;
        double end = 0.0;
        std::string text;
        if (decodeSubtitlePacket(subtitle.codec, subtitle.stream, packet,
                                 start, end, text)) {
            subtitleSink_(track, start, end, std::move(text));
        }
    }

    bool prepareNv12Buffer(int width, int height) {
        if (width == outputWidth_ && height == outputHeight_ &&
            av_frame_make_writable(nv12_) >= 0) {
            return true;
        }
        av_frame_unref(nv12_);
        nv12_->format = AV_PIX_FMT_NV12;
        nv12_->width = width;
        nv12_->height = height;
        if (av_frame_get_buffer(nv12_, 32) < 0) {
            error_ = "cannot allocate the NV12 conversion buffer";
            return false;
        }
        outputWidth_ = width;
        outputHeight_ = height;
        return true;
    }

    bool copyDecodedFrame(Nv12Frame& destination) {
        const int width = frame_->width;
        const int height = frame_->height;
        if (width <= 0 || height <= 0) {
            error_ = "the decoder produced an invalid frame size";
            return false;
        }

        const AVFrame* source = frame_;
        if (frame_->format != AV_PIX_FMT_NV12) {
            if (!prepareNv12Buffer(width, height)) return false;
            sws_ = sws_getCachedContext(
                sws_, width, height, static_cast<AVPixelFormat>(frame_->format),
                width, height, AV_PIX_FMT_NV12, SWS_BILINEAR,
                nullptr, nullptr, nullptr);
            if (sws_ == nullptr ||
                sws_scale(sws_, frame_->data, frame_->linesize, 0, height,
                          nv12_->data, nv12_->linesize) <= 0) {
                error_ = "cannot convert the decoded frame to NV12";
                return false;
            }
            source = nv12_;
        }

        const std::uint32_t frameWidth = static_cast<std::uint32_t>(width);
        const std::uint32_t frameHeight = static_cast<std::uint32_t>(height);
        const std::uint32_t chromaHeight = (frameHeight + 1u) / 2u;
        const std::uint32_t uvStride = ((frameWidth + 1u) / 2u) * 2u;
        auto y = acquirePlaneBuffer(false);
        auto uv = acquirePlaneBuffer(true);
        if (!copyPlane(*y, frameWidth, source->data[0], source->linesize[0],
                       frameWidth, frameHeight) ||
            !copyPlane(*uv, uvStride, source->data[1], source->linesize[1],
                       uvStride, chromaHeight)) {
            error_ = "the NV12 planes are incomplete";
            return false;
        }
        destination = {y, uv, frameWidth, frameHeight, frameWidth, uvStride,
                       colorSpaceFor(*frame_), colorRangeFor(*frame_)};
        return true;
    }

    std::shared_ptr<std::vector<std::uint8_t>> acquirePlaneBuffer(bool chroma) {
        auto& buffers = chroma ? uvBuffers_ : yBuffers_;
        for (std::size_t attempt = 0; attempt < buffers.size(); ++attempt) {
            const std::size_t index = (bufferCursor_ + attempt) % buffers.size();
            if (!buffers[index] || buffers[index].use_count() == 1) {
                if (!buffers[index]) {
                    buffers[index] = std::make_shared<std::vector<std::uint8_t>>();
                }
                bufferCursor_ = (index + 1) % buffers.size();
                return buffers[index];
            }
        }
        return std::make_shared<std::vector<std::uint8_t>>();
    }

    void close() {
        sws_freeContext(sws_);
        sws_ = nullptr;
        av_packet_free(&packet_);
        av_frame_free(&nv12_);
        av_frame_free(&frame_);
        for (auto& track : embeddedSubtitleTracks_) {
            avcodec_free_context(&track.codec);
        }
        embeddedSubtitleTracks_.clear();
        avcodec_free_context(&codec_);
        avformat_close_input(&format_);
        videoStreams_.clear();
        audioStreams_.clear();
        stream_ = nullptr;
        streamIndex_ = -1;
        selectedVideoTrack_ = 0;
        defaultAudioTrack_ = -1;
        defaultSubtitleTrack_ = -1;
    }

    AVFormatContext* format_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* nv12_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVStream* stream_ = nullptr;
    SwsContext* sws_ = nullptr;
    std::atomic<bool>* stop_ = nullptr;
    SubtitleSink subtitleSink_;
    std::vector<int> videoStreams_;
    std::vector<int> audioStreams_;
    std::vector<EmbeddedSubtitleTrack> embeddedSubtitleTracks_;
    int streamIndex_ = -1;
    int selectedVideoTrack_ = 0;
    int defaultAudioTrack_ = -1;
    int defaultSubtitleTrack_ = -1;
    int outputWidth_ = 0;
    int outputHeight_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool draining_ = false;
    bool packetPending_ = false;
    double frameRate_ = 30.0;
    double duration_ = 0.0;
    double lastPts_ = 0.0;
    std::string error_;
    std::array<std::shared_ptr<std::vector<std::uint8_t>>, 3> yBuffers_;
    std::array<std::shared_ptr<std::vector<std::uint8_t>>, 3> uvBuffers_;
    std::size_t bufferCursor_ = 0;
};

// --- audio --------------------------------------------------------------------

class AudioPlayer {
public:
    ~AudioPlayer() {
        close();
    }

    void open(std::string path, int track = 0, double start = 0.0,
              bool paused = false) {
        close();
        path_ = std::move(path);
        initialTrack_ = track;
        startPosition_ = std::max(0.0, start);
        stop_ = false;
        paused_ = paused;
        ended_ = false;
        seekPending_ = false;
        readFrame_ = 0;
        writeFrame_ = 0;
        worker_ = std::thread([this] { run(); });
    }

    void close() {
        stop_ = true;
        commandCv_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (deviceReady_) {
            ma_device_uninit(&device_);
            deviceReady_ = false;
        }
        deviceStarted_ = false;
        hasAudio_ = false;
    }

    bool hasAudio() const { return hasAudio_.load(); }

    void setPaused(bool paused) {
        paused_ = paused;
        if (deviceReady_) {
            if (paused) {
                if (deviceStarted_) {
                    ma_device_stop(&device_);
                    deviceStarted_ = false;
                }
            } else if (!seekPending_.load() && !ended_) {
                ma_device_start(&device_);
                deviceStarted_ = true;
            }
        }
        commandCv_.notify_all();
    }

    void seek(double seconds) {
        ended_ = false;
        if (deviceReady_ && deviceStarted_) {
            ma_device_stop(&device_);
            deviceStarted_ = false;
        }
        {
            std::lock_guard lock(commandMutex_);
            seekTarget_ = std::max(0.0, seconds);
            seekPending_ = true;
        }
        commandCv_.notify_all();
    }

    void setVolume(float volume) {
        volume_ = std::clamp(volume, 0.0f, 1.0f);
        applyVolume();
    }

    void setMuted(bool muted) {
        muted_ = muted;
        applyVolume();
    }

    float volume() const { return volume_; }
    bool muted() const { return muted_; }

private:
    static void dataCallback(ma_device* device, void* output,
                             const void*, ma_uint32 frameCount) {
        static_cast<AudioPlayer*>(device->pUserData)->readPcm(
            static_cast<float*>(output), frameCount);
    }

    void applyVolume() {
        if (deviceReady_) {
            ma_device_set_master_volume(
                &device_, muted_.load() ? 0.0f : volume_.load());
        }
    }

    void readPcm(float* output, ma_uint32 frameCount) {
        const std::uint32_t channels = kAudioChannels;
        std::fill(output, output + static_cast<std::size_t>(frameCount) * channels, 0.0f);
        const std::uint64_t read = readFrame_.load(std::memory_order_acquire);
        const std::uint64_t write = writeFrame_.load(std::memory_order_acquire);
        const std::uint64_t available = std::min<std::uint64_t>(frameCount, write - read);
        for (std::uint64_t frame = 0; frame < available; ++frame) {
            const std::size_t source =
                static_cast<std::size_t>((read + frame) % kRingFrames) * channels;
            const std::size_t destination =
                static_cast<std::size_t>(frame) * channels;
            output[destination] = ring_[source];
            output[destination + 1] = ring_[source + 1];
        }
        readFrame_.store(read + available, std::memory_order_release);
    }

    bool writePcm(const float* samples, std::size_t frames) {
        while (frames > 0) {
            if (stop_) return false;
            if (seekPending_.load()) return false;
            const std::uint64_t write = writeFrame_.load(std::memory_order_relaxed);
            const std::uint64_t read = readFrame_.load(std::memory_order_acquire);
            const std::uint64_t used = write - read;
            if (used >= kRingFrames) {
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                continue;
            }
            const std::size_t available = static_cast<std::size_t>(kRingFrames - used);
            const std::size_t count = std::min(frames, available);
            for (std::size_t frame = 0; frame < count; ++frame) {
                const std::size_t destination =
                    static_cast<std::size_t>((write + frame) % kRingFrames) * kAudioChannels;
                ring_[destination] = samples[frame * kAudioChannels];
                ring_[destination + 1] = samples[frame * kAudioChannels + 1];
            }
            writeFrame_.store(write + count, std::memory_order_release);
            samples += count * kAudioChannels;
            frames -= count;
        }
        return true;
    }

    bool consumeSeek(double& target) {
        std::lock_guard lock(commandMutex_);
        if (!seekPending_) return false;
        target = seekTarget_;
        seekPending_ = false;
        return true;
    }

    static int interruptCallback(void* opaque) {
        const auto* audio = static_cast<const AudioPlayer*>(opaque);
        return audio->stop_.load() ? 1 : 0;
    }

    bool applySeek(AVFormatContext* format, AVCodecContext* codec,
                   AVStream* stream, SwrContext* swr, double target) {
        if (format == nullptr || codec == nullptr || stream == nullptr) return false;
        const int64_t timestamp = av_rescale_q(
            static_cast<int64_t>(target * AV_TIME_BASE), AV_TIME_BASE_Q,
            stream->time_base);
        if (av_seek_frame(format, stream->index, timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
            return false;
        }
        avcodec_flush_buffers(codec);
        if (swr != nullptr) {
            swr_close(swr);
            swr_init(swr);
        }
        if (deviceStarted_) {
            ma_device_stop(&device_);
            deviceStarted_ = false;
        }
        ended_ = false;
        readFrame_ = 0;
        writeFrame_ = 0;
        if (!paused_ && deviceReady_.load()) {
            ma_device_start(&device_);
            deviceStarted_ = true;
        }
        return true;
    }

    void run() {
        av_log_set_level(AV_LOG_ERROR);
        AVFormatContext* format = nullptr;
        AVCodecContext* codec = nullptr;
        AVStream* stream = nullptr;
        SwrContext* swr = nullptr;
        AVFrame* frame = nullptr;
        AVPacket* packet = nullptr;
        int audioIndex = -1;

        const auto cleanup = [&] {
            swr_free(&swr);
            av_packet_free(&packet);
            av_frame_free(&frame);
            avcodec_free_context(&codec);
            avformat_close_input(&format);
        };

        AVIOInterruptCB interrupt{&AudioPlayer::interruptCallback, this};
        format = avformat_alloc_context();
        if (format == nullptr) return;
        format->interrupt_callback = interrupt;
        if (avformat_open_input(&format, path_.c_str(), nullptr, nullptr) < 0) {
            cleanup();
            return;
        }
        if (avformat_find_stream_info(format, nullptr) < 0) {
            cleanup();
            return;
        }
        std::vector<int> audioStreams;
        for (unsigned index = 0; index < format->nb_streams; ++index) {
            if (format->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audioStreams.push_back(static_cast<int>(index));
            }
        }
        if (!audioStreams.empty()) {
            if (initialTrack_ >= 0 &&
                initialTrack_ < static_cast<int>(audioStreams.size())) {
                audioIndex = audioStreams[static_cast<std::size_t>(initialTrack_)];
            } else {
                audioIndex = av_find_best_stream(
                    format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            }
        }
        if (audioIndex < 0) {
            cleanup();
            return;
        }
        stream = format->streams[audioIndex];
        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (decoder == nullptr) {
            cleanup();
            return;
        }
        codec = avcodec_alloc_context3(decoder);
        if (codec == nullptr ||
            avcodec_parameters_to_context(codec, stream->codecpar) < 0 ||
            avcodec_open2(codec, decoder, nullptr) < 0) {
            cleanup();
            return;
        }

        AVChannelLayout inLayout = codec->ch_layout;
        if (inLayout.order == AV_CHANNEL_ORDER_UNSPEC) {
            av_channel_layout_default(&inLayout, codec->ch_layout.nb_channels);
        }
        AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, kAudioSampleRate,
                                &inLayout, codec->sample_fmt, codec->sample_rate,
                                0, nullptr) < 0 ||
            swr_init(swr) < 0) {
            cleanup();
            return;
        }
        frame = av_frame_alloc();
        packet = av_packet_alloc();
        if (frame == nullptr || packet == nullptr) {
            cleanup();
            return;
        }

        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = kAudioChannels;
        config.sampleRate = kAudioSampleRate;
        config.dataCallback = &AudioPlayer::dataCallback;
        config.pUserData = this;
        if (ma_device_init(nullptr, &config, &device_) != MA_SUCCESS) {
            cleanup();
            return;
        }
        deviceReady_ = true;
        hasAudio_ = true;
        core::platform::requestUiUpdate();
        applyVolume();
        if (!applySeek(format, codec, stream, swr, startPosition_)) {
            // A failed initial seek should not kill audio entirely; play from
            // the stream start instead.
            if (!paused_ && deviceReady_) {
                ma_device_start(&device_);
                deviceStarted_ = true;
            }
        }

        std::vector<float> converted;
        while (!stop_) {
            double target = 0.0;
            if (consumeSeek(target)) {
                if (!applySeek(format, codec, stream, swr, target)) break;
                continue;
            }
            if (paused_) {
                std::unique_lock lock(commandMutex_);
                commandCv_.wait_for(lock, std::chrono::milliseconds(20), [this] {
                    return stop_.load() || !paused_.load() || seekPending_.load();
                });
                continue;
            }

            const int readResult = av_read_frame(format, packet);
            if (readResult < 0) {
                if (readResult == AVERROR_EXIT) break;
                if (readResult == AVERROR_EOF) {
                    // Stop at the end instead of looping. A seek (video
                    // transport, sync restart) clears this and resumes.
                    ended_ = true;
                    if (deviceStarted_) {
                        ma_device_stop(&device_);
                        deviceStarted_ = false;
                    }
                    std::unique_lock lock(commandMutex_);
                    commandCv_.wait_for(lock, std::chrono::milliseconds(20), [this] {
                        return stop_.load() || seekPending_.load();
                    });
                    continue;
                }
                break;
            }
            if (packet->stream_index != audioIndex) {
                av_packet_unref(packet);
                continue;
            }

            const int sendResult = avcodec_send_packet(codec, packet);
            av_packet_unref(packet);
            if (sendResult < 0 && sendResult != AVERROR(EAGAIN)) break;

            for (;;) {
                const int receiveResult = avcodec_receive_frame(codec, frame);
                if (receiveResult == AVERROR(EAGAIN)) break;
                if (receiveResult < 0) break;
                const int outSamples = swr_get_out_samples(swr, frame->nb_samples);
                if (outSamples > 0) {
                    converted.resize(static_cast<std::size_t>(outSamples) * kAudioChannels);
                    uint8_t* destination[1] = {
                        reinterpret_cast<uint8_t*>(converted.data())};
                    const int convertedFrames = swr_convert(
                        swr, destination, outSamples,
                        const_cast<const uint8_t**>(frame->extended_data),
                        frame->nb_samples);
                    if (convertedFrames > 0 &&
                        !writePcm(converted.data(),
                                  static_cast<std::size_t>(convertedFrames))) {
                        break;
                    }
                }
                av_frame_unref(frame);
                if (stop_ || paused_ || seekPending_.load()) break;
            }
        }

        cleanup();
    }

    std::string path_;
    int initialTrack_ = 0;
    double startPosition_ = 0.0;
    ma_device device_{};
    std::atomic<bool> deviceReady_{false};
    std::atomic<bool> deviceStarted_{false};
    std::atomic<bool> hasAudio_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> seekPending_{false};
    std::atomic<bool> ended_{false};
    std::atomic<float> volume_{0.5f};
    std::atomic<bool> muted_{false};
    std::mutex commandMutex_;
    std::condition_variable commandCv_;
    double seekTarget_ = 0.0;
    std::thread worker_;
    std::vector<float> ring_ =
        std::vector<float>(static_cast<std::size_t>(kRingFrames) * kAudioChannels);
    std::atomic<std::uint64_t> readFrame_{0};
    std::atomic<std::uint64_t> writeFrame_{0};
};

// --- progress preview ---------------------------------------------------------

struct PreviewImage {
    std::shared_ptr<const std::vector<std::uint8_t>> pixels;
    int width = 0;
    int height = 0;
};

class PreviewDecoder {
public:
    ~PreviewDecoder() {
        close();
    }

    bool open(const std::string& path, std::atomic<bool>* stop) {
        stop_ = stop;
        AVIOInterruptCB interrupt{&PreviewDecoder::interruptCallback, this};
        format_ = avformat_alloc_context();
        if (format_ == nullptr) return false;
        format_->interrupt_callback = interrupt;
        if (avformat_open_input(&format_, path.c_str(), nullptr, nullptr) < 0) return false;
        if (avformat_find_stream_info(format_, nullptr) < 0) return false;
        streamIndex_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (streamIndex_ < 0) return false;
        stream_ = format_->streams[streamIndex_];
        const AVCodec* decoder = avcodec_find_decoder(stream_->codecpar->codec_id);
        if (decoder == nullptr) return false;
        codec_ = avcodec_alloc_context3(decoder);
        if (codec_ == nullptr ||
            avcodec_parameters_to_context(codec_, stream_->codecpar) < 0) {
            return false;
        }
        codec_->thread_count = 1;
        if (avcodec_open2(codec_, decoder, nullptr) < 0) return false;
        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        if (frame_ == nullptr || packet_ == nullptr) return false;
        duration_ = format_->duration != AV_NOPTS_VALUE
            ? static_cast<double>(format_->duration) / AV_TIME_BASE
            : 0.0;
        return true;
    }

    bool decodeAt(double seconds, PreviewImage& output) {
        if (format_ == nullptr || stream_ == nullptr) return false;
        if (duration_ > 0.0) seconds = std::clamp(seconds, 0.0, duration_);
        else seconds = std::max(0.0, seconds);
        const int64_t timestamp = av_rescale_q(
            static_cast<int64_t>(seconds * AV_TIME_BASE), AV_TIME_BASE_Q,
            stream_->time_base);
        if (av_seek_frame(format_, streamIndex_, timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
            return false;
        }
        avcodec_flush_buffers(codec_);
        draining_ = false;

        for (int attempt = 0; attempt < 180 && !stop_->load(); ++attempt) {
            const int receiveResult = avcodec_receive_frame(codec_, frame_);
            if (receiveResult == 0) {
                const int64_t pts = frame_->best_effort_timestamp;
                const double frameTime = pts != AV_NOPTS_VALUE
                    ? static_cast<double>(pts) * av_q2d(stream_->time_base)
                    : seconds;
                const bool convert = frameTime + 0.001 >= seconds;
                bool ok = false;
                if (convert) ok = convertFrame(output);
                av_frame_unref(frame_);
                if (convert) return ok;
                continue;
            }
            if (receiveResult == AVERROR_EOF) return false;
            if (receiveResult != AVERROR(EAGAIN)) return false;
            if (draining_) return false;

            for (;;) {
                const int readResult = av_read_frame(format_, packet_);
                if (readResult < 0) {
                    if (readResult == AVERROR_EXIT) return false;
                    if (readResult != AVERROR_EOF) return false;
                    avcodec_send_packet(codec_, nullptr);
                    draining_ = true;
                    break;
                }
                if (packet_->stream_index != streamIndex_) {
                    av_packet_unref(packet_);
                    continue;
                }
                avcodec_send_packet(codec_, packet_);
                av_packet_unref(packet_);
                break;
            }
        }
        return false;
    }

private:
    static int interruptCallback(void* opaque) {
        const auto* decoder = static_cast<const PreviewDecoder*>(opaque);
        return decoder->stop_ != nullptr && decoder->stop_->load() ? 1 : 0;
    }

    bool convertFrame(PreviewImage& output) {
        const int width = frame_->width;
        const int height = frame_->height;
        if (width <= 0 || height <= 0) return false;
        const double scale = std::min({160.0 / width, 90.0 / height, 1.0});
        const int outWidth = std::max(1, static_cast<int>(std::lround(width * scale)));
        const int outHeight = std::max(1, static_cast<int>(std::lround(height * scale)));
        sws_ = sws_getCachedContext(
            sws_, width, height, static_cast<AVPixelFormat>(frame_->format),
            outWidth, outHeight, AV_PIX_FMT_RGBA, SWS_BILINEAR,
            nullptr, nullptr, nullptr);
        if (sws_ == nullptr) return false;

        auto pixels = std::make_shared<std::vector<std::uint8_t>>(
            static_cast<std::size_t>(outWidth) * outHeight * 4u);
        uint8_t* destination[4] = {pixels->data(), nullptr, nullptr, nullptr};
        int destinationStride[4] = {outWidth * 4, 0, 0, 0};
        if (sws_scale(sws_, frame_->data, frame_->linesize, 0, height,
                      destination, destinationStride) <= 0) {
            return false;
        }
        output.pixels = pixels;
        output.width = outWidth;
        output.height = outHeight;
        return true;
    }

    void close() {
        sws_freeContext(sws_);
        av_packet_free(&packet_);
        av_frame_free(&frame_);
        avcodec_free_context(&codec_);
        avformat_close_input(&format_);
    }

    AVFormatContext* format_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVStream* stream_ = nullptr;
    SwsContext* sws_ = nullptr;
    std::atomic<bool>* stop_ = nullptr;
    int streamIndex_ = -1;
    bool draining_ = false;
    double duration_ = 0.0;
};

class Previewer {
public:
    explicit Previewer(std::string path)
        : path_(std::move(path)),
          stream_(std::make_shared<core::render::ImageStream>(1)) {}

    ~Previewer() {
        close();
    }

    std::shared_ptr<core::render::ImageStream> stream() const {
        return stream_;
    }

    void request(double seconds) {
        {
            std::lock_guard lock(mutex_);
            if (!started_) {
                started_ = true;
                worker_ = std::thread([this] { run(); });
            }
            requestTime_ = std::max(0.0, seconds);
            hasRequest_ = true;
        }
        condition_.notify_all();
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
            hasRequest_ = false;
        }
        condition_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

private:
    void run() {
        PreviewDecoder decoder;
        bool opened = false;
        std::map<int, PreviewImage> cache;
        int lastBucket = -1;
        Clock::time_point lastSubmit{};
        while (true) {
            double target = 0.0;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return stop_ || hasRequest_; });
                if (stop_) break;
                target = requestTime_;
                hasRequest_ = false;
            }
            if (!opened) {
                opened = decoder.open(path_, &stop_);
                if (!opened) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    continue;
                }
            }
            const int bucket = static_cast<int>(std::floor(target));
            const auto now = Clock::now();
            if (bucket == lastBucket &&
                now - lastSubmit < std::chrono::milliseconds(200)) {
                continue;
            }
            auto cached = cache.find(bucket);
            if (cached != cache.end()) {
                submit(cached->second);
                lastBucket = bucket;
                lastSubmit = now;
                continue;
            }
            PreviewImage image;
            if (decoder.decodeAt(target, image)) {
                cache[bucket] = image;
                while (cache.size() > 64) cache.erase(cache.begin());
                submit(image);
                lastBucket = bucket;
                lastSubmit = now;
            }
        }
    }

    void submit(const PreviewImage& image) {
        if (!image.pixels || image.width <= 0 || image.height <= 0) return;
        core::render::ImageFrame frame;
        frame.pixels = image.pixels;
        frame.width = static_cast<std::uint32_t>(image.width);
        frame.height = static_cast<std::uint32_t>(image.height);
        frame.stride = static_cast<std::uint32_t>(image.width) * 4u;
        frame.format = core::render::ImagePixelFormat::RGBA8;
        frame.sequence = ++sequence_;
        stream_->submit(std::move(frame));
        core::platform::requestUiUpdate();
    }

    std::string path_;
    std::shared_ptr<core::render::ImageStream> stream_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    bool started_ = false;
    std::atomic<bool> stop_{false};
    bool hasRequest_ = false;
    double requestTime_ = 0.0;
    std::uint64_t sequence_ = 0;
};

} // namespace

// --- player -------------------------------------------------------------------

struct Player::Impl {
    ~Impl() {
        close();
    }

    void open(const std::string& path) {
        close();
        path_ = path;
        {
            std::lock_guard lock(mutex_);
            status_ = Status{};
            status_.active = true;
            status_.name = std::filesystem::u8path(path).filename().u8string();
            status_.subtitlesEnabled = true;
            status_.volume = volume_;
            status_.muted = muted_;
            stream_ = std::make_shared<core::render::ImageStream>(3);
            lastShownPts_ = 0.0;
            seekRequest_ = -1.0;
            stepRequest_ = 0;
            videoTrackRequest_ = -1;
            audioTrack_ = 0;
            audioStartedTrack_ = 0;
            embeddedSubtitleCount_ = 0;
            subtitleTracks_.clear();
            externalSubtitles_.clear();
        }
        stop_ = false;
        paused_ = false;
        failed_ = false;
        {
            std::lock_guard audioLock(audioOpMutex_);
            audio_ = std::make_unique<AudioPlayer>();
            audio_->open(path_, 0, 0.0, false);
            audio_->setVolume(volume_);
            audio_->setMuted(muted_);
        }
        preview_ = std::make_unique<Previewer>(path);
        worker_ = std::thread([this, path] { run(path); });
        core::platform::requestUiUpdate();
    }

    void close() {
        stop_ = true;
        commandCv_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (preview_) preview_->close();
        {
            std::lock_guard audioLock(audioOpMutex_);
            if (audio_) audio_->close();
            audio_.reset();
        }
        preview_.reset();
        {
            std::lock_guard lock(mutex_);
            stream_.reset();
            status_ = Status{};
            externalSubtitles_.clear();
            subtitleTracks_.clear();
            embeddedSubtitleCount_ = 0;
            seekRequest_ = -1.0;
            stepRequest_ = 0;
            videoTrackRequest_ = -1;
            audioStartedTrack_ = 0;
        }
        core::platform::requestUiUpdate();
    }

    std::shared_ptr<core::render::ImageStream> stream() const {
        std::lock_guard lock(mutex_);
        return stream_;
    }

    std::shared_ptr<core::render::ImageStream> previewStream() const {
        std::lock_guard lock(mutex_);
        return preview_ ? preview_->stream() : nullptr;
    }

    Status status() const {
        std::lock_guard lock(mutex_);
        Status result = status_;
        result.hasAudio = audio_ && audio_->hasAudio();
        result.muted = audio_ ? audio_->muted() : muted_;
        result.volume = audio_ ? audio_->volume() : volume_;
        return result;
    }

    void togglePlayPause() {
        bool paused = false;
        bool restart = false;
        {
            std::lock_guard lock(mutex_);
            if (!status_.active || status_.failed) return;
            if (status_.ended) {
                // A click on a finished video restarts it from zero instead of
                // leaving the worker parked at EOF.
                status_.ended = false;
                status_.paused = false;
                status_.position = 0.0;
                seekRequest_ = 0.0;
                restart = true;
            } else {
                paused = !status_.paused;
                status_.paused = paused;
            }
        }
        paused_ = paused;
        audioPause(paused);
        if (restart) audioSeek(0.0);
        commandCv_.notify_all();
        core::platform::requestUiUpdate();
    }

    void seekBy(double seconds) {
        double target = 0.0;
        {
            std::lock_guard lock(mutex_);
            if (!status_.active || status_.failed) return;
            status_.ended = false;
            target = status_.position + seconds;
            if (status_.duration > 0.0) {
                target = std::clamp(target, 0.0, status_.duration);
            } else {
                target = std::max(0.0, target);
            }
            seekRequest_ = target;
        }
        audioSeek(target);
        commandCv_.notify_all();
        core::platform::requestUiUpdate();
    }

    void seekTo(double seconds) {
        {
            std::lock_guard lock(mutex_);
            if (!status_.active || status_.failed) return;
            status_.ended = false;
            if (status_.duration > 0.0) seconds = std::clamp(seconds, 0.0, status_.duration);
            else seconds = std::max(0.0, seconds);
            seekRequest_ = seconds;
        }
        audioSeek(seconds);
        commandCv_.notify_all();
        core::platform::requestUiUpdate();
    }

    void stepFrames(int delta) {
        if (delta == 0) return;
        {
            std::lock_guard lock(mutex_);
            if (!status_.active || status_.failed) return;
            status_.paused = true;
            status_.ended = false;
            stepRequest_ = std::clamp(stepRequest_ + delta, -30, 30);
        }
        paused_ = true;
        audioPause(true);
        commandCv_.notify_all();
        core::platform::requestUiUpdate();
    }

    void setVideoTrack(int track) {
        {
            std::lock_guard lock(mutex_);
            if (!status_.active || status_.failed) return;
            if (track < 0 || track >= status_.videoTrackCount ||
                track == status_.videoTrack) {
                return;
            }
            videoTrackRequest_ = track;
        }
        commandCv_.notify_all();
        core::platform::requestUiUpdate();
    }

    void setAudioTrack(int track) {
        int current = -1;
        int count = 0;
        {
            std::lock_guard lock(mutex_);
            if (!status_.active || status_.failed) return;
            current = status_.audioTrack;
            count = status_.audioTrackCount;
            if (track < 0 || track >= count || track == current) return;
            status_.audioTrack = track;
        }
        applyAudioTrack(track);
        core::platform::requestUiUpdate();
    }

    void setSubtitleTrack(int track) {
        {
            std::lock_guard lock(mutex_);
            if (track < 0 || track >= status_.subtitleTrackCount) return;
            status_.subtitleTrack = track;
            status_.subtitlesEnabled = true;
            refreshSubtitleLocked(lastShownPts_);
        }
        core::platform::requestUiUpdate();
    }

    void setVolume(float volume) {
        volume = std::clamp(volume, 0.0f, 1.0f);
        volume_ = volume;
        muted_ = false;
        {
            std::lock_guard lock(mutex_);
            status_.volume = volume;
            status_.muted = false;
        }
        audioSetMuted(false);
        audioSetVolume(volume);
        core::platform::requestUiUpdate();
    }

    void setMuted(bool muted) {
        muted_ = muted;
        {
            std::lock_guard lock(mutex_);
            status_.muted = muted;
        }
        audioSetMuted(muted);
        core::platform::requestUiUpdate();
    }

    void toggleMute() {
        bool muted = false;
        {
            std::lock_guard lock(mutex_);
            muted_ = !muted_;
            status_.muted = muted_;
            muted = muted_;
        }
        audioSetMuted(muted);
        core::platform::requestUiUpdate();
    }

    void toggleSubtitles() {
        {
            std::lock_guard lock(mutex_);
            if (!status_.hasSubtitles) return;
            status_.subtitlesEnabled = !status_.subtitlesEnabled;
            refreshSubtitleLocked(lastShownPts_);
        }
        core::platform::requestUiUpdate();
    }

    void requestPreview(double seconds) {
        std::lock_guard lock(mutex_);
        if (preview_) preview_->request(seconds);
    }

private:
    void audioPause(bool paused) {
        std::lock_guard lock(audioOpMutex_);
        if (audio_) audio_->setPaused(paused);
    }

    void audioSeek(double seconds) {
        std::lock_guard lock(audioOpMutex_);
        if (audio_) audio_->seek(seconds);
    }

    void audioSetVolume(float volume) {
        std::lock_guard lock(audioOpMutex_);
        if (audio_) audio_->setVolume(volume);
    }

    void audioSetMuted(bool muted) {
        std::lock_guard lock(audioOpMutex_);
        if (audio_) audio_->setMuted(muted);
    }

    // Audio track changes are rare, so restart the small audio device rather
    // than keeping a second codec-switching state machine. The audio worker is
    // joined by close(), and audioOpMutex_ serializes this with the video
    // worker's seek/pause calls.
    void applyAudioTrack(int track) {
        double position = 0.0;
        bool paused = false;
        {
            std::lock_guard lock(mutex_);
            position = status_.position;
            paused = status_.paused;
        }
        std::lock_guard audioLock(audioOpMutex_);
        if (!audio_) return;
        audio_->close();
        audio_->open(path_, track, position, paused);
        audio_->setVolume(volume_);
        audio_->setMuted(muted_);
        audioStartedTrack_ = track;
    }

    void fail(std::string error) {
        {
            std::lock_guard lock(mutex_);
            if (status_.error.empty()) status_.error = std::move(error);
            status_.failed = true;
            status_.paused = false;
        }
        paused_ = false;
        failed_ = true;
        audioPause(true);
        core::platform::requestUiUpdate();
    }

    void setOpened(const Decoder& decoder) {
        int defaultAudio = -1;
        {
            std::lock_guard lock(mutex_);
            status_.width = decoder.width();
            status_.height = decoder.height();
            status_.duration = decoder.duration();
            status_.videoTrackCount = decoder.videoTrackCount();
            status_.videoTrack = decoder.selectedVideoTrack();
            status_.audioTrackCount = decoder.audioTrackCount();
            defaultAudio = decoder.defaultAudioTrack();
            status_.audioTrack = defaultAudio;
            audioTrack_ = defaultAudio;
            embeddedSubtitleCount_ = decoder.subtitleTrackCount();
            const int totalSubtitles = embeddedSubtitleCount_ +
                                       static_cast<int>(externalSubtitles_.size());
            subtitleTracks_.resize(static_cast<std::size_t>(totalSubtitles));
            for (std::size_t index = 0; index < externalSubtitles_.size(); ++index) {
                std::vector<SubtitleEvent>& destination = subtitleTracks_[
                    static_cast<std::size_t>(embeddedSubtitleCount_) + index];
                destination = externalSubtitles_[index].events;
            }
            status_.subtitleTrackCount = totalSubtitles;
            if (totalSubtitles == 0) {
                status_.hasSubtitles = false;
                status_.subtitlesEnabled = false;
                status_.subtitleTrack = -1;
                status_.subtitle.clear();
            } else {
                int selected = decoder.defaultSubtitleTrack();
                if (selected < 0 || selected >= totalSubtitles) selected = 0;
                status_.hasSubtitles = true;
                status_.subtitlesEnabled = true;
                status_.subtitleTrack = selected;
                refreshSubtitleLocked(lastShownPts_);
            }
        }
        if (defaultAudio >= 0 && defaultAudio != audioStartedTrack_) {
            applyAudioTrack(defaultAudio);
        }
        core::platform::requestUiUpdate();
    }

    void onSubtitle(int track, double start, double end, std::string text) {
        std::lock_guard lock(mutex_);
        if (track < 0 || track >= static_cast<int>(subtitleTracks_.size())) return;
        auto& events = subtitleTracks_[static_cast<std::size_t>(track)];
        events.push_back({start, end, std::move(text)});
        while (events.size() > 5000) events.erase(events.begin());
        if (track == status_.subtitleTrack &&
            lastShownPts_ >= start && lastShownPts_ < end) {
            refreshSubtitleLocked(lastShownPts_);
        }
    }

    void refreshSubtitleLocked(double pts) {
        if (!status_.subtitlesEnabled || status_.subtitleTrack < 0 ||
            status_.subtitleTrack >= static_cast<int>(subtitleTracks_.size())) {
            status_.subtitle.clear();
            return;
        }
        std::string text;
        for (const auto& event : subtitleTracks_[static_cast<std::size_t>(
                 status_.subtitleTrack)]) {
            if (event.start <= pts && pts < event.end) text = event.text;
        }
        status_.subtitle = std::move(text);
    }

    void clearEmbeddedSubtitlesLocked() {
        for (int track = 0; track < embeddedSubtitleCount_ &&
                            track < static_cast<int>(subtitleTracks_.size()); ++track) {
            subtitleTracks_[static_cast<std::size_t>(track)].clear();
        }
        status_.subtitle.clear();
    }

    void pruneSubtitlesLocked(double pts) {
        const double cutoff = pts - 10.0;
        for (int track = 0; track < embeddedSubtitleCount_ &&
                            track < static_cast<int>(subtitleTracks_.size()); ++track) {
            auto& events = subtitleTracks_[static_cast<std::size_t>(track)];
            while (!events.empty() && events.front().end < cutoff) {
                events.erase(events.begin());
            }
        }
    }

    bool handleSeek(Decoder& decoder, double& dropUntil, bool& resetClock,
                    double& clockBase, Clock::time_point& clockStart,
                    double& lastShownPts) {
        double target = -1.0;
        {
            std::lock_guard lock(mutex_);
            if (seekRequest_ >= 0.0) {
                target = seekRequest_;
                seekRequest_ = -1.0;
            }
        }
        if (target < 0.0) return false;

        if (!decoder.seek(target)) {
            // A failed user seek should not stop a file that can still play.
            return true;
        }
        {
            std::lock_guard lock(mutex_);
            clearEmbeddedSubtitlesLocked();
        }
        dropUntil = target;
        resetClock = true;
        if (paused_) {
            Nv12Frame frame;
            Nv12Frame previous;
            double pts = 0.0;
            double previousPts = 0.0;
            bool havePrevious = false;
            bool submitted = false;
            for (int attempt = 0; attempt < 240 && !stop_; ++attempt) {
                const Decoder::NextResult result = decoder.next(frame, pts);
                if (result == Decoder::NextResult::End) break;
                if (result == Decoder::NextResult::Error) {
                    fail(decoder.error());
                    break;
                }
                if (pts + 0.001 < target) {
                    previous = frame;
                    previousPts = pts;
                    havePrevious = true;
                    continue;
                }
                if (!submit(frame, pts)) return true;
                lastShownPts = pts;
                clockBase = pts;
                clockStart = Clock::now();
                resetClock = false;
                submitted = true;
                break;
            }
            if (!submitted && havePrevious) {
                // Seeking at or past the end: show the last frame instead of
                // leaving the old position or jumping back to the start.
                if (!submit(previous, previousPts)) return true;
                lastShownPts = previousPts;
                clockBase = previousPts;
                clockStart = Clock::now();
                resetClock = false;
            }
        }
        return true;
    }

    bool performStep(Decoder& decoder, int delta, double& lastShownPts,
                     bool& resetClock, double& clockBase,
                     Clock::time_point& clockStart) {
        resetClock = true;
        double finalPts = lastShownPts;
        if (delta > 0) {
            for (int index = 0; index < delta && !stop_; ++index) {
                Nv12Frame frame;
                double pts = 0.0;
                Decoder::NextResult result = decoder.next(frame, pts);
                if (result == Decoder::NextResult::Error) {
                    fail(decoder.error());
                    return false;
                }
                if (result == Decoder::NextResult::End) break;
                if (!submit(frame, pts)) return false;
                finalPts = pts;
            }
        } else {
            {
                std::lock_guard lock(mutex_);
                clearEmbeddedSubtitlesLocked();
            }
            const double current = lastShownPts;
            for (int index = 0; index < -delta && !stop_; ++index) {
                const double threshold = std::max(0.0, finalPts - 0.001);
                if (threshold <= 0.0) break;
                if (!decoder.seek(std::max(0.0, threshold - 1.0))) return false;
                Nv12Frame previous;
                double previousPts = 0.0;
                bool havePrevious = false;
                for (int attempt = 0; attempt < 600 && !stop_; ++attempt) {
                    Nv12Frame frame;
                    double pts = 0.0;
                    const Decoder::NextResult result = decoder.next(frame, pts);
                    if (result == Decoder::NextResult::End) break;
                    if (result == Decoder::NextResult::Error) {
                        fail(decoder.error());
                        return false;
                    }
                    if (pts + 0.0005 >= threshold) break;
                    previous = frame;
                    previousPts = pts;
                    havePrevious = true;
                }
                if (!havePrevious) break;
                if (!submit(previous, previousPts)) return false;
                finalPts = previousPts;
            }
        }
        lastShownPts = finalPts;
        clockBase = finalPts;
        clockStart = Clock::now();
        audioSeek(finalPts);
        core::platform::requestUiUpdate();
        return true;
    }

    bool presentAt(double pts, bool& resetClock, double& clockBase,
                   Clock::time_point& clockStart) {
        if (resetClock) {
            clockBase = pts;
            clockStart = Clock::now();
            resetClock = false;
        }
        auto target = clockStart + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(pts - clockBase));

        for (;;) {
            if (stop_) return false;
            {
                std::lock_guard lock(mutex_);
                if (seekRequest_ >= 0.0 || stepRequest_ != 0 ||
                    videoTrackRequest_ >= 0) return false;
            }
            if (paused_) {
                std::unique_lock lock(mutex_);
                commandCv_.wait(lock, [this] {
                    return stop_.load() || !paused_.load() || seekRequest_ >= 0.0 ||
                           stepRequest_ != 0 || videoTrackRequest_ >= 0;
                });
                if (stop_ || seekRequest_ >= 0.0 || stepRequest_ != 0 ||
                    videoTrackRequest_ >= 0) return false;
                clockBase = pts;
                clockStart = Clock::now();
                target = clockStart;
            }
            const auto now = Clock::now();
            if (now >= target) break;
            const auto wait = std::min<Clock::duration>(
                target - now,
                std::chrono::duration_cast<Clock::duration>(
                    std::chrono::milliseconds(5)));
            std::unique_lock lock(mutex_);
            commandCv_.wait_for(lock, wait, [this] {
                return stop_.load() || paused_.load() || seekRequest_ >= 0.0 ||
                       stepRequest_ != 0;
            });
        }

        if (stop_) return false;
        {
            std::lock_guard lock(mutex_);
            if (seekRequest_ >= 0.0 || stepRequest_ != 0) return false;
        }
        if (paused_) {
            std::unique_lock lock(mutex_);
            commandCv_.wait(lock, [this] {
                return stop_.load() || !paused_.load() || seekRequest_ >= 0.0 ||
                       stepRequest_ != 0;
            });
            if (stop_ || seekRequest_ >= 0.0 || stepRequest_ != 0) return false;
            clockBase = pts;
            clockStart = Clock::now();
        }
        return true;
    }

    bool submit(const Nv12Frame& frame, double pts) {
        auto stream = this->stream();
        if (!stream) return false;
        core::render::ImageFrame image;
        image.pixels = frame.y;
        image.width = frame.width;
        image.height = frame.height;
        image.stride = frame.yStride;
        image.format = core::render::ImagePixelFormat::NV12;
        image.sequence = ++sequence_;
        image.plane1 = frame.uv;
        image.stride1 = frame.uvStride;
        image.colorSpace = frame.colorSpace;
        image.colorRange = frame.colorRange;
        if (!stream->submit(std::move(image))) {
            fail("cannot submit a video frame");
            return false;
        }
        {
            std::lock_guard lock(mutex_);
            status_.position = pts;
            status_.hasFrame = true;
            lastShownPts_ = pts;
            pruneSubtitlesLocked(pts);
            refreshSubtitleLocked(pts);
        }
        const auto now = Clock::now();
        if (now - lastUiUpdate_ >= std::chrono::milliseconds(200)) {
            lastUiUpdate_ = now;
            core::platform::requestUiUpdate();
        }
        return true;
    }

    void run(const std::string& path) {
        av_log_set_level(AV_LOG_ERROR);
        {
            std::vector<ExternalSubtitleTrack> loaded =
                loadExternalSubtitles(path, &stop_);
            std::lock_guard lock(mutex_);
            externalSubtitles_ = std::move(loaded);
        }
        Decoder decoder;
        if (!decoder.open(path, &stop_,
                          [this](int track, double start, double end,
                                 std::string text) {
                              onSubtitle(track, start, end, std::move(text));
                          })) {
            fail(decoder.error());
            return;
        }
        setOpened(decoder);

        bool resetClock = true;
        double clockBase = 0.0;
        Clock::time_point clockStart = Clock::now();
        double dropUntil = -1.0;
        double lastShownPts = 0.0;
        lastUiUpdate_ = Clock::now();

        while (!stop_ && !failed_) {
            int videoTrack = -1;
            int step = 0;
            {
                std::lock_guard lock(mutex_);
                videoTrack = videoTrackRequest_;
                videoTrackRequest_ = -1;
                step = stepRequest_;
                stepRequest_ = 0;
            }
            if (videoTrack >= 0) {
                const double target = lastShownPts;
                if (!decoder.selectVideoTrack(videoTrack, target)) {
                    fail(decoder.error());
                    break;
                }
                {
                    std::lock_guard lock(mutex_);
                    status_.videoTrack = decoder.selectedVideoTrack();
                    status_.width = decoder.width();
                    status_.height = decoder.height();
                    status_.ended = false;
                    seekRequest_ = target;
                }
                dropUntil = target;
                resetClock = true;
                continue;
            }
            if (step != 0) {
                if (!performStep(decoder, step, lastShownPts, resetClock,
                                 clockBase, clockStart)) {
                    break;
                }
                continue;
            }
            if (handleSeek(decoder, dropUntil, resetClock, clockBase, clockStart,
                           lastShownPts)) {
                continue;
            }
            if (paused_) {
                std::unique_lock lock(mutex_);
                commandCv_.wait(lock, [this] {
                    return stop_.load() || !paused_.load() || seekRequest_ >= 0.0 ||
                           stepRequest_ != 0 || videoTrackRequest_ >= 0;
                });
                resetClock = true;
                continue;
            }

            Nv12Frame frame;
            double pts = 0.0;
            const Decoder::NextResult result = decoder.next(frame, pts);
            if (result == Decoder::NextResult::End) {
                if (stop_) break;
                // Do not loop: pause on the final frame. Toggling play from
                // this state requests seek(0) in togglePlayPause().
                {
                    std::lock_guard lock(mutex_);
                    status_.paused = true;
                    status_.ended = true;
                    status_.position = status_.duration > 0.0
                        ? status_.duration : lastShownPts;
                }
                paused_ = true;
                audioPause(true);
                core::platform::requestUiUpdate();
                std::unique_lock lock(mutex_);
                commandCv_.wait(lock, [this] {
                    return stop_.load() || !paused_.load() ||
                           seekRequest_ >= 0.0 || stepRequest_ != 0 ||
                           videoTrackRequest_ >= 0;
                });
                resetClock = true;
                continue;
            }
            if (result == Decoder::NextResult::Error) {
                fail(decoder.error());
                break;
            }
            if (dropUntil >= 0.0 && pts + 0.001 < dropUntil) {
                continue;
            }
            dropUntil = -1.0;
            if (!presentAt(pts, resetClock, clockBase, clockStart)) {
                continue;
            }
            if (!submit(frame, pts)) break;
            lastShownPts = pts;
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable commandCv_;
    std::shared_ptr<core::render::ImageStream> stream_;
    std::unique_ptr<AudioPlayer> audio_;
    std::unique_ptr<Previewer> preview_;
    Status status_;
    std::string path_;
    std::vector<ExternalSubtitleTrack> externalSubtitles_;
    std::vector<std::vector<SubtitleEvent>> subtitleTracks_;
    int embeddedSubtitleCount_ = 0;
    int audioTrack_ = 0;
    int audioStartedTrack_ = 0;
    float volume_ = 0.5f;
    bool muted_ = false;
    int videoTrackRequest_ = -1;
    double lastShownPts_ = 0.0;
    double seekRequest_ = -1.0;
    int stepRequest_ = 0;
    std::mutex audioOpMutex_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> failed_{false};
    std::uint64_t sequence_ = 0;
    Clock::time_point lastUiUpdate_{};
    std::thread worker_;
};

Player::Player() : impl_(std::make_unique<Impl>()) {}
Player::~Player() = default;

void Player::open(const std::string& path) {
    impl_->open(path);
}

void Player::close() {
    impl_->close();
}

std::shared_ptr<core::render::ImageStream> Player::stream() const {
    return impl_->stream();
}

std::shared_ptr<core::render::ImageStream> Player::previewStream() const {
    return impl_->previewStream();
}

Status Player::status() const {
    return impl_->status();
}

void Player::togglePlayPause() {
    impl_->togglePlayPause();
}

void Player::seekBy(double seconds) {
    impl_->seekBy(seconds);
}

void Player::seekTo(double seconds) {
    impl_->seekTo(seconds);
}

void Player::stepFrames(int delta) {
    impl_->stepFrames(delta);
}

void Player::setVideoTrack(int track) {
    impl_->setVideoTrack(track);
}

void Player::setAudioTrack(int track) {
    impl_->setAudioTrack(track);
}

void Player::setSubtitleTrack(int track) {
    impl_->setSubtitleTrack(track);
}

void Player::setVolume(float volume) {
    impl_->setVolume(volume);
}

void Player::setMuted(bool muted) {
    impl_->setMuted(muted);
}

void Player::toggleMute() {
    impl_->toggleMute();
}

void Player::toggleSubtitles() {
    impl_->toggleSubtitles();
}

void Player::requestPreview(double seconds) {
    impl_->requestPreview(seconds);
}

} // namespace imview::video
