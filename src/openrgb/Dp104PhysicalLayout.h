#ifndef DP104_PHYSICAL_LAYOUT_H
#define DP104_PHYSICAL_LAYOUT_H

#include <cstdint>

#include "dp104_host_client.h"

namespace dp104
{
namespace openrgb
{

struct Dp104PhysicalCoordinate
{
    uint8_t x;
    uint8_t y;
};

/* Physical output coordinates in PLC LED-index order. The table matches the
 * verified matrix-to-key map and the physical Esc, Space, and Enter anchors. */
static constexpr Dp104PhysicalCoordinate kDp104PhysicalCoordinates[DP104_LED_COUNT] =
{
    {0u, 0u}, // 0
    {21u, 0u}, // 1
    {31u, 0u}, // 2
    {42u, 0u}, // 3
    {52u, 0u}, // 4
    {68u, 0u}, // 5
    {78u, 0u}, // 6
    {89u, 0u}, // 7
    {99u, 0u}, // 8
    {115u, 0u}, // 9
    {125u, 0u}, // 10
    {135u, 0u}, // 11
    {146u, 0u}, // 12
    {159u, 0u}, // 13
    {169u, 0u}, // 14
    {180u, 0u}, // 15
    {224u, 15u}, // 16
    {214u, 15u}, // 17
    {203u, 15u}, // 18
    {193u, 15u}, // 19
    {180u, 15u}, // 20
    {169u, 15u}, // 21
    {159u, 15u}, // 22
    {141u, 15u}, // 23
    {125u, 15u}, // 24
    {115u, 15u}, // 25
    {104u, 15u}, // 26
    {94u, 15u}, // 27
    {83u, 15u}, // 28
    {73u, 15u}, // 29
    {63u, 15u}, // 30
    {52u, 15u}, // 31
    {42u, 15u}, // 32
    {31u, 15u}, // 33
    {21u, 15u}, // 34
    {10u, 15u}, // 35
    {0u, 15u}, // 36
    {3u, 27u}, // 37
    {16u, 27u}, // 38
    {26u, 27u}, // 39
    {36u, 27u}, // 40
    {47u, 27u}, // 41
    {57u, 27u}, // 42
    {68u, 27u}, // 43
    {78u, 27u}, // 44
    {89u, 27u}, // 45
    {99u, 27u}, // 46
    {109u, 27u}, // 47
    {120u, 27u}, // 48
    {130u, 27u}, // 49
    {143u, 27u}, // 50
    {159u, 27u}, // 51
    {169u, 27u}, // 52
    {180u, 27u}, // 53
    {193u, 27u}, // 54
    {203u, 27u}, // 55
    {214u, 27u}, // 56
    {221u, 30u}, // 57
    {214u, 40u}, // 58
    {203u, 40u}, // 59
    {193u, 40u}, // 60
    {139u, 40u}, // 61
    {122u, 40u}, // 62
    {112u, 40u}, // 63
    {102u, 40u}, // 64
    {91u, 40u}, // 65
    {81u, 40u}, // 66
    {70u, 40u}, // 67
    {60u, 40u}, // 68
    {49u, 40u}, // 69
    {39u, 40u}, // 70
    {29u, 40u}, // 71
    {18u, 40u}, // 72
    {4u, 40u}, // 73
    {7u, 52u}, // 74
    {23u, 52u}, // 75
    {34u, 52u}, // 76
    {44u, 52u}, // 77
    {55u, 52u}, // 78
    {65u, 52u}, // 79
    {76u, 52u}, // 80
    {86u, 52u}, // 81
    {96u, 52u}, // 82
    {107u, 52u}, // 83
    {117u, 52u}, // 84
    {137u, 52u}, // 85
    {169u, 52u}, // 86
    {193u, 52u}, // 87
    {203u, 52u}, // 88
    {214u, 52u}, // 89
    {221u, 55u}, // 90
    {214u, 64u}, // 91
    {198u, 64u}, // 92
    {180u, 64u}, // 93
    {169u, 64u}, // 94
    {159u, 64u}, // 95
    {145u, 64u}, // 96
    {132u, 64u}, // 97
    {119u, 64u}, // 98
    {105u, 64u}, // 99
    {66u, 64u}, // 100
    {27u, 64u}, // 101
    {14u, 64u}, // 102
    {1u, 64u}, // 103
};

constexpr unsigned Dp104LinearSourceIndexForX(unsigned x)
{
    return (x * 103u + 112u) / 224u;
}

static_assert(kDp104PhysicalCoordinates[0].x == 0u &&
              kDp104PhysicalCoordinates[0].y == 0u &&
              Dp104LinearSourceIndexForX(kDp104PhysicalCoordinates[0].x) == 0u,
              "DP104 Esc must use the leftmost linear sample");
static_assert(kDp104PhysicalCoordinates[100].x == 66u &&
              kDp104PhysicalCoordinates[100].y == 64u &&
              Dp104LinearSourceIndexForX(kDp104PhysicalCoordinates[100].x) == 30u,
              "DP104 Space must map from linear source 30");
static_assert(kDp104PhysicalCoordinates[61].x == 139u &&
              kDp104PhysicalCoordinates[61].y == 40u &&
              Dp104LinearSourceIndexForX(kDp104PhysicalCoordinates[61].x) == 64u,
              "DP104 Enter must map from linear source 64");
static_assert(Dp104LinearSourceIndexForX(224u) == 103u,
              "the rightmost physical x coordinate must use source 103");

inline host::Frame Dp104FrameForControllerMode(const host::Frame& linearFrame,
                                               bool lconnectCompatibility)
{
    if(!lconnectCompatibility)
    {
        return linearFrame;
    }

    host::Frame physicalFrame;
    for(unsigned ledIndex = 0; ledIndex < DP104_LED_COUNT; ++ledIndex)
    {
        const unsigned sourceIndex =
            Dp104LinearSourceIndexForX(kDp104PhysicalCoordinates[ledIndex].x);
        physicalFrame[ledIndex] = linearFrame[sourceIndex];
    }
    return physicalFrame;
}

} // namespace openrgb
} // namespace dp104

#endif
