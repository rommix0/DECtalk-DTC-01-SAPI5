// rom_images.hpp - assemble DTC-01 ROM images from a user-supplied dump.
//
// Mirrors addon/synthDrivers/dectalkDtc01/emu/rom_loader.py: same chip SHA1
// tables, same MAIN_CPU_IMAGE_SIZE/DSP_IMAGE_SIZE, same even/odd byte
// interleave, same DSP candidate preference order (409/410 before 204/205
// for v2.0). See that file's module docstring and DESIGN.md sections 1, 2,
// 8, and 21 for the hardware rationale this is mechanically re-deriving.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace dtc01 {

// Assembled ROM images ready for the emulator cores.
struct RomImages {
    std::vector<uint8_t> main;   // 0x40000-byte 68000 program ROM image
    std::vector<uint16_t> dsp;   // 2048 big-endian TMS32010 program words
};

// Assemble both images from rom_dir for the given firmware version
// ("v20"/"v18", case-insensitive; also accepts "2.0"/"1.8" etc. like
// rom_loader.resolve_version). Throws std::runtime_error listing the
// missing/invalid chips if rom_dir does not hold a complete set.
RomImages load_rom_images(const std::wstring& rom_dir, const std::wstring& version);

// Which firmware versions rom_dir holds a complete, valid set for (main
// image plus at least one DSP candidate pair). One directory scan.
std::vector<std::wstring> available_versions(const std::wstring& rom_dir);

}  // namespace dtc01
