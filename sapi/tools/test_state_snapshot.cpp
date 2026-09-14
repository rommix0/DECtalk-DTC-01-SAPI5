// test_state_snapshot.cpp - the native state snapshots (dtc01_state_*) the
// SAPI engine uses to start every utterance from the post-boot state, and to
// give a new engine instance a booted machine without running the boot.
//
//   test_state_snapshot <rom dir> <dtc01_x64.dll>
//
// <rom dir> must hold v2.0; if it also holds v1.8 (chips are matched by
// content hash, so both can share one flat directory) the cross-firmware
// rejection case runs too.
#include "dtc01_core.hpp"
#include "rom_images.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kSamples = 40000;  // 4 s: the whole utterance and silence after it
const std::string kText = "[:np] [:ra 207]Hello, this is a snapshot test.\r";

std::vector<int16_t> render(dtc01::Machine& m, int samples) {
    std::vector<int16_t> out(samples);
    int got = 0;
    while (got < samples) {
        const int n = m.run_block(out.data() + got, std::min(1000, samples - got));
        assert(n > 0);
        got += n;
    }
    return out;
}

std::vector<int16_t> speak(dtc01::Machine& m) {
    m.feed_text(kText);
    return render(m, kSamples);
}

int peak(const std::vector<int16_t>& pcm) {
    int p = 0;
    for (int16_t s : pcm) p = std::max(p, std::abs(static_cast<int>(s)));
    return p;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::fwprintf(stderr, L"usage: test_state_snapshot <rom dir> <dtc01_x64.dll>\n");
        return 2;
    }
    const std::wstring rom_dir = argv[1];
    const std::wstring dll = argv[2];

    std::lock_guard<std::mutex> lk(dtc01::exec_mutex());
    const dtc01::RomImages v20 = dtc01::load_rom_images(rom_dir, L"v20");

    auto a = dtc01::Machine::create(v20.main, v20.dsp, dll);
    assert(a);
    a->consume_boot_announcement();
    std::vector<uint8_t> booted;
    assert(a->save_state(booted) && !booted.empty());

    // Rewinding a machine in place replays an utterance bit for bit.
    const std::vector<int16_t> first = speak(*a);
    assert(peak(first) > 2000);
    assert(a->restore_state(booted));
    assert(speak(*a) == first);
    std::puts("in-place rewind: identical");

    // The state restores into another machine -- what a new engine instance
    // does instead of booting -- and both then speak identically even when
    // they run interleaved, swapping in and out of Musashi's globals.
    auto b = dtc01::Machine::create(v20.main, v20.dsp, dll);
    assert(b);
    assert(b->restore_state(booted));
    assert(a->restore_state(booted));
    a->feed_text(kText);
    b->feed_text(kText);
    std::vector<int16_t> from_a, from_b;
    while (from_a.size() < kSamples) {
        const auto chunk_a = render(*a, 1000);
        const auto chunk_b = render(*b, 1000);
        from_a.insert(from_a.end(), chunk_a.begin(), chunk_a.end());
        from_b.insert(from_b.end(), chunk_b.begin(), chunk_b.end());
    }
    assert(from_a == first);
    assert(from_b == first);
    std::puts("cross-machine restore, interleaved: identical");

    // A truncated or damaged state is rejected and leaves the target untouched.
    assert(a->restore_state(booted));
    const std::vector<uint8_t> truncated(booted.begin(), booted.end() - 1);
    assert(!a->restore_state(truncated));
    std::vector<uint8_t> damaged = booted;
    damaged[0] ^= 0xFF;
    assert(!a->restore_state(damaged));
    assert(speak(*a) == first);
    std::puts("damaged/truncated state: rejected, machine untouched");

    // A state never restores into a machine running other ROMs.
    const std::vector<std::wstring> versions = dtc01::available_versions(rom_dir);
    if (std::find(versions.begin(), versions.end(), L"v18") != versions.end()) {
        const dtc01::RomImages v18 = dtc01::load_rom_images(rom_dir, L"v18");
        auto c = dtc01::Machine::create(v18.main, v18.dsp, dll);
        assert(c);
        c->consume_boot_announcement();
        std::vector<uint8_t> c_booted;
        assert(c->save_state(c_booted));
        assert(!c->restore_state(booted));
        const std::vector<int16_t> c_first = speak(*c);
        assert(c->restore_state(c_booted));
        assert(speak(*c) == c_first);
        assert(c_first != first);
        std::puts("v2.0 state into a v1.8 machine: rejected, machine untouched");
    } else {
        std::puts("SKIP: no v1.8 ROMs in the directory; cross-firmware case not run");
    }

    std::puts("test_state_snapshot: PASS");
    return 0;
}
