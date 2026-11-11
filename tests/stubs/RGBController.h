#ifndef DP104_TEST_RGB_CONTROLLER_H
#define DP104_TEST_RGB_CONTROLLER_H

#include <string>
#include <vector>

typedef unsigned int RGBColor;

#define RGBGetRValue(rgb) ((rgb) & 0x000000ffu)
#define RGBGetGValue(rgb) (((rgb) >> 8) & 0x000000ffu)
#define RGBGetBValue(rgb) (((rgb) >> 16) & 0x000000ffu)
#define ToRGBColor(r, g, b) ((RGBColor)(((b) << 16) | ((g) << 8) | (r)))

enum
{
    MODE_FLAG_HAS_PER_LED_COLOR = (1u << 5),
    MODE_COLORS_PER_LED = 1u,
    ZONE_TYPE_LINEAR = 1,
    DEVICE_TYPE_KEYBOARD = 5,
    CONTROLLER_FLAG_LOCAL = (1u << 0)
};

typedef int zone_type;
struct mode
{
    std::string name;
    int value;
    unsigned int flags;
    unsigned int color_mode;
};

struct zone
{
    std::string name;
    zone_type type;
    unsigned int leds_min;
    unsigned int leds_max;
    unsigned int leds_count;
    void* matrix_map;
    unsigned int flags;
};

struct led
{
    std::string name;
    unsigned int value;
};

class RGBController
{
public:
    virtual ~RGBController() {}

    virtual void SetupZones() = 0;
    virtual void ResizeZone(int zoneIndex, int newSize) = 0;
    virtual void DeviceUpdateLEDs() = 0;
    virtual void UpdateZoneLEDs(int zoneIndex) = 0;
    virtual void UpdateSingleLED(int ledIndex) = 0;
    virtual void DeviceUpdateMode() = 0;

    void SetupColors()
    {
        colors.assign(leds.size(), 0);
    }

    std::string name;
    std::string vendor;
    std::string description;
    std::string version;
    std::string serial;
    std::string location;
    std::vector<led> leds;
    std::vector<zone> zones;
    std::vector<mode> modes;
    std::vector<RGBColor> colors;
    int type;
    int active_mode;
    unsigned int flags;
};

#endif
