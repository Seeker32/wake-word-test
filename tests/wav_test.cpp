#include "../main/wav.h"

#include <cassert>
#include <fstream>
#include <iterator>
#include <vector>

int main() {
    std::ifstream input("voice_file/wake_up.wav", std::ios::binary);
    const std::vector<uint8_t> wav{std::istreambuf_iterator<char>{input}, {}};
    const auto prompt = ParsePcm16MonoWave(wav);
    assert(prompt.sample_rate == 24000);
    assert(prompt.samples.size() == 45600);
    assert(ParsePcm16MonoWave(std::span(wav).first(11)).samples.empty());
    assert(ParsePcm16MonoWave(std::span(wav).first(wav.size() - 1)).samples.empty());

    auto unsupported = wav;
    unsupported[20] = 3;  // IEEE float instead of PCM.
    assert(ParsePcm16MonoWave(unsupported).samples.empty());
}
