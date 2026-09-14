// test_speech_shaper.cpp - SpeechShaper (speech_shaper.hpp) on synthetic
// audio shaped like the firmware's: a held DAC, a faint attack, loud speech,
// and the pipeline going idle part-way through a block.
#include "speech_shaper.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr size_t kBlock = 250;

struct Block {
    std::vector<int16_t> samples;
    bool idle;
};

std::vector<int16_t> zeros(size_t n) {
    return std::vector<int16_t>(n, 0);
}

// Starts off zero, so it always moves away from a preceding hold at 0.
std::vector<int16_t> loud(size_t n, double phase) {
    std::vector<int16_t> out(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<int16_t>(std::lround(5000.0 * std::sin(phase + 1.0 + 0.3 * i)));
    }
    return out;
}

// Below kSpeechLevel but moving: a fricative's first milliseconds.
std::vector<int16_t> faint(size_t n) {
    std::vector<int16_t> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = (i % 2 == 0) ? -16 : 0;
    return out;
}

std::vector<int16_t> cat(std::initializer_list<std::vector<int16_t>> parts) {
    std::vector<int16_t> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

std::vector<int16_t> run(dtc01::SpeechShaper& shaper, const std::vector<Block>& blocks) {
    std::vector<int16_t> emit;
    for (const Block& b : blocks) {
        shaper.add(b.samples.data(), b.samples.size(), b.idle, emit);
    }
    shaper.finish(emit);
    return emit;
}

// The firmware's shape: 1 s idle and holding, 25 ms of DSP-generated silence,
// a faint attack, loud speech, then idle from part-way through a block.
std::vector<Block> utterance() {
    std::vector<Block> b;
    for (int i = 0; i < 4; ++i) b.push_back({zeros(kBlock), true});      //    0-1000
    b.push_back({zeros(kBlock), false});                                  // 1000-1250
    b.push_back({cat({zeros(50), faint(200)}), false});                   // 1250-1500
    b.push_back({loud(kBlock, 0.0), false});                              // 1500-1750
    b.push_back({loud(kBlock, 1.0), false});                              // 1750-2000
    b.push_back({cat({loud(100, 2.0), zeros(150)}), true});               // 2000-2250
    for (int i = 0; i < 4; ++i) b.push_back({zeros(kBlock), true});      // 2250-3250
    return b;
}

std::vector<int16_t> flatten(const std::vector<Block>& blocks) {
    std::vector<int16_t> out;
    for (const Block& b : blocks) out.insert(out.end(), b.samples.begin(), b.samples.end());
    return out;
}

std::vector<int16_t> slice(const std::vector<int16_t>& v, size_t from, size_t to) {
    return std::vector<int16_t>(v.begin() + from, v.begin() + to);
}

}  // namespace

int wmain() {
    const std::vector<Block> blocks = utterance();
    const std::vector<int16_t> stream = flatten(blocks);

    // First speech of a call: everything before the faint attack goes except
    // a 5 ms pad of the hold; after the last sound, the held idle audio is kept
    // only up to the last moving sample (the zero that ends the loud run).
    {
        dtc01::SpeechShaper shaper(true);
        const auto got = run(shaper, blocks);
        assert(shaper.speech_started());
        assert(got == slice(stream, 1300 - dtc01::SpeechShaper::kOnsetPad, 2101));
        assert(shaper.idle_samples() == 5 * kBlock);
    }

    // A later piece: pre-speech audio produced while idle goes, the DSP's own
    // silence ahead of the phrase stays.
    {
        dtc01::SpeechShaper shaper(false);
        const auto got = run(shaper, blocks);
        assert(got == slice(stream, 1000, 2101));
    }

    // An idle gap inside speech is released in order when speech resumes.
    {
        const std::vector<Block> gap = {
            {loud(kBlock, 0.0), false},
            {zeros(kBlock), true},
            {zeros(kBlock), true},
            {loud(kBlock, 3.0), false},
            {zeros(kBlock), true},
        };
        dtc01::SpeechShaper shaper(false);
        const auto got = run(shaper, gap);
        const auto all = flatten(gap);
        assert(got == slice(all, 0, 4 * kBlock + 1));
    }

    // A DAC stuck at a loud level (DESIGN.md s22) and a quiet waveform are
    // never speech, and nothing is written for them.
    {
        dtc01::SpeechShaper stuck(true);
        std::vector<Block> b(10, Block{std::vector<int16_t>(kBlock, 5000), false});
        assert(run(stuck, b).empty() && !stuck.speech_started());

        dtc01::SpeechShaper quiet(true);
        std::vector<Block> q(10, Block{faint(kBlock), false});
        assert(run(quiet, q).empty() && !quiet.speech_started());
        assert(quiet.idle_samples() == 0);
    }

    // Long silence before speech: only the retained second is searched, and
    // the result is still the pad plus the speech.
    {
        std::vector<Block> b(50, Block{zeros(kBlock), true});
        b.push_back({loud(kBlock, 0.0), false});
        dtc01::SpeechShaper shaper(true);
        const auto got = run(shaper, b);
        assert(got.size() == dtc01::SpeechShaper::kOnsetPad + kBlock);
        const auto all = flatten(b);
        assert(got == slice(all, all.size() - got.size(), all.size()));
    }

    std::puts("test_speech_shaper: PASS");
    return 0;
}
