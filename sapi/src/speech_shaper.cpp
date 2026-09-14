// speech_shaper.cpp - see speech_shaper.hpp.
#include "speech_shaper.hpp"

#include <cstdlib>

namespace dtc01 {

void SpeechShaper::add(const int16_t* samples, size_t count, bool idle,
                       std::vector<int16_t>& emit) {
    if (count == 0) {
        return;
    }

    if (started_) {
        if (idle) {
            if (held_.empty()) {
                before_held_ = last_;
            }
            held_.insert(held_.end(), samples, samples + count);
            idle_run_ += count;
        } else {
            // Speech resumed: whatever was held was a pause inside it.
            emit.insert(emit.end(), held_.begin(), held_.end());
            held_.clear();
            emit.insert(emit.end(), samples, samples + count);
            idle_run_ = 0;
        }
        last_ = samples[count - 1];
        return;
    }

    size_t onset = count;
    for (size_t i = 0; i < count; ++i) {
        const bool moving = (i > 0) ? samples[i] != samples[i - 1]
                                    : (have_last_ && samples[i] != last_);
        if (moving && std::abs(static_cast<int>(samples[i])) > kSpeechLevel) {
            onset = i;
            break;
        }
    }
    last_ = samples[count - 1];
    have_last_ = true;

    if (onset == count) {
        if (trim_lead_ || !idle) {
            pre_.insert(pre_.end(), samples, samples + count);
            if (pre_.size() > kPreSpeechKeep) {
                pre_.erase(pre_.begin(), pre_.end() - kPreSpeechKeep);
            }
        }
        idle_run_ = idle ? idle_run_ + count : 0;
        return;
    }

    started_ = true;
    idle_run_ = 0;
    const size_t onset_at = pre_.size() + onset;
    pre_.insert(pre_.end(), samples, samples + count);

    size_t start = 0;
    if (trim_lead_) {
        // Walk back from the first loud sample to the end of the last hold:
        // everything after it is the waveform's own attack, however quiet.
        size_t run = 0;
        size_t run_top = 0;
        for (size_t j = onset_at; j-- > 0;) {
            if (run > 0 && pre_[j] == pre_[j + 1]) {
                ++run;
            } else {
                run = 1;
                run_top = j;
            }
            if (run >= kHoldRun) {
                const size_t waveform = run_top + 1;
                start = waveform > kOnsetPad ? waveform - kOnsetPad : 0;
                break;
            }
        }
    }
    emit.insert(emit.end(), pre_.begin() + start, pre_.end());
    pre_.clear();
}

void SpeechShaper::finish(std::vector<int16_t>& emit) {
    if (started_ && !held_.empty()) {
        size_t keep = 0;
        int16_t prev = before_held_;
        for (size_t i = 0; i < held_.size(); ++i) {
            if (held_[i] != prev) {
                keep = i + 1;
            }
            prev = held_[i];
        }
        emit.insert(emit.end(), held_.begin(), held_.begin() + keep);
    }
    held_.clear();
    pre_.clear();
}

}  // namespace dtc01
