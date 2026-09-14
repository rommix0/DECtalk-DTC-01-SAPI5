#include "ratebooster.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

// A `durationSamples`-sample, `freqHz` sine at dtc01::kRateBoosterSampleRate,
// scaled to `amplitude` -- used as a pitch-checkable stand-in for rendered
// speech (real speech isn't a pure tone, but a preserved sine frequency is a
// simple, unambiguous proxy for "pitch was preserved").
std::vector<int16_t> makeSine(int durationSamples, double freqHz, double amplitude) {
    std::vector<int16_t> out(durationSamples);
    for (int i = 0; i < durationSamples; ++i) {
        double t = static_cast<double>(i) / dtc01::kRateBoosterSampleRate;
        out[i] = static_cast<int16_t>(std::lround(amplitude * std::sin(2.0 * kPi * freqHz * t)));
    }
    return out;
}

double rms(const std::vector<int16_t>& x) {
    if (x.empty()) return 0.0;
    double sum = 0.0;
    for (int16_t v : x) sum += static_cast<double>(v) * v;
    return std::sqrt(sum / x.size());
}

// Zero-crossing-rate frequency estimate: a sine at F Hz crosses zero 2F
// times per second, so (crossings / 2) / duration_seconds ~= F. Robust
// enough to confirm pitch survived time-compression without a full DFT.
double estimateFreqHz(const std::vector<int16_t>& x) {
    if (x.size() < 2) return 0.0;
    int crossings = 0;
    for (size_t i = 1; i < x.size(); ++i) {
        bool prevNeg = x[i - 1] < 0;
        bool curNeg = x[i] < 0;
        if (prevNeg != curNeg) ++crossings;
    }
    double durationSeconds = static_cast<double>(x.size()) / dtc01::kRateBoosterSampleRate;
    return (crossings / 2.0) / durationSeconds;
}

}  // namespace

int wmain() {
    constexpr int kInputSamples = 10000;   // 1.0 s at 10 kHz.
    constexpr double kToneHz = 200.0;
    constexpr double kAmplitude = 8000.0;  // headroom for crossfade overshoot.

    std::vector<int16_t> tone = makeSine(kInputSamples, kToneHz, kAmplitude);
    double inRms = rms(tone);
    double inFreq = estimateFreqHz(tone);
    printf("input: %zu samples, rms=%.1f, estFreq=%.1f Hz\n", tone.size(), inRms, inFreq);
    assert(std::fabs(inFreq - kToneHz) < 5.0);  // sanity check on the test signal itself.

    // factor == 1.0: exact passthrough.
    std::vector<int16_t> passthrough = dtc01::time_compress(tone, 1.0);
    assert(passthrough == tone);
    printf("factor=1.0: exact passthrough (%zu samples)\n", passthrough.size());

    // factor == 2.0: ~half the length, pitch preserved, not silent.
    std::vector<int16_t> compressed = dtc01::time_compress(tone, 2.0);
    double outRms = rms(compressed);
    double outFreq = estimateFreqHz(compressed);
    printf("factor=2.0: %zu samples, rms=%.1f, estFreq=%.1f Hz\n",
           compressed.size(), outRms, outFreq);

    size_t expectedLen = kInputSamples / 2;
    double lenRatio = static_cast<double>(compressed.size()) / expectedLen;
    assert(lenRatio > 0.95 && lenRatio < 1.05);  // within +/-5% of 5000 samples.

    assert(outRms > inRms * 0.5 && outRms < inRms * 1.5);  // energy roughly preserved, not silent.
    assert(std::fabs(outFreq - kToneHz) < 20.0);            // pitch preserved (within 10%).

    // Edge case: empty input must return empty output, no crash.
    std::vector<int16_t> emptyOut = dtc01::time_compress({}, 2.0);
    assert(emptyOut.empty());
    printf("empty input: %zu samples out (no crash)\n", emptyOut.size());

    // Edge case: input shorter than one analysis frame (kFrameSamples == 200
    // in ratebooster.cpp) takes the early passthrough branch. Asserting
    // exact equality, not just "no crash" -- that's the documented
    // behavior in ratebooster.hpp ("too short to frame meaningfully").
    std::vector<int16_t> shortIn = makeSine(50, kToneHz, kAmplitude);  // 50 < 200
    std::vector<int16_t> shortOut = dtc01::time_compress(shortIn, 2.0);
    assert(shortOut == shortIn);
    printf("short input (50 samples < frame): exact passthrough (%zu samples)\n", shortOut.size());

    // Edge case: non-integer factor. 10000 / 1.5 = 6666.67.
    std::vector<int16_t> compressed15 = dtc01::time_compress(tone, 1.5);
    double outRms15 = rms(compressed15);
    printf("factor=1.5: %zu samples, rms=%.1f\n", compressed15.size(), outRms15);
    double expectedLen15 = kInputSamples / 1.5;
    double lenRatio15 = compressed15.size() / expectedLen15;
    assert(lenRatio15 > 0.95 && lenRatio15 < 1.05);  // within +/-5% of ~6667 samples.
    assert(outRms15 > inRms * 0.5 && outRms15 < inRms * 1.5);  // not silent.

    // Edge case: factor <= 0 must be treated as passthrough (no
    // div-by-zero, no crash) -- same clamp-up-to-1.0 path as factor < 1.0.
    std::vector<int16_t> zeroFactorOut = dtc01::time_compress(tone, 0.0);
    assert(zeroFactorOut == tone);
    std::vector<int16_t> negFactorOut = dtc01::time_compress(tone, -1.0);
    assert(negFactorOut == tone);
    printf("factor<=0 (0.0, -1.0): exact passthrough, no crash\n");

    // Streaming: TimeCompressor must reproduce time_compress() exactly, however
    // the input is chunked -- the engine feeds it as the firmware synthesizes.
    // The signal gives the correlation search something different to latch
    // onto at every frame: two tones plus a deterministic noise component.
    std::vector<int16_t> speechy(37123);
    uint32_t lcg = 12345;
    for (size_t i = 0; i < speechy.size(); ++i) {
        lcg = lcg * 1664525u + 1013904223u;
        double t = static_cast<double>(i) / dtc01::kRateBoosterSampleRate;
        double v = 5000.0 * std::sin(2.0 * kPi * 140.0 * t) +
                   2500.0 * std::sin(2.0 * kPi * 910.0 * t) +
                   (static_cast<int>(lcg >> 20) - 2048);
        speechy[i] = static_cast<int16_t>(std::lround(v));
    }
    for (double factor : {0.5, 1.0, 1.25, 2.0, 3.0, 6.0, 9.0}) {
        for (size_t len : {speechy.size(), size_t{0}, size_t{199}, size_t{200}, size_t{239},
                           size_t{241}, size_t{1000}}) {
            const std::vector<int16_t> input(speechy.begin(), speechy.begin() + len);
            const std::vector<int16_t> batch = dtc01::time_compress(input, factor);
            for (size_t chunk : {size_t{1}, size_t{37}, size_t{250}, size_t{1000}, len}) {
                if (chunk == 0) continue;
                dtc01::TimeCompressor stream(factor);
                std::vector<int16_t> got;
                for (size_t off = 0; off < len; off += chunk) {
                    stream.push(input.data() + off, std::min(chunk, len - off), got);
                }
                stream.finish(got);
                assert(got == batch);
            }
        }
    }
    // finish() resets: a second stream through the same object matches too.
    dtc01::TimeCompressor reused(2.0);
    std::vector<int16_t> firstRun, secondRun;
    reused.push(speechy.data(), 5000, firstRun);
    reused.finish(firstRun);
    reused.push(speechy.data(), 5000, secondRun);
    reused.finish(secondRun);
    assert(firstRun == secondRun && !firstRun.empty());
    printf("streaming: identical to time_compress for every factor, length and chunk size\n");

    printf("all assertions passed\n");
    return 0;
}
