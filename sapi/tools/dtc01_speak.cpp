// dtc01_speak.cpp - Phase-A console deliverable: loads ROMs, drives a
// dtc01_*.dll Machine through the same voice+rate+dv+text+flush sequence the
// SAPI5 driver will use, and writes the result to a WAV file.
//
// Usage: dtc01_speak.exe <rom_dir> <dll> <firmware> <voice_key> <text> <out.wav>
//
// Render-loop shape (block-pump until idle-with-sustained-silence or a hard
// cap) mirrors addon/synthDrivers/dectalkDtc01/emu/native.py's run_block()
// pattern and sapi/tools/test_core.cpp's own pump loop.
#include "dtc01_core.hpp"
#include "rom_images.hpp"
#include "voices.hpp"
#include "text_pipeline.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace {

// Wide (UTF-16, as argv arrives under wmain) -> UTF-8 std::string.
std::string to_utf8(const wchar_t* w) {
    if (!w || !*w) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out(static_cast<size_t>(len - 1), '\0');  // len includes the NUL
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

void write_wav_pcm16_mono(const std::wstring& path, const std::vector<int16_t>& samples,
                           uint32_t sample_rate) {
    const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    const uint16_t channels = 1;
    const uint16_t bits_per_sample = 16;
    const uint32_t byte_rate = sample_rate * channels * bits_per_sample / 8;
    const uint16_t block_align = channels * bits_per_sample / 8;
    const uint32_t riff_size = 36 + data_bytes;

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("could not open output WAV for writing");

    f.write("RIFF", 4);
    f.write(reinterpret_cast<const char*>(&riff_size), 4);
    f.write("WAVE", 4);

    f.write("fmt ", 4);
    const uint32_t fmt_chunk_size = 16;
    f.write(reinterpret_cast<const char*>(&fmt_chunk_size), 4);
    const uint16_t audio_format = 1;  // PCM
    f.write(reinterpret_cast<const char*>(&audio_format), 2);
    f.write(reinterpret_cast<const char*>(&channels), 2);
    f.write(reinterpret_cast<const char*>(&sample_rate), 4);
    f.write(reinterpret_cast<const char*>(&byte_rate), 4);
    f.write(reinterpret_cast<const char*>(&block_align), 2);
    f.write(reinterpret_cast<const char*>(&bits_per_sample), 2);

    f.write("data", 4);
    f.write(reinterpret_cast<const char*>(&data_bytes), 4);
    if (!samples.empty()) {
        f.write(reinterpret_cast<const char*>(samples.data()), data_bytes);
    }
    if (!f) throw std::runtime_error("failed writing WAV data");
}

// Drop a trailing run of near-silent samples (the idle-detection tail lets a
// few blocks of near-zero audio through before the pump loop gives up).
// Mirrors native.py's is_flat()/peak_of() spirit: a low-but-nonzero DAC park
// level still counts as silence for trimming purposes.
void trim_trailing_silence(std::vector<int16_t>& samples) {
    constexpr int16_t SILENCE_THRESHOLD = 300;
    size_t end = samples.size();
    while (end > 0 && std::abs(static_cast<int>(samples[end - 1])) < SILENCE_THRESHOLD) {
        --end;
    }
    samples.resize(end);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 7) {
        std::fwprintf(stderr,
                       L"usage: %s <rom_dir> <dll> <firmware> <voice_key> <text> <out.wav>\n",
                       argv[0]);
        return 2;
    }

    const std::wstring rom_dir = argv[1];
    const std::wstring dll_path = argv[2];
    const std::wstring firmware_w = argv[3];
    const std::string firmware = to_utf8(argv[3]);
    const std::string voice_key = to_utf8(argv[4]);
    const std::string text = to_utf8(argv[5]);
    const std::wstring out_path = argv[6];
    const int wpm = 180;

    const dtc01::VoiceDef* voice = dtc01::find_voice(voice_key, firmware);
    if (!voice) {
        std::fprintf(stderr, "error: unknown voice '%s' for firmware '%s'\n",
                     voice_key.c_str(), firmware.c_str());
        return 1;
    }

    std::vector<int16_t> samples;
    try {
        dtc01::RomImages images = dtc01::load_rom_images(rom_dir, firmware_w);

        // Contract: hold exec_mutex() around every Machine call, including
        // create() and destruction -- the emulator core keeps 68000 state in
        // process globals. The Machine is created and destroyed entirely
        // inside this locked scope.
        std::lock_guard<std::mutex> lk(dtc01::exec_mutex());
        auto machine = dtc01::Machine::create(images.main, images.dsp, dll_path);
        if (!machine) {
            std::fprintf(stderr, "error: Machine::create failed (bad DLL/ROMs)\n");
            return 1;
        }

        machine->consume_boot_announcement();

        std::string sanitized = dtc01::sanitize_text(text);
        while (!sanitized.empty() &&
               std::isspace(static_cast<unsigned char>(sanitized.back()))) {
            sanitized.pop_back();
        }
        const std::string fed = dtc01::voice_command(voice->mnemonic) + " " +
                                 dtc01::rate_command(wpm) +
                                 dtc01::dv_command(voice_key, dtc01::DvParams{}) +
                                 sanitized + dtc01::flush_suffix(sanitized);
        machine->feed_text(fed);

        constexpr int kBlockSamples = 2000;
        constexpr long kCapSamples = 14 * 10000;  // ~14s hard ceiling at 10 kHz
        int16_t buf[kBlockSamples];
        long total = 0;
        int idle_runs = 0;
        while (total < kCapSamples) {
            int got = machine->run_block(buf, kBlockSamples);
            if (got <= 0) break;
            samples.insert(samples.end(), buf, buf + got);
            total += got;
            if (machine->is_idle() && total > 10000) {
                if (++idle_runs > 4) break;
            } else {
                idle_runs = 0;
            }
        }
        // machine (and the lock) fall out of scope here.
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    trim_trailing_silence(samples);

    try {
        write_wav_pcm16_mono(out_path, samples, 10000);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    std::printf("dtc01_speak: wrote %zu samples (%.2fs) to WAV\n", samples.size(),
                samples.size() / 10000.0);
    return 0;
}
