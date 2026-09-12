#include "ratebooster.hpp"
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

    printf("all assertions passed\n");
    return 0;
}
