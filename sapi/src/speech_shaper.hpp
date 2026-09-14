// speech_shaper.hpp - decides which synthesized samples are worth writing.
//
// The emulator produces audio at the DTC-01's fixed 10 kHz whether or not the
// firmware is saying anything, and much of what it produces around an
// utterance is dead air: the firmware spends 150-560 ms working an utterance
// out before its first sound, and after the last one the pump has to watch
// the pipeline stay idle for up to a second before it can trust that the
// utterance is over (DESIGN.md s20, s23). Written to SAPI, all of that is
// heard as delay. SpeechShaper holds such audio back and releases it only if
// speech follows, so every sample that is written is exactly what the
// firmware produced -- only the silence around it is dropped.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dtc01 {

class SpeechShaper {
public:
    // |sample| above this, on a moving waveform, is speech. The DAC holds its
    // last value when the DSP has nothing to say, and v1.8 parks it at small
    // non-zero levels -- once at 128 (DESIGN.md s22) -- so level alone never
    // counts.
    static constexpr int kSpeechLevel = 256;

    // A run this long of one repeated value is the DAC holding, not a waveform.
    static constexpr size_t kHoldRun = 200;  // 20 ms

    // How much of that hold is kept ahead of the first sound, so the waveform
    // starts from rest rather than on its first sample.
    static constexpr size_t kOnsetPad = 50;  // 5 ms

    // Pre-speech audio retained while waiting for the first sound.
    static constexpr size_t kPreSpeechKeep = 10000;  // 1 s

    // trim_lead: this is the first speech of a SAPI Speak call, where silence
    // before the first sound is pure latency -- drop all of it, back to where
    // the waveform starts. Otherwise keep the pause the firmware itself
    // generates ahead of a phrase (the natural gap between pieces of one
    // call), dropping only what was produced while the pipeline sat idle.
    explicit SpeechShaper(bool trim_lead) : trim_lead_(trim_lead) {}

    // One block from the machine, and whether the firmware was idle over it:
    // nothing queued for it to say, and its output silent. Appends whatever
    // should be written now to `emit`.
    void add(const int16_t* samples, size_t count, bool idle, std::vector<int16_t>& emit);

    // End of the piece: appends held audio up to its last moving sample, so a
    // decaying tail survives but the DAC holding afterwards does not.
    void finish(std::vector<int16_t>& emit);

    bool speech_started() const { return started_; }

    // Consecutive samples, in whole blocks, that ended with the pipeline idle.
    size_t idle_samples() const { return idle_run_; }

private:
    bool trim_lead_;
    bool started_ = false;
    bool have_last_ = false;
    int16_t last_ = 0;             // the most recent sample added
    int16_t before_held_ = 0;      // the sample just ahead of held_
    size_t idle_run_ = 0;
    std::vector<int16_t> pre_;     // before the first sound
    std::vector<int16_t> held_;    // after it, while the pipeline is idle
};

}  // namespace dtc01
