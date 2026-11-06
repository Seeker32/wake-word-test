#pragma once

#include <cstdint>
#include <cstring>
#include <span>

struct PcmWave {
    uint32_t sample_rate = 0;
    std::span<const uint8_t> samples;
};

inline PcmWave ParsePcm16MonoWave(std::span<const uint8_t> file) {
    const auto read_u16 = [](const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); };
    const auto read_u32 = [](const uint8_t *p) {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    };
    if (file.size() < 12 || std::memcmp(file.data(), "RIFF", 4) || std::memcmp(file.data() + 8, "WAVE", 4)) return {};
    const size_t riff_bytes = read_u32(file.data() + 4);
    if (riff_bytes < 4 || riff_bytes > file.size() - 8) return {};

    uint32_t sample_rate = 0;
    auto chunks = file.subspan(12, riff_bytes - 4);
    while (chunks.size() >= 8) {
        const size_t chunk_bytes = read_u32(chunks.data() + 4);
        if (chunk_bytes > chunks.size() - 8 || chunk_bytes % 2 > chunks.size() - 8 - chunk_bytes) return {};
        const auto body = chunks.subspan(8, chunk_bytes);
        if (std::memcmp(chunks.data(), "fmt ", 4) == 0) {
            if (body.size() < 16 || read_u16(body.data()) != 1 || read_u16(body.data() + 2) != 1 ||
                read_u16(body.data() + 12) != 2 || read_u16(body.data() + 14) != 16) return {};
            sample_rate = read_u32(body.data() + 4);
        } else if (std::memcmp(chunks.data(), "data", 4) == 0) {
            if (!sample_rate || body.empty() || body.size() % 2) return {};
            return {sample_rate, body};
        }
        chunks = chunks.subspan(8 + chunk_bytes + chunk_bytes % 2);
    }
    return {};
}
