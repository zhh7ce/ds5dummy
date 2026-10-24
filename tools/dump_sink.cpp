/**
 * @file dump_sink.cpp
 * @brief Ogg Opus + WAV 落盘实现（离线回听验收用）。
 * @details Ogg 封装的写法源自 DS5Server origin/main:src/opus_output.{h,cpp}，
 *          实现为 `tools::dump` namespace + loguru + 编译期 CRC 表；约束是
 *          "产出可被通用播放器直接读"这个行为，不是与参考同字节。
 */

#include "tools/dump_sink.h"

#include "audio/audio_encoder.h"
#include "core/debug.h"
#include "core/protocol/audio_format.h"
#include "loguru.hpp"

#include <array>
#include <fstream>
#include <vector>

namespace tools {
namespace dump {
namespace {

/**
 * @brief Ogg 页头长度（不含 segment table）。
 */
constexpr size_t kOggHeaderSize = 27;
constexpr uint8_t kOggFlagBos = 0x02;  ///< 首包页（beginning of stream）
constexpr uint8_t kOggFlagEos = 0x04;  ///< 结束页
// 上游拿手柄的 vendor:product 当 Ogg serial number，纯标识用途，沿用。
constexpr uint32_t kOggSerial = 0x054c0ce6;
constexpr size_t kOggMaxLacing = 255;  ///< 一条 lacing 值能表达的最大字节数

constexpr size_t kWavHeaderSize = 44;                 ///< 规范 44 字节：RIFF+fmt +data 头
constexpr uint64_t kWavRiffSizeOffset = 4;            ///< RIFF chunk size 字段偏移
constexpr uint64_t kWavDataSizeOffset = 40;           ///< data chunk size 字段偏移
constexpr uint8_t kWavSignedToUnsigned = 0x80;        ///< int8 → WAV 的 PCM U8：翻符号位

/**
 * @brief Ogg 用的 CRC32 表：多项式 0x04C11DB7、不反射、初值 0、无末异或。
 * @return 256 项表。
 * @warning 与 `core::protocol::Crc32`（手柄报告用的反射多项式那套）不是同一个函数，
 *          不能复用——混用会让播放器整页判废，而且不报错，只是听不见。
 */
constexpr std::array<uint32_t, 256> build_ogg_crc_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t r = i << 24;
        for (int bit = 0; bit < 8; ++bit) {
            r = (r & 0x80000000u) != 0u ? ((r << 1) ^ 0x04C11DB7u) : (r << 1);
        }
        table[i] = r;
    }
    return table;
}

constexpr std::array<uint32_t, 256> kOggCrcTable = build_ogg_crc_table();

uint32_t ogg_crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0;
    for (size_t i = 0; i < length; ++i) {
        crc = (crc << 8) ^ kOggCrcTable[((crc >> 24) ^ data[i]) & 0xFFu];
    }
    return crc;
}

/** @brief 两个落盘目标 + Ogg 页计数状态；只在编码线程写、主线程开关。 */
struct State {
    std::ofstream opus;            ///< `<prefix>.opus`
    std::ofstream wav;             ///< `<prefix>.haptics.wav`
    uint32_t page_sequence = 0;    ///< 两个头页 + 每包一页，共用一个序号
    uint64_t granule = 0;          ///< 累计样本数，每包步进 kOpusFrameSamples
    uint64_t wav_bytes = 0;        ///< data chunk 已写字节数，close 时回填进头
} st;

void append_le(std::vector<uint8_t>& out, uint64_t value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFFu));
    }
}

void write_le(std::ofstream& f, uint64_t value, size_t bytes) {
    std::vector<uint8_t> raw;
    append_le(raw, value, bytes);
    f.write(reinterpret_cast<const char*>(raw.data()),
            static_cast<std::streamsize>(raw.size()));
}

/**
 * @brief 写一整页：27 字节头 + segment table + 载荷，CRC 覆盖整页。
 * @param page 目标文件。
 * @param data 载荷首地址，size 为 0 时可为 nullptr。
 * @param size 载荷字节数。
 * @param granule_pos 本页 granule position。
 * @param header_type Ogg headerType 标志位（BOS/EOS/0）。
 * @details 先按 CRC 字段清零算校验，再写回偏移 22——顺序不能颠倒。
 */
void write_page(std::ofstream& page, const uint8_t* data, size_t size,
                uint64_t granule_pos, uint8_t header_type) {
    // Ogg 的 lacing 规则：每段满 255 才算完整，末尾必须再跟一条不满值（可为 0），
    // 所以 size/255 + 1 天然带上那条终止值。但空页是另一回事：segC=1、lacing=0
    // 表示"这里有一个 0 字节的包"，播放器会当畸形包判废（EOS 页正是空页）。
    const size_t num_segments = size == 0 ? 0 : size / kOggMaxLacing + 1;

    std::vector<uint8_t> buf;
    buf.reserve(kOggHeaderSize + num_segments + size);
    const char magic[4] = {'O', 'g', 'g', 'S'};
    for (const char c : magic) buf.push_back(static_cast<uint8_t>(c));
    buf.push_back(0);  // stream structure version
    buf.push_back(header_type);
    append_le(buf, granule_pos, 8);
    append_le(buf, kOggSerial, 4);
    append_le(buf, st.page_sequence, 4);
    ++st.page_sequence;
    append_le(buf, 0, 4);  // CRC 占位，算完再回填
    buf.push_back(static_cast<uint8_t>(num_segments));

    size_t remaining = size;
    for (size_t i = 0; i < num_segments; ++i) {
        const size_t lacing = remaining > kOggMaxLacing ? kOggMaxLacing : remaining;
        buf.push_back(static_cast<uint8_t>(lacing));
        remaining -= lacing;
    }
    if (size > 0) buf.insert(buf.end(), data, data + size);

    const uint32_t crc = ogg_crc32(buf.data(), buf.size());
    for (size_t i = 0; i < 4; ++i) {
        buf[22 + i] = static_cast<uint8_t>((crc >> (8 * i)) & 0xFFu);
    }

    page.write(reinterpret_cast<const char*>(buf.data()),
               static_cast<std::streamsize>(buf.size()));
}

/** @brief OpusHead：版本 1、通道数、pre-skip、输入采样率、增益、mapping family 0。 */
void write_opus_head() {
    std::vector<uint8_t> head;
    const char id[8] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
    for (const char c : id) head.push_back(static_cast<uint8_t>(c));
    head.push_back(1);                                    // version
    head.push_back(static_cast<uint8_t>(audio::encoder::kEncodedChannels));
    append_le(head, 0, 2);                                // pre-skip：上游写 0
    append_le(head, core::protocol::kOpusSampleRate, 4);
    append_le(head, 0, 2);                                // output gain 0 dB
    head.push_back(0);                                    // mapping family 0
    write_page(st.opus, head.data(), head.size(), 0, kOggFlagBos);
}

/** @brief OpusTags：vendor 串 + 零条 user comment。 */
void write_opus_tags() {
    const std::string vendor = "ds5dummy audio::dump";
    std::vector<uint8_t> tags;
    const char id[8] = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
    for (const char c : id) tags.push_back(static_cast<uint8_t>(c));
    append_le(tags, vendor.size(), 4);
    for (const char c : vendor) tags.push_back(static_cast<uint8_t>(c));
    append_le(tags, 0, 4);  // user comment list length
    write_page(st.opus, tags.data(), tags.size(), 0, 0);
}

/** @brief 写 44 字节 WAV 头，两个长度字段先填 0，close 时回填。 */
void write_wav_header() {
    std::vector<uint8_t> hdr;
    const char riff[4] = {'R', 'I', 'F', 'F'};
    for (const char c : riff) hdr.push_back(static_cast<uint8_t>(c));
    append_le(hdr, 0, 4);  // RIFF chunk size = 36 + data，close 回填
    const char wave[4] = {'W', 'A', 'V', 'E'};
    for (const char c : wave) hdr.push_back(static_cast<uint8_t>(c));
    const char fmt[4] = {'f', 'm', 't', ' '};
    for (const char c : fmt) hdr.push_back(static_cast<uint8_t>(c));
    append_le(hdr, 16, 4);                              // fmt chunk size
    append_le(hdr, 1, 2);                               // PCM
    append_le(hdr, audio::encoder::kEncodedChannels, 2);
    append_le(hdr, core::protocol::kHapticsSampleRate, 4);
    // byte rate = 采样率 × 块大小；触觉是一帧一个字节 × 两路。
    append_le(hdr, core::protocol::kHapticsSampleRate * audio::encoder::kEncodedChannels, 4);
    append_le(hdr, audio::encoder::kEncodedChannels, 2);       // block align
    append_le(hdr, 8, 2);                                // bits per sample
    const char data[4] = {'d', 'a', 't', 'a'};
    for (const char c : data) hdr.push_back(static_cast<uint8_t>(c));
    append_le(hdr, 0, 4);  // data chunk size，close 回填
    st.wav.write(reinterpret_cast<const char*>(hdr.data()),
                 static_cast<std::streamsize>(hdr.size()));
}

}  // namespace

bool open(const std::string& path_prefix) {
    if (path_prefix.empty()) return false;

    close();  // 重开：残留状态（页序号/granule/字节数）一律归零

    const std::string opus_path = path_prefix + ".opus";
    const std::string wav_path = path_prefix + ".haptics.wav";

    st.opus.open(opus_path, std::ios::binary | std::ios::trunc);
    if (!st.opus.is_open()) {
        DS5DUMMY_LOG(ERROR, "[dump] cannot open {}", opus_path);
        return false;
    }
    st.wav.open(wav_path, std::ios::binary | std::ios::trunc);
    if (!st.wav.is_open()) {
        DS5DUMMY_LOG(ERROR, "[dump] cannot open {}", wav_path);
        st.opus.close();
        return false;
    }

    st.page_sequence = 0;
    st.granule = 0;
    st.wav_bytes = 0;

    write_opus_head();
    write_opus_tags();
    write_wav_header();

    DS5DUMMY_LOG(INFO, "[dump] writing {} + {}", opus_path, wav_path);
    return true;
}

void write(const uint8_t* haptics, const uint8_t* speaker) {
    if (!haptics || !speaker || (!st.opus.is_open() && !st.wav.is_open())) return;

    // 触觉段：int8 位模式原样搬到 WAV 的 PCM U8，翻符号位即精确 +128，无量化损失。
    if (st.wav.is_open()) {
        std::vector<uint8_t> pcm(core::protocol::kHapticsSize);
        for (size_t i = 0; i < core::protocol::kHapticsSize; ++i) {
            pcm[i] = static_cast<uint8_t>(haptics[i] ^ kWavSignedToUnsigned);
        }
        st.wav.write(reinterpret_cast<const char*>(pcm.data()),
                     static_cast<std::streamsize>(pcm.size()));
        st.wav_bytes += pcm.size();
    }

    // 喇叭段：一次 opus_encode_float 的产出，一整页一包。
    if (st.opus.is_open()) {
        st.granule += core::protocol::kOpusFrameSamples;
        write_page(st.opus, speaker, core::protocol::kSpeakerSize, st.granule, 0);
    }

    if (!st.opus.good() || !st.wav.good()) {
        DS5DUMMY_LOG(WARNING, "[dump] write failed, dump disabled");
        close();
    }
}

void close() {
    if (st.opus.is_open()) {
        // EOS 页的 granule 必须是全流累计样本数：写 0 播放器会把时长算成空。
        write_page(st.opus, nullptr, 0, st.granule, kOggFlagEos);
        st.opus.close();
    }
    if (st.wav.is_open()) {
        // 长度字段只能在收尾时补：WAV 的 RIFF size 要含整个 fmt+data。
        st.wav.seekp(static_cast<std::streamoff>(kWavRiffSizeOffset), std::ios::beg);
        write_le(st.wav, kWavHeaderSize - 8 + st.wav_bytes, 4);
        st.wav.seekp(static_cast<std::streamoff>(kWavDataSizeOffset), std::ios::beg);
        write_le(st.wav, st.wav_bytes, 4);
        st.wav.close();
    }
    st.page_sequence = 0;
    st.granule = 0;
    st.wav_bytes = 0;
}

}  // namespace dump
}  // namespace tools
