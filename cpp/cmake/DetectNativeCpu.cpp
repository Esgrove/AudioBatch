/// Reports the usable MSVC instruction sets and coarse vendor tuning for a local Release build.
/// This configure-time probe is compiled with baseline instructions and never cached across hosts.
/// The CRT ISA queries account for operating-system register state support.

#include <isa_availability.h>
#include <string_view>

#include <array>
#include <cstdio>
#include <cstring>
#include <immintrin.h>
#include <intrin.h>

/// Lists safe /arch candidates in descending order and the matching /favor setting.
/// Returns failure instead of allowing a local native build below the Windows AVX2 minimum.
int main()
{
    std::array<int, 4> registers {};
    __cpuid(registers.data(), 0);
    const auto maximumLeaf = registers[0];
    const std::array vendorWords {registers[1], registers[3], registers[2]};
    std::array<char, sizeof(vendorWords) + 1> vendor {};
    std::memcpy(vendor.data(), vendorWords.data(), sizeof(vendorWords));

    constexpr int structuredFeaturesLeaf {7};
    if (maximumLeaf < structuredFeaturesLeaf || !__check_isa_support(__IA_SUPPORT_VECTOR256)) {
        std::fputs("AudioBatch requires an AVX2 CPU and OS support for AVX register state.\n", stderr);
        return 1;
    }

    __cpuid(registers.data(), 1);
    const auto basicFeatures = static_cast<unsigned>(registers[2]);
    // MSVC may emit FMA, MOVBE, POPCNT, and BMI instructions alongside AVX2.
    constexpr unsigned requiredBasicFeatures {(1u << 12u) | (1u << 20u) | (1u << 22u) | (1u << 23u)};
    __cpuidex(registers.data(), structuredFeaturesLeaf, 0);
    const auto extendedFeatures = static_cast<unsigned>(registers[1]);
    constexpr unsigned requiredExtendedFeatures {(1u << 3u) | (1u << 5u) | (1u << 8u)};
    if ((basicFeatures & requiredBasicFeatures) != requiredBasicFeatures
        || (extendedFeatures & requiredExtendedFeatures) != requiredExtendedFeatures)
    {
        std::fputs("The CPU lacks instruction extensions required by MSVC /arch:AVX2.\n", stderr);
        return 1;
    }

    // /arch:AVX512 permits F, DQ, CD, BW, and VL, not just the foundation instructions.
    constexpr unsigned requiredAvx512Features {(1u << 16u) | (1u << 17u) | (1u << 28u) | (1u << 30u) | (1u << 31u)};
    const auto hasAvx512 = __check_isa_support(__IA_SUPPORT_VECTOR512)
        && (extendedFeatures & requiredAvx512Features) == requiredAvx512Features;

    // AVX10 defaults to 256-bit vectors, but MSVC can also emit its scalar and 128-bit AVX-512 instructions.
    constexpr unsigned requiredAvx10Features {__IA_SUPPORT_AVX10 | __IA_SUPPORT_VECTOR256 | __IA_SUPPORT_SV128X};
    if (__check_isa_support(requiredAvx10Features | __IA_SUPPORT_AVX10_2, 2)) {
        std::fputs("AVX10.2;", stdout);
    }
    if (__check_isa_support(requiredAvx10Features, 1)) {
        std::fputs("AVX10.1;", stdout);
    }
    if (hasAvx512) {
        std::fputs("AVX512;", stdout);
    }
    std::fputs("AVX2\n", stdout);

    const std::string_view cpuVendor {vendor.data()};
    if (cpuVendor == "GenuineIntel") {
        std::fputs("INTEL64\n", stdout);
    } else if (cpuVendor == "AuthenticAMD") {
        std::fputs("AMD64\n", stdout);
    } else {
        std::fputs("blend\n", stdout);
    }
    return 0;
}
