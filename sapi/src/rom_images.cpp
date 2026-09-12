// rom_images.cpp - see rom_images.hpp for the contract this implements and
// addon/synthDrivers/dectalkDtc01/emu/rom_loader.py for the source of truth
// this mirrors (chip tables, sizes, interleave, DSP candidate order).
#include "rom_images.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace dtc01 {
namespace {

namespace fs = std::filesystem;

constexpr size_t MAIN_CPU_IMAGE_SIZE = 0x40000;
constexpr size_t DSP_IMAGE_SIZE = 0x1000;  // 0x800 words, 2 bytes/word

struct RomChunk {
    const char* sha1;   // lowercase hex, 40 chars
    size_t size;
    size_t offset;       // starting byte offset within the linear image
    const char* label;   // human-readable chip identity, for error messages only
};

// 68000 main-CPU program ROM: 16 x 0x4000-byte chips, byte-interleaved in
// pairs (stride 2) into a linear 0x40000-byte big-endian word image.
// v2.0 firmware (first-half tag 23Jul84 / second-half tag 02Jul84).
constexpr std::array<RomChunk, 16> MAIN_CPU_ROMS_V20{{
    {"e586de03e113683c2534fca1f3f40ba391193044", 0x4000, 0x00000, "23-123e5 @E8"},
    {"7954bb56b7591f8954403a22d34de31c7d5441ac", 0x4000, 0x00001, "23-119e5 @E22"},
    {"7724babf4ae5d77c0b4200f608d599058d04b25c", 0x4000, 0x08000, "23-124e5 @E7"},
    {"af5e4ea0b3631f7d6f16c22e86a33fa2cb520ee0", 0x4000, 0x08001, "23-120e5 @E21"},
    {"1b60cd71dfa83408b17e13f683b6bf3198c905cc", 0x4000, 0x10000, "23-125e5 @E6"},
    {"4ad0b00628a90085cd7c78a354256c39fd14db6c", 0x4000, 0x10001, "23-121e5 @E20"},
    {"e2b2415eec838ddd46094f2fea93fd289dd0caa2", 0x4000, 0x18000, "23-126e5 @E5"},
    {"92ab22a24484ad0d0f5c8a07347105509999f3ee", 0x4000, 0x18001, "23-122e5 @E19"},
    {"b5aec0bf37a176ff4d66d6a10357715957662ebd", 0x4000, 0x20000, "23-103e5 @E4"},
    {"891f3a3b4ce75ef14001257bc8f1f60463a9a7cb", 0x4000, 0x20001, "23-095e5 @E18"},
    {"4d6808f67cbdd316df23adc8ddf701df57aa854a", 0x4000, 0x28000, "23-104e5 @E3"},
    {"496c69e52cfa013173f7b9c500ce544a03ad01f7", 0x4000, 0x28001, "23-096e5 @E17"},
    {"de0c25687bab3ff0c88c98622092e0b58331aa16", 0x4000, 0x30000, "23-105e5 @E2"},
    {"c450abae0ccf372d7eb87370b8a8c97a45e164d3", 0x4000, 0x30001, "23-097e5 @E16"},
    {"355348bfc96a04193136cdde3418366e6476c3ca", 0x4000, 0x38000, "23-106e5 @E1"},
    {"01921e77b46c2d4845023605239c45ffa4a35872", 0x4000, 0x38001, "23-098e5 @E15"},
}};

// v1.8 firmware (first-half tag 05Dec83 / second-half tag 11Oct83). Same
// chip positions, different part numbers; hashes from MAME's ROM_BIOS(1)
// entries and verified against the local dump.
constexpr std::array<RomChunk, 16> MAIN_CPU_ROMS_V18{{
    {"1b1b9c1e092c44329b385fb04001e13422eb8d39", 0x4000, 0x00000, "23-063e5 @E8"},
    {"84bbe9ff303ea6ce7b1c0b1ad05421edd18fae49", 0x4000, 0x00001, "23-059e5 @E22"},
    {"fdd91e4d2ef92608a08b2e78b6108e31ff53a1f9", 0x4000, 0x08000, "23-064e5 @E7"},
    {"c95662d0d40499af01cdc23f05936762ab54081a", 0x4000, 0x08001, "23-060e5 @E21"},
    {"232b622cef6d69a493db1ed02e5236235c68daba", 0x4000, 0x10000, "23-065e5 @E6"},
    {"81daa4abae273c7f0aead902b5c3c842f7e7f116", 0x4000, 0x10001, "23-061e5 @E20"},
    {"5f9f916b99867d1adbafd58d411feb630f6e4b6d", 0x4000, 0x18000, "23-066e5 @E5"},
    {"46ee22a295b8709b6f829751aca5f92e4f459a9f", 0x4000, 0x18001, "23-062e5 @E19"},
    {"1d8008e30a448358224364fd8237dbb08907b219", 0x4000, 0x20000, "23-032e5 @E4"},
    {"55c759b3fb927d2dfc9d77e8e080748866bea854", 0x4000, 0x20001, "23-031e5 @E18"},
    {"738337c5b6acd3f30c3c4be2457370d2ce9313f9", 0x4000, 0x28000, "23-034e5 @E3"},
    {"5946ccd367d88a484bb1549d0cc990b9b7d88f0c", 0x4000, 0x28001, "23-033e5 @E17"},
    {"30f95e5383c4f71bc700346e2d49e8ad70b94c8c", 0x4000, 0x30000, "23-036e5 @E2"},
    {"7b3b68e61b421dedaad88b5600c739943a316c9e", 0x4000, 0x30001, "23-035e5 @E16"},
    {"abd6af442690e981a9089f19febffc8f3fb52717", 0x4000, 0x38000, "23-038e5 @E1"},
    {"a743a23625feadf6e46ef889e2bb04af88589992", 0x4000, 0x38001, "23-037e5 @E15"},
}};

// TMS32010 DSP program ROM. Each chip supplies the high (offset 0x000) or
// low (offset 0x001) byte of every big-endian word.
constexpr std::array<RomChunk, 2> DSP_ROMS_V20_409{{
    {"3fabe018d0e0b478093951cb20501853358faa18", 0x800, 0x000, "23-410f4 @E70"},
    {"9a13426c92f879f2953f180f805990a91c37ac43", 0x800, 0x001, "23-409f4 @E69"},
}};
constexpr std::array<RomChunk, 2> DSP_ROMS_V20_204{{
    {"3136bae243ef48721e21c66fde70dab5fc3c21d0", 0x800, 0x000, "23-205f4 @E70"},
    {"9409f90f7a397b041e4440341f2d7934cb479285", 0x800, 0x001, "23-204f4 @E69"},
}};
constexpr std::array<RomChunk, 2> DSP_ROMS_V18{{
    {"e8c25ca092dde2dc0aec73921af806026bdfbbc3", 0x800, 0x000, "23-166f4 @E70"},
    {"249f269c38f7f44edb6d025bcc867c8ca0de3e9c", 0x800, 0x001, "23-165f4 @E69"},
}};

struct RomSet {
    const wchar_t* name;                 // "v20" / "v18"
    const std::array<RomChunk, 16>* main_cpu;
    // Candidate DSP pairs in preference order (409/410 before 204/205 for v2.0).
    std::vector<const std::array<RomChunk, 2>*> dsp;
};

const RomSet& rom_set_for(const std::wstring& version) {
    // Normalise the same spellings rom_loader.resolve_version() accepts.
    std::wstring key;
    key.reserve(version.size());
    for (wchar_t c : version) key.push_back(towlower(c));
    while (!key.empty() && iswspace(key.front())) key.erase(key.begin());
    while (!key.empty() && iswspace(key.back())) key.pop_back();

    static const RomSet v20{L"v20", &MAIN_CPU_ROMS_V20, {&DSP_ROMS_V20_409, &DSP_ROMS_V20_204}};
    static const RomSet v18{L"v18", &MAIN_CPU_ROMS_V18, {&DSP_ROMS_V18}};

    if (key == L"v20" || key == L"20" || key == L"2.0") return v20;
    if (key == L"v18" || key == L"18" || key == L"1.8") return v18;
    throw std::runtime_error("unknown ROM version (expected v20 or v18)");
}

std::string sha1_hex(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0);
    if (status < 0) throw std::runtime_error("BCryptOpenAlgorithmProvider failed");

    DWORD hash_len = 0, cb_data = 0;
    status = BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_len),
                                sizeof(hash_len), &cb_data, 0);
    if (status < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCryptGetProperty(HASH_LENGTH) failed");
    }

    BCRYPT_HASH_HANDLE hash = nullptr;
    status = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    if (status < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCryptCreateHash failed");
    }

    status = BCryptHashData(hash, const_cast<PUCHAR>(data.data()),
                             static_cast<ULONG>(data.size()), 0);
    if (status < 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCryptHashData failed");
    }

    std::vector<uint8_t> digest(hash_len);
    status = BCryptFinishHash(hash, digest.data(), hash_len, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (status < 0) throw std::runtime_error("BCryptFinishHash failed");

    static const char* hexd = "0123456789abcdef";
    std::string out;
    out.resize(digest.size() * 2);
    for (size_t i = 0; i < digest.size(); ++i) {
        out[i * 2] = hexd[digest[i] >> 4];
        out[i * 2 + 1] = hexd[digest[i] & 0xF];
    }
    return out;
}

// Index rom_dir non-recursively by file-content SHA1, mirroring
// rom_loader._index_dir_by_sha1.
std::unordered_map<std::string, std::vector<uint8_t>> index_dir_by_sha1(const std::wstring& rom_dir) {
    std::unordered_map<std::string, std::vector<uint8_t>> index;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::path(rom_dir), ec)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream f(entry.path(), std::ios::binary);
        if (!f) continue;
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
        index[sha1_hex(data)] = std::move(data);
    }
    if (ec) throw std::runtime_error("could not list ROM directory");
    return index;
}

// Assemble `chunks` into a zero-filled image of `image_size` bytes, placing
// each chip's bytes at chunk.offset + i*2 (even/odd interleave). Throws
// std::runtime_error listing missing/invalid chips on failure.
std::vector<uint8_t> assemble(const std::unordered_map<std::string, std::vector<uint8_t>>& files,
                               const RomChunk* chunks, size_t count, size_t image_size) {
    std::vector<uint8_t> image(image_size, 0);
    std::ostringstream missing;
    bool any_missing = false;
    for (size_t c = 0; c < count; ++c) {
        const RomChunk& chunk = chunks[c];
        auto it = files.find(chunk.sha1);
        if (it == files.end()) {
            any_missing = true;
            missing << "\n  " << chunk.label << " (sha1 " << chunk.sha1 << ")";
            continue;
        }
        const auto& data = it->second;
        if (data.size() != chunk.size) {
            any_missing = true;
            missing << "\n  " << chunk.label << ": found matching hash but size "
                    << data.size() << " != expected " << chunk.size;
            continue;
        }
        for (size_t i = 0; i < data.size(); ++i) {
            image[chunk.offset + i * 2] = data[i];
        }
    }
    if (any_missing) {
        throw std::runtime_error("ROM validation failed, missing/invalid chips:" + missing.str());
    }
    return image;
}

// Assemble the first DSP candidate pair the dump can satisfy, in preference
// order (mirrors rom_loader._assemble_dsp).
std::vector<uint8_t> assemble_dsp(const std::unordered_map<std::string, std::vector<uint8_t>>& files,
                                   const std::vector<const std::array<RomChunk, 2>*>& candidates) {
    std::ostringstream failures;
    for (const auto* pair : candidates) {
        try {
            return assemble(files, pair->data(), pair->size(), DSP_IMAGE_SIZE);
        } catch (const std::runtime_error& e) {
            failures << "\n" << e.what();
        }
    }
    throw std::runtime_error("no complete DSP ROM pair found; tried:" + failures.str());
}

std::vector<uint16_t> to_words(const std::vector<uint8_t>& image) {
    std::vector<uint16_t> words;
    words.reserve(image.size() / 2);
    for (size_t i = 0; i + 1 < image.size(); i += 2) {
        words.push_back(static_cast<uint16_t>((image[i] << 8) | image[i + 1]));
    }
    return words;
}

}  // namespace

RomImages load_rom_images(const std::wstring& rom_dir, const std::wstring& version) {
    const RomSet& rs = rom_set_for(version);
    auto files = index_dir_by_sha1(rom_dir);

    RomImages result;
    result.main = assemble(files, rs.main_cpu->data(), rs.main_cpu->size(), MAIN_CPU_IMAGE_SIZE);
    auto dsp_image = assemble_dsp(files, rs.dsp);
    result.dsp = to_words(dsp_image);
    return result;
}

std::vector<std::wstring> available_versions(const std::wstring& rom_dir) {
    auto files = index_dir_by_sha1(rom_dir);
    std::vector<std::wstring> found;
    for (const wchar_t* name : {L"v20", L"v18"}) {
        try {
            const RomSet& rs = rom_set_for(name);
            assemble(files, rs.main_cpu->data(), rs.main_cpu->size(), MAIN_CPU_IMAGE_SIZE);
            assemble_dsp(files, rs.dsp);
        } catch (const std::runtime_error&) {
            continue;
        }
        found.emplace_back(name);
    }
    return found;
}

}  // namespace dtc01
