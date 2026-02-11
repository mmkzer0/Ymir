#include <catch2/catch_test_macros.hpp>

#include <ymir/sys/saturn.hpp>

namespace ymir {

struct SaturnExecutableRangeTestAccess {
    static bool IsExecutableMainBusRange(uint32 address, uint32 size) {
        return Saturn::IsExecutableMainBusRange(address, size);
    }
};

} // namespace ymir

namespace saturn_block_cache_observer_tests {

using namespace ymir;

TEST_CASE("Saturn executable range filter handles boundaries and aliases", "[saturn][block-cache][observer]") {
    struct RangeCase {
        uint32 address;
        uint32 size;
        bool expected;
    };

    static constexpr RangeCase cases[] = {
        {0x000'0000, 0, false},
        {0x020'0000, 1, true},   // Low WRAM start
        {0x02F'FFFF, 1, true},   // Low WRAM end
        {0x01F'FFFF, 1, false},  // Low WRAM below
        {0x030'0000, 1, false},  // Low WRAM above

        {0x5A0'0000, 1, true},   // SCSP WRAM start
        {0x5A7'FFFF, 1, true},   // SCSP WRAM end
        {0x5A8'0000, 1, false},  // SCSP WRAM above

        {0x5C0'0000, 1, true},   // VDP1 VRAM start
        {0x5CF'FFFF, 1, true},   // VDP1 framebuffer RAM end
        {0x5D0'0000, 1, false},  // VDP1 ranges above

        {0x5E0'0000, 1, true},   // VDP2 VRAM start
        {0x5F7'FFFF, 1, true},   // VDP2 CRAM end
        {0x5F8'0000, 1, false},  // VDP2 ranges above

        {0x600'0000, 1, true},   // High WRAM start
        {0x7FF'FFFF, 1, true},   // High WRAM end
        {0x01F'FFF8, 0x10, true}, // Crosses into low WRAM

        {0x820'0000, 1, true},   // Aliases to low WRAM by 27-bit mask
        {0x880'0000, 1, false},  // Aliases to 0x0000000 (non-executable)
    };

    for (const RangeCase &testCase : cases) {
        CAPTURE(testCase.address, testCase.size, testCase.expected);
        CHECK(SaturnExecutableRangeTestAccess::IsExecutableMainBusRange(testCase.address, testCase.size) ==
              testCase.expected);
    }
}

} // namespace saturn_block_cache_observer_tests
