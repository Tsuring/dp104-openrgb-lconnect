#include <hidapi.h>

#include "Detector.h"
#include "RGBController_DP104.h"
#include "ResourceManager.h"
#include "SettingsManager.h"
#include "dp104_calibration.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace
{
const uint16_t kDp104VendorId = 0xe560;
const uint16_t kDp104ProductId = 0xe104;
const int kDp104Interface = 1;
const int kDp104UsagePage = 0xff60;
const int kDp104Usage = 0x0061;

bool RestoreConfirmedOrder(dp104::host::ITransport& transport, const char* path)
{
    const json settings = ResourceManager::get()->GetSettingsManager()->GetSettings("DP104Settings");
    if(settings.is_null() || (settings.is_object() && settings.empty())) return true;
    if(!settings.is_object() || !settings.contains("schema") || settings["schema"] != 1 ||
       !settings.contains("verifiedColorOrders") || !settings["verifiedColorOrders"].is_array())
        return false;
    dp104::host::ColorOrder order{};
    bool found = false;
    for(const auto& entry : settings["verifiedColorOrders"])
    {
        if(!entry.is_object() || !entry.contains("hidPath") ||
           !entry["hidPath"].is_string() || entry["hidPath"].get<std::string>() != path)
            continue;
        if(found || !entry.contains("confirmed") || entry["confirmed"] != true ||
           !entry.contains("rgbChannels") || !entry["rgbChannels"].is_array() ||
           entry["rgbChannels"].size() != 3) return false;
        for(unsigned i = 0; i < 3; ++i)
        {
            const auto& value = entry["rgbChannels"][i];
            if(!value.is_number_integer() || value < 0 || value > 2) return false;
            order[i] = value.get<uint8_t>();
        }
        if(!dp104::host::IsColorOrder(order)) return false;
        found = true;
    }
    return !found || dp104::host::ApplyVerifiedColorOrder(transport, order);
}

bool ReadLConnectCompatibility(bool& enabled)
{
    enabled = false;
    const json settings = ResourceManager::get()->GetSettingsManager()->GetSettings("DP104Settings");
    if(settings.is_null() || (settings.is_object() && settings.empty())) return true;
    if(!settings.is_object()) return false;
    if(!settings.contains("lConnectCompatibility")) return true;
    if(!settings["lConnectCompatibility"].is_boolean()) return false;
    enabled = settings["lConnectCompatibility"].get<bool>();
    return true;
}
}

void DetectDP104Controller(hid_device_info* info, const std::string& /*detectorName*/)
{
    if(info == nullptr || info->path == nullptr)
    {
        return;
    }

    hid_device* device = hid_open_path(info->path);
    if(device == nullptr)
    {
        return;
    }

    std::unique_ptr<dp104::openrgb::Dp104HidTransport> transport(
        new dp104::openrgb::Dp104HidTransport(device));
    bool lconnectCompatibility = false;
    if(!ReadLConnectCompatibility(lconnectCompatibility)) return;
    std::unique_ptr<RGBController_DP104> controller;
    {
        /* Serialize calibration/version discovery and the candidate CAPS
         * exchange against the active controller's complete frame transfer. */
        dp104::openrgb::Dp104HidTransport::ProtocolSession protocolSession;
        if(!RestoreConfirmedOrder(*transport, info->path)) return;
        dp104::host::ApplicationVersion applicationVersion{};
        if(!dp104::host::QueryApplicationVersion(*transport, applicationVersion) ||
           !applicationVersion.ready) return;
        const std::string firmwareVersion = std::to_string(applicationVersion.major) + "." +
            std::to_string(applicationVersion.minor) + "." + std::to_string(applicationVersion.patch);
        controller.reset(new RGBController_DP104(
            std::move(transport), firmwareVersion, lconnectCompatibility));

        /* Registration is deliberately gated on the live D2 CAPS response. */
        if(!controller->IsProtocolReady())
        {
            return;
        }
    }

    /* Do not hold the protocol gate across ResourceManager list updates or
     * destruction of a duplicate candidate; its worker must be joinable. */
    ResourceManager::get()->RegisterRGBController(controller.release());
}

REGISTER_HID_DETECTOR_IPU("Ticktype DP104",
                          DetectDP104Controller,
                          kDp104VendorId,
                          kDp104ProductId,
                          kDp104Interface,
                          kDp104UsagePage,
                          kDp104Usage);
