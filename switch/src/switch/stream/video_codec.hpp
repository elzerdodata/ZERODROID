#pragma once

#include <cstddef>
#include <cstdint>

namespace gnx::stream {

// Video codec carried by the Boosteroid stream. This client speaks H.265/HEVC
// end to end: the gateway handshake requests "h265", the WebRTC offer
// advertises H265/90000 (payload type 104) and the jitter buffer depacketizes
// RFC 7798.
enum class VideoCodec { H264, H265 };

// What the user picked in Settings -> Advanced -> Codec. Auto prefers H.265
// and falls back to H.264 when the HEVC decoder or the stream fails.
enum class CodecPreference { Auto, H265, H264 };

// What the user picked in Settings -> Advanced -> Transport.
// Auto prefers Boosteroid's native UDP transport (proto: 2, Reed-Solomon FEC).
// NativeUdp forces proto: 2, and WebRtc forces proto: 1 (libpeer WebRTC).
enum class TransportPreference { Auto = 0, NativeUdp = 1, WebRtc = 2 };

// Settings -> Advanced -> Upscaler. FSR 1 runs AMD FidelityFX Super Resolution
// 1 on the decoded frame: EASU when the stream is smaller than the display and
// always the RCAS sharpening pass, so a low-bitrate stream stays usable even in
// handheld mode (720p panel) where the frame is presented 1:1.
enum class UpscalerPreference { Off = 0, Fsr1 = 1 };

inline const char* video_codec_label(VideoCodec codec) {
    return codec == VideoCodec::H265 ? "H.265" : "H.264";
}

// NAL unit type of a depacketized Annex-B unit. H.264 uses the low 5 bits of a
// 1-byte header (RFC 6184); HEVC uses 6 bits shifted past the F bit of a
// 2-byte header (RFC 7798 / ITU-T H.265 Table 7-1).
inline uint8_t nal_unit_type(const uint8_t* data, size_t size,
                             VideoCodec codec) {
    if (size == 0) return 0;
    return codec == VideoCodec::H265
               ? static_cast<uint8_t>((data[0] >> 1) & 0x3f)
               : static_cast<uint8_t>(data[0] & 0x1f);
}

// H.264 IDR slice (RFC 6184).
constexpr uint8_t kH264NalIdr = 5;
// HEVC aggregation packet and fragmentation unit (RFC 7798).
constexpr uint8_t kHevcNalAp = 48;
constexpr uint8_t kHevcNalFu = 49;

// HEVC IRAP pictures: BLA_W_LP (16) .. CRA_NUT (21). These -- not the VPS/SPS/
// PPS parameter sets (32..34) -- are the slices that rebuild the reference
// chain, exactly like an H.264 IDR slice.
inline bool is_random_access_nal(uint8_t nal_type, VideoCodec codec) {
    return codec == VideoCodec::H265 ? nal_type >= 16 && nal_type <= 21
                                     : nal_type == kH264NalIdr;
}

// True if a complete Annex-B access unit contains a random-access picture:
// H.264 IDR or any HEVC IRAP. Parameter sets alone do not count -- resuming
// decode on them would feed predictive frames whose references were never
// decoded, which the hardware decoder conceals with garbage.
inline bool has_random_access_picture(const uint8_t* data, size_t size,
                                      VideoCodec codec) {
    for (size_t i = 0; i + 3 < size; ++i) {
        size_t start_code = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            start_code = 3;
        } else if (i + 4 < size && data[i] == 0 && data[i + 1] == 0 &&
                   data[i + 2] == 0 && data[i + 3] == 1) {
            start_code = 4;
        }
        if (!start_code) continue;
        if (i + start_code < size &&
            is_random_access_nal(nal_unit_type(data + i + start_code,
                                               size - i - start_code, codec),
                                 codec)) {
            return true;
        }
        i += start_code - 1;
    }
    return false;
}

}  // namespace gnx::stream
