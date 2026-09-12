// Loads dtc01_x64.dll, builds ROM images via rom_images (Task A3 provides
// load_rom_images), creates a Machine, consumes boot, speaks "[:np] test.\r",
// asserts it produced non-silent audio.
#include "dtc01_core.hpp"
#include "rom_images.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
int wmain(int argc, wchar_t** argv) {
    auto imgs = dtc01::load_rom_images(argv[1], L"v20");   // argv[1] = staged roms_v20
    auto m = dtc01::Machine::create(imgs.main, imgs.dsp, argv[2]); // argv[2] = dtc01_x64.dll
    assert(m);
    std::lock_guard<std::mutex> lk(dtc01::exec_mutex());
    m->consume_boot_announcement();
    m->feed_text("[:np] Hello test.\r");
    int16_t buf[8192]; long total = 0; int peak = 0; int got; int idleRuns = 0;
    while (total < 14 * 10000) {
        got = m->run_block(buf, 8192);
        if (got <= 0) break;
        total += got;
        for (int i = 0; i < got; ++i) peak = std::max(peak, std::abs((int)buf[i]));
        if (m->is_idle() && total > 10000) { if (++idleRuns > 4) break; } else idleRuns = 0;
    }
    printf("total=%ld peak=%d\n", total, peak);
    assert(peak > 2000);
    return 0;
}
