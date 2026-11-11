#ifndef RGB_CONTROLLER_DP104_H
#define RGB_CONTROLLER_DP104_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "Dp104HidTransport.h"
#include "RGBController.h"

class RGBController_DP104 final : public RGBController
{
public:
    explicit RGBController_DP104(std::unique_ptr<dp104::openrgb::Dp104HidTransport> transport,
                                 const std::string& firmwareVersion = std::string(),
                                 bool lconnectCompatibility = false);
    ~RGBController_DP104() override;

    RGBController_DP104(const RGBController_DP104&) = delete;
    RGBController_DP104& operator=(const RGBController_DP104&) = delete;

    bool IsProtocolReady() const;

    void SetupZones() override;
    void ResizeZone(int zone, int newSize) override;
    void DeviceUpdateLEDs() override;
    void UpdateZoneLEDs(int zone) override;
    void UpdateSingleLED(int led) override;
    void DeviceUpdateMode() override;

private:
    void RefreshLoop();
    void StopRefresh();
    void MarkFrameFailure();

    /* Declaration order keeps the transport alive through Client destruction. */
    std::unique_ptr<dp104::openrgb::Dp104HidTransport> transport_;
    dp104::host::Client client_;
    std::mutex ioMutex_;
    std::mutex refreshMutex_;
    std::condition_variable refreshCv_;
    std::thread refreshThread_;
    dp104::host::Frame latestFrame_;
    std::chrono::steady_clock::time_point lastSuccessfulFrame_;
    std::chrono::steady_clock::time_point recoveryWindowStarted_;
    std::chrono::steady_clock::time_point nextRecoveryAttempt_;
    uint64_t frameGeneration_;
    uint64_t sentGeneration_;
    unsigned recoveryFailures_;
    bool stopping_;
    bool haveFrame_;
    bool failed_;
    bool recovering_;
    bool lconnectCompatibility_;
    std::atomic<bool> protocolReady_;
};

#endif
