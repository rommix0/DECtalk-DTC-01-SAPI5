// Builds v20 images from a dir holding both DSP pairs; asserts main is 0x40000
// bytes, dsp is 2048 words, and dsp[1] matches the 409 pair's word (0x00d9),
// proving 409/410 is preferred (204's word[1] is 0x00e1).
#include "rom_images.hpp"
#include <cassert>
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fwprintf(stderr, L"usage: %s <rom_dir>\n", argv[0]);
        return 2;
    }
    auto im = dtc01::load_rom_images(argv[1], L"v20");   // argv[1] = roms_v20 (both pairs)
    assert(im.main.size() == 0x40000);
    assert(im.dsp.size() == 2048);
    assert(im.dsp[1] == 0x00d9);   // 409/410 preferred (see rom_loader diff)

    auto versions = dtc01::available_versions(argv[1]);
    bool has_v20 = false;
    for (const auto& v : versions) {
        if (v == L"v20") has_v20 = true;
    }
    assert(has_v20);

    std::wprintf(L"test_rom_images: OK\n");
    return 0;
}
