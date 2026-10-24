// DS5Dummy — 落盘 sink 单测（tools/dump_sink.cpp）
//
// 离线回听这条路的另一半：给定若干段（触觉 64B + 喇叭 200B 两个指针，与
// audio::OutputSink 交出来的形状一致），检查落下来的 Ogg Opus 与 WAV 在字节层面
// 自洽——页结构、granule 累积、Ogg CRC、WAV 两个长度字段的回填、int8→PCM U8 的翻符号位。
// 这里的 Ogg CRC 按规范独立实现了一遍，不复用被测代码那张表：实现错了才要测得出。

#include "tools/dump_sink.h"

#include "audio/audio_encoder.h"
#include "core/protocol/audio_format.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr size_t kOggHeaderSize = 27;
constexpr uint32_t kExpectedSerial = 0x054c0ce6;

std::string dump_base(const std::string& name) {
    return ::testing::TempDir() + "ds5dummy_dump_" + name;
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> out(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(out.data()), size);
    return out;
}

// 触觉与喇叭是两个独立缓冲，每字节等于 (下标 + seed) 模 256：任何错位、漏写、
// 串道都会直接暴露在断言里，段与段之间靠 seed 区分。
std::array<uint8_t, core::protocol::kHapticsSize> make_haptics(uint8_t seed) {
    std::array<uint8_t, core::protocol::kHapticsSize> h{};
    for (size_t i = 0; i < h.size(); ++i) h[i] = static_cast<uint8_t>((i + seed) % 256);
    return h;
}

std::array<uint8_t, core::protocol::kSpeakerSize> make_speaker(uint8_t seed) {
    std::array<uint8_t, core::protocol::kSpeakerSize> s{};
    for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<uint8_t>((i + seed) % 256);
    return s;
}

uint16_t le_u16(const std::vector<uint8_t>& b, size_t at) {
    return static_cast<uint16_t>(b[at] | (b[at + 1] << 8));
}

uint32_t le_u32(const std::vector<uint8_t>& b, size_t at) {
    return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
           (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}

uint64_t le_u64(const std::vector<uint8_t>& b, size_t at) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[at + i]) << (8 * i);
    return v;
}

// Ogg 规范版 CRC32：poly 0x04C11DB7、不反射、初值 0、无末异或，逐位算（不用表）。
uint32_t ogg_crc(const uint8_t* data, size_t length) {
    uint32_t crc = 0;
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000u) != 0u ? ((crc << 1) ^ 0x04C11DB7u) : (crc << 1);
        }
    }
    return crc;
}

struct OggPage {
    uint8_t header_type = 0;
    uint64_t granule = 0;
    uint32_t serial = 0;
    uint32_t seq = 0;
    uint32_t crc = 0;
    size_t segments = 0;               ///< segment table 条目数，即本页包数
    std::string magic;                 ///< 载荷前 8 字节，OpusHead/OpusTags 用
    std::vector<uint8_t> payload;
    size_t total = 0;                  ///< 本页占的字节数
    size_t next = 0;                   ///< 下一页偏移
};

// 解析 offset 处的一页，并把页内 CRC 字段清零后重算校验。
bool parse_page(const std::vector<uint8_t>& buf, size_t offset, OggPage& page) {
    if (offset + kOggHeaderSize > buf.size()) return false;
    if (std::memcmp(&buf[offset], "OggS", 4) != 0) return false;
    if (buf[offset + 4] != 0) return false;  // stream structure version

    const auto base = buf.begin() + static_cast<std::ptrdiff_t>(offset);
    page.header_type = buf[offset + 5];
    page.granule = le_u64(buf, offset + 6);
    page.serial = le_u32(buf, offset + 14);
    page.seq = le_u32(buf, offset + 18);
    page.crc = le_u32(buf, offset + 22);

    page.segments = buf[offset + 26];
    if (offset + kOggHeaderSize + page.segments > buf.size()) return false;
    size_t payload_size = 0;
    for (size_t i = 0; i < page.segments; ++i) payload_size += buf[offset + kOggHeaderSize + i];

    page.total = kOggHeaderSize + page.segments + payload_size;
    if (offset + page.total > buf.size()) return false;

    const auto data_begin = base + static_cast<std::ptrdiff_t>(kOggHeaderSize + page.segments);
    page.payload.assign(data_begin, base + static_cast<std::ptrdiff_t>(page.total));
    page.magic.assign(page.payload.begin(),
                      page.payload.begin() +
                          static_cast<std::ptrdiff_t>(page.payload.size() < 8 ? page.payload.size() : 8));
    page.next = offset + page.total;

    std::vector<uint8_t> for_crc(buf.begin() + static_cast<std::ptrdiff_t>(offset),
                                 buf.begin() + static_cast<std::ptrdiff_t>(offset + page.total));
    for (size_t i = 22; i < 26; ++i) for_crc[i] = 0;
    return ogg_crc(for_crc.data(), for_crc.size()) == page.crc;
}

class DumpSinkTest : public ::testing::Test {
protected:
    void TearDown() override {
        tools::dump::close();
        for (const std::string& path : leftovers) {
            std::remove(path.c_str());
        }
    }

    std::vector<std::string> leftovers;

    // 返回 <prefix>.opus 与 <prefix>.haptics.wav 两个路径，并登记待清理。
    std::string use_prefix(const std::string& name) {
        const std::string base = dump_base(name);
        leftovers.push_back(base + ".opus");
        leftovers.push_back(base + ".haptics.wav");
        return base;
    }
};

}  // namespace

TEST_F(DumpSinkTest, EmptyPrefixMeansNoFile) {
    EXPECT_FALSE(tools::dump::open(""));
    const auto haptics = make_haptics(0);
    const auto speaker = make_speaker(0);
    // 未 open：空操作，不建文件也不崩。两个指针任一为 null 同样不写。
    tools::dump::write(haptics.data(), speaker.data());
    tools::dump::write(nullptr, speaker.data());
    tools::dump::write(haptics.data(), nullptr);
    tools::dump::close();

    const std::string opus_path = dump_base("empty") + ".opus";
    leftovers.push_back(opus_path);
    EXPECT_TRUE(read_file(opus_path).empty());
}

TEST_F(DumpSinkTest, OggPagesHeadTagsPacketsAndEos) {
    const std::string base = use_prefix("opus");
    ASSERT_TRUE(tools::dump::open(base));

    const auto h1 = make_haptics(0);
    const auto s1 = make_speaker(0);
    const auto h2 = make_haptics(7);
    const auto s2 = make_speaker(7);
    tools::dump::write(h1.data(), s1.data());
    tools::dump::write(h2.data(), s2.data());
    tools::dump::close();

    const std::vector<uint8_t> buf = read_file(base + ".opus");
    ASSERT_FALSE(buf.empty());

    OggPage head;
    ASSERT_TRUE(parse_page(buf, 0, head));
    EXPECT_EQ(head.seq, 0u);
    EXPECT_EQ(head.header_type, 0x02);  // BOS
    EXPECT_EQ(head.granule, 0u);
    EXPECT_EQ(head.serial, kExpectedSerial);
    EXPECT_EQ(head.magic, "OpusHead");
    ASSERT_EQ(head.payload.size(), 19u);
    EXPECT_EQ(head.payload[8], 1);  // version
    EXPECT_EQ(head.payload[9], audio::encoder::kEncodedChannels);
    EXPECT_EQ(le_u32(head.payload, 12), core::protocol::kOpusSampleRate);
    EXPECT_EQ(head.payload[18], 0);  // channel mapping family

    OggPage tags;
    ASSERT_TRUE(parse_page(buf, head.next, tags));
    EXPECT_EQ(tags.seq, 1u);
    EXPECT_EQ(tags.magic, "OpusTags");

    // 每个 Opus 包独占一页，granule 按一包 480 帧累积。
    OggPage packet1;
    ASSERT_TRUE(parse_page(buf, tags.next, packet1));
    EXPECT_EQ(packet1.seq, 2u);
    EXPECT_EQ(packet1.header_type, 0);
    EXPECT_EQ(packet1.segments, 1u);  // 一页一包
    EXPECT_EQ(packet1.granule, core::protocol::kOpusFrameSamples);
    ASSERT_EQ(packet1.payload.size(), core::protocol::kSpeakerSize);
    for (size_t i = 0; i < packet1.payload.size(); ++i) {
        EXPECT_EQ(packet1.payload[i], s1[i]);
    }

    OggPage packet2;
    ASSERT_TRUE(parse_page(buf, packet1.next, packet2));
    EXPECT_EQ(packet2.granule, core::protocol::kOpusFrameSamples * 2);
    ASSERT_EQ(packet2.payload.size(), core::protocol::kSpeakerSize);
    for (size_t i = 0; i < packet2.payload.size(); ++i) {
        EXPECT_EQ(packet2.payload[i], s2[i]);
    }

    OggPage eos;
    ASSERT_TRUE(parse_page(buf, packet2.next, eos));
    EXPECT_EQ(eos.header_type, 0x04);  // EOS
    // 空页必须是 segC=0。写成一条 lacing=0 是"一个 0 字节的包"，
    // ffmpeg 会报 Packet processing failed: Invalid data found。
    EXPECT_EQ(eos.segments, 0u);
    EXPECT_TRUE(eos.payload.empty());
    // 收尾页的 granule 是全流累计样本数，不是 0——否则时长算成空。
    EXPECT_EQ(eos.granule, core::protocol::kOpusFrameSamples * 2);
    EXPECT_EQ(eos.next, buf.size());   // 最后一页正好把文件读完
}

TEST_F(DumpSinkTest, WavHeaderSizesPatchedAndHapticsBounded) {
    const std::string base = use_prefix("wav");
    ASSERT_TRUE(tools::dump::open(base));

    const auto haptics = make_haptics(0);
    const auto speaker = make_speaker(0);
    tools::dump::write(haptics.data(), speaker.data());
    tools::dump::write(haptics.data(), speaker.data());
    tools::dump::close();

    const std::vector<uint8_t> buf = read_file(base + ".haptics.wav");
    const size_t data_bytes = core::protocol::kHapticsSize * 2;
    ASSERT_EQ(buf.size(), 44u + data_bytes);

    EXPECT_EQ(std::memcmp(buf.data(), "RIFF", 4), 0);
    EXPECT_EQ(le_u32(buf, 4), static_cast<uint32_t>(36u + data_bytes));  // close 时回填
    EXPECT_EQ(std::memcmp(&buf[8], "WAVE", 4), 0);
    EXPECT_EQ(std::memcmp(&buf[12], "fmt ", 4), 0);
    EXPECT_EQ(le_u32(buf, 16), 16u);
    EXPECT_EQ(le_u16(buf, 20), 1u);  // PCM
    EXPECT_EQ(le_u16(buf, 22), audio::encoder::kEncodedChannels);
    EXPECT_EQ(le_u32(buf, 24), core::protocol::kHapticsSampleRate);
    EXPECT_EQ(le_u32(buf, 28), core::protocol::kHapticsSampleRate * 2u);  // byte rate
    EXPECT_EQ(le_u16(buf, 32), 2u);                                       // block align
    EXPECT_EQ(le_u16(buf, 34), 8u);                                       // bits per sample
    EXPECT_EQ(std::memcmp(&buf[36], "data", 4), 0);
    EXPECT_EQ(le_u32(buf, 40), static_cast<uint32_t>(data_bytes));        // close 时回填

    // int8 位模式 → WAV 的 PCM U8：翻符号位，等价于精确 +128，不改数值。
    // 两次写入内容相同，所以第二半也该逐字节相同——顺带证明两遍没有串道。
    for (size_t i = 0; i < core::protocol::kHapticsSize; ++i) {
        EXPECT_EQ(buf[44 + i], static_cast<uint8_t>(haptics[i] ^ 0x80));
        EXPECT_EQ(buf[44 + core::protocol::kHapticsSize + i],
                  static_cast<uint8_t>(haptics[i] ^ 0x80));
    }
}
