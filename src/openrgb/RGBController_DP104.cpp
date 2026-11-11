#include "RGBController_DP104.h"

#include "Dp104PhysicalLayout.h"
#include "Dp104RuntimeStatus.h"

#include <chrono>
#include <utility>

namespace
{
const std::chrono::milliseconds kRefreshInterval(250);
const std::chrono::milliseconds kRecoveryBackoff(250);
const std::chrono::seconds kRecoveryWindow(10);
const unsigned kMaximumRecoveryFailures = 3u;
/* DEVICE_TYPE_GPU in the pinned OpenRGB API; L-Connect exposes that category. */
const int kLConnectCompatibilityDeviceType = 2;
}

RGBController_DP104::RGBController_DP104(
    std::unique_ptr<dp104::openrgb::Dp104HidTransport> transport,
    const std::string& firmwareVersion,
    bool lconnectCompatibility)
    : transport_(std::move(transport)),
      client_(*transport_),
      lastSuccessfulFrame_(),
      recoveryWindowStarted_(),
      nextRecoveryAttempt_(),
      frameGeneration_(0),
      sentGeneration_(0),
      recoveryFailures_(0),
      stopping_(false),
      haveFrame_(false),
      failed_(false),
      recovering_(false),
      lconnectCompatibility_(lconnectCompatibility),
      protocolReady_(false)
{
    name = "Ticktype DP104";
    vendor = "Ticktype";
    version = firmwareVersion;
    description = lconnectCompatibility
        ? "DP104 104 linear outputs (L-Connect compatibility)"
        : "DP104 host-controlled lighting";
    location = "USB E560:E104 interface 1";
    type = lconnectCompatibility ? kLConnectCompatibilityDeviceType : DEVICE_TYPE_KEYBOARD;
    flags = CONTROLLER_FLAG_LOCAL;

    mode direct;
    direct.name = "Direct";
    direct.value = 0;
    direct.flags = MODE_FLAG_HAS_PER_LED_COLOR;
    direct.color_mode = MODE_COLORS_PER_LED;
    modes.push_back(direct);
    active_mode = 0;
    SetupZones();

    g_dp104RuntimeStatus.protocolReady.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.recoveryState.store(
        dp104::openrgb::DP104_RUNTIME_RECOVERY_READY, std::memory_order_relaxed);
    {
        dp104::openrgb::Dp104HidTransport::ProtocolSession protocolSession;
        if(client_.Connect())
        {
            try
            {
                protocolReady_ = true;
                g_dp104RuntimeStatus.protocolReady.store(1u, std::memory_order_relaxed);
                g_dp104RuntimeStatus.recoveryState.store(
                    dp104::openrgb::DP104_RUNTIME_RECOVERY_READY, std::memory_order_relaxed);
                refreshThread_ = std::thread(&RGBController_DP104::RefreshLoop, this);
            }
            catch(...)
            {
                protocolReady_ = false;
                g_dp104RuntimeStatus.protocolReady.store(0u, std::memory_order_relaxed);
                client_.Disconnect();
            }
        }
    }
}

RGBController_DP104::~RGBController_DP104()
{
    StopRefresh();
    dp104::openrgb::Dp104HidTransport::ProtocolSession protocolSession;
    std::lock_guard<std::mutex> ioLock(ioMutex_);
    protocolReady_ = false;
    g_dp104RuntimeStatus.protocolReady.store(0u, std::memory_order_relaxed);
    client_.Disconnect();
}

bool RGBController_DP104::IsProtocolReady() const
{
    return protocolReady_.load() && client_.IsReady();
}

void RGBController_DP104::SetupZones()
{
    zones.clear();
    leds.clear();

    zone outputZone;
    outputZone.name = lconnectCompatibility_
        ? "DP104 Physical Key LEDs (X-projected)"
        : "DP104 Outputs (unmapped)";
    outputZone.type = ZONE_TYPE_LINEAR;
    outputZone.leds_min = DP104_LED_COUNT;
    outputZone.leds_max = DP104_LED_COUNT;
    outputZone.leds_count = DP104_LED_COUNT;
    outputZone.matrix_map = nullptr;
    outputZone.flags = 0;
    zones.push_back(outputZone);

    for(unsigned int index = 0; index < DP104_LED_COUNT; ++index)
    {
        led output;
        output.name = "DP104 output " + std::to_string(index + 1u);
        output.value = index;
        leds.push_back(output);
    }

    SetupColors();
}

void RGBController_DP104::ResizeZone(int zoneIndex, int newSize)
{
    /* The device has a fixed 104-entry output; resizing cannot change it. */
    if(zoneIndex != 0 || newSize != static_cast<int>(DP104_LED_COUNT))
    {
        return;
    }
}

void RGBController_DP104::DeviceUpdateLEDs()
{
    {
        std::lock_guard<std::mutex> refreshLock(refreshMutex_);
        if((!protocolReady_.load() && !recovering_) || stopping_ || failed_ ||
           colors.size() != DP104_LED_COUNT)
        {
            return;
        }

        for(unsigned int index = 0; index < DP104_LED_COUNT; ++index)
        {
            const RGBColor color = colors[index];
            latestFrame_[index] = dp104::host::Color(
                static_cast<uint8_t>(RGBGetRValue(color)),
                static_cast<uint8_t>(RGBGetGValue(color)),
                static_cast<uint8_t>(RGBGetBValue(color)));
        }
        ++frameGeneration_;
        haveFrame_ = true;
        g_dp104RuntimeStatus.framesQueued.fetch_add(1u, std::memory_order_relaxed);
    }
    refreshCv_.notify_one();
}

void RGBController_DP104::UpdateZoneLEDs(int zoneIndex)
{
    if(zoneIndex == 0)
    {
        DeviceUpdateLEDs();
    }
}

void RGBController_DP104::UpdateSingleLED(int ledIndex)
{
    if(ledIndex >= 0 && static_cast<unsigned int>(ledIndex) < DP104_LED_COUNT)
    {
        DeviceUpdateLEDs();
    }
}

void RGBController_DP104::DeviceUpdateMode()
{
    /* Direct is the only exposed mode; each update already commits a full frame. */
}

void RGBController_DP104::RefreshLoop()
{
    std::unique_lock<std::mutex> refreshLock(refreshMutex_);
    while(!stopping_ && !failed_)
    {
        if(recovering_)
        {
            refreshCv_.wait_until(refreshLock, nextRecoveryAttempt_, [this]()
            {
                return stopping_ || failed_;
            });
            if(stopping_ || failed_)
            {
                break;
            }

            const std::chrono::steady_clock::time_point attemptStarted =
                std::chrono::steady_clock::now();
            if(recoveryFailures_ >= kMaximumRecoveryFailures ||
               attemptStarted - recoveryWindowStarted_ >= kRecoveryWindow)
            {
                failed_ = true;
                recovering_ = false;
                haveFrame_ = false;
                protocolReady_ = false;
                g_dp104RuntimeStatus.protocolReady.store(0u, std::memory_order_relaxed);
                g_dp104RuntimeStatus.recoveryState.store(
                    dp104::openrgb::DP104_RUNTIME_RECOVERY_TERMINAL,
                    std::memory_order_relaxed);
                refreshCv_.notify_all();
                break;
            }

            const std::chrono::steady_clock::time_point recoveryWindowStarted =
                recoveryWindowStarted_;
            refreshLock.unlock();

            bool connected = false;
            bool sent = false;
            bool recoveryWindowExpired = false;
            bool recoveryAttemptCounted = false;
            bool haveRecoveryFrame = false;
            uint64_t sendingGeneration = 0;
            dp104::host::Frame sendingFrame;
            {
                dp104::openrgb::Dp104HidTransport::ProtocolSession protocolSession;
                if(std::chrono::steady_clock::now() - recoveryWindowStarted >=
                   kRecoveryWindow)
                {
                    recoveryWindowExpired = true;
                }
                else
                {
                    g_dp104RuntimeStatus.recoveryAttempts.fetch_add(
                        1u, std::memory_order_relaxed);
                    recoveryAttemptCounted = true;
                    {
                        std::lock_guard<std::mutex> ioLock(ioMutex_);
                        connected = client_.Connect();
                    }

                    if(connected)
                    {
                        if(std::chrono::steady_clock::now() - recoveryWindowStarted >=
                           kRecoveryWindow)
                        {
                            recoveryWindowExpired = true;
                        }
                        else
                        {
                            /* CAPS is complete and no HID call is in flight
                             * here. Read the newest coalesced mailbox value. */
                            refreshLock.lock();
                            haveRecoveryFrame = !stopping_ && !failed_ && haveFrame_;
                            if(haveRecoveryFrame)
                            {
                                sendingGeneration = frameGeneration_;
                                sendingFrame = latestFrame_;
                            }
                            refreshLock.unlock();

                            if(haveRecoveryFrame)
                            {
                                const dp104::host::Frame frameToSend =
                                    dp104::openrgb::Dp104FrameForControllerMode(
                                        sendingFrame, lconnectCompatibility_);
                                g_dp104RuntimeStatus.framesStarted.fetch_add(
                                    1u, std::memory_order_relaxed);
                                std::lock_guard<std::mutex> ioLock(ioMutex_);
                                sent = client_.SetFrame(frameToSend);
                                if(sent)
                                {
                                    g_dp104RuntimeStatus.framesCommitted.fetch_add(
                                        1u, std::memory_order_relaxed);
                                }
                                else
                                {
                                    g_dp104RuntimeStatus.framesFailed.fetch_add(
                                        1u, std::memory_order_relaxed);
                                }
                            }
                        }
                    }
                }
            }

            refreshLock.lock();
            if(stopping_ || failed_)
            {
                break;
            }

            if(recoveryWindowExpired)
            {
                if(recoveryAttemptCounted)
                {
                    ++recoveryFailures_;
                    g_dp104RuntimeStatus.recoveryFailures.fetch_add(
                        1u, std::memory_order_relaxed);
                }
                failed_ = true;
                recovering_ = false;
                haveFrame_ = false;
                protocolReady_ = false;
                g_dp104RuntimeStatus.protocolReady.store(0u, std::memory_order_relaxed);
                g_dp104RuntimeStatus.recoveryState.store(
                    dp104::openrgb::DP104_RUNTIME_RECOVERY_TERMINAL,
                    std::memory_order_relaxed);
                refreshCv_.notify_all();
                break;
            }

            if(sent)
            {
                recovering_ = false;
                recoveryFailures_ = 0u;
                sentGeneration_ = sendingGeneration;
                lastSuccessfulFrame_ = std::chrono::steady_clock::now();
                protocolReady_ = true;
                g_dp104RuntimeStatus.protocolReady.store(1u, std::memory_order_relaxed);
                g_dp104RuntimeStatus.recoverySucceeded.fetch_add(1u,
                                                                  std::memory_order_relaxed);
                g_dp104RuntimeStatus.recoveryState.store(
                    dp104::openrgb::DP104_RUNTIME_RECOVERY_READY,
                    std::memory_order_relaxed);
                continue;
            }

            ++recoveryFailures_;
            g_dp104RuntimeStatus.recoveryFailures.fetch_add(1u, std::memory_order_relaxed);
            protocolReady_ = false;
            g_dp104RuntimeStatus.protocolReady.store(0u, std::memory_order_relaxed);
            const std::chrono::steady_clock::time_point failedAt =
                std::chrono::steady_clock::now();
            if(recoveryFailures_ >= kMaximumRecoveryFailures ||
               failedAt - recoveryWindowStarted_ >= kRecoveryWindow)
            {
                failed_ = true;
                recovering_ = false;
                haveFrame_ = false;
                g_dp104RuntimeStatus.recoveryState.store(
                    dp104::openrgb::DP104_RUNTIME_RECOVERY_TERMINAL,
                    std::memory_order_relaxed);
                refreshCv_.notify_all();
                break;
            }

            nextRecoveryAttempt_ = failedAt + kRecoveryBackoff;
            g_dp104RuntimeStatus.recoveryState.store(
                dp104::openrgb::DP104_RUNTIME_RECOVERY_WAITING,
                std::memory_order_relaxed);
            continue;
        }

        if(!haveFrame_)
        {
            refreshCv_.wait(refreshLock, [this]()
            {
                return stopping_ || failed_ || haveFrame_;
            });
            continue;
        }

        if(frameGeneration_ == sentGeneration_)
        {
            const std::chrono::steady_clock::time_point due =
                lastSuccessfulFrame_ + kRefreshInterval;
            const bool updated = refreshCv_.wait_until(refreshLock, due, [this]()
            {
                return stopping_ || failed_ ||
                       frameGeneration_ != sentGeneration_;
            });
            if(stopping_ || failed_)
            {
                break;
            }
            if(updated)
            {
                continue;
            }
        }

        const uint64_t sendingGeneration = frameGeneration_;
        const dp104::host::Frame sendingFrame = latestFrame_;
        const dp104::host::Frame frameToSend =
            dp104::openrgb::Dp104FrameForControllerMode(sendingFrame,
                                                        lconnectCompatibility_);

        refreshLock.unlock();
        bool sent = false;
        bool attempted = false;
        {
            dp104::openrgb::Dp104HidTransport::ProtocolSession protocolSession;
            std::lock_guard<std::mutex> ioLock(ioMutex_);
            if(protocolReady_.load() && client_.IsReady())
            {
                attempted = true;
                g_dp104RuntimeStatus.framesStarted.fetch_add(1u,
                                                              std::memory_order_relaxed);
                sent = client_.SetFrame(frameToSend);
            }
        }

        if(sent)
        {
            g_dp104RuntimeStatus.framesCommitted.fetch_add(1u, std::memory_order_relaxed);
        }
        else
        {
            if(attempted)
            {
                g_dp104RuntimeStatus.framesFailed.fetch_add(1u, std::memory_order_relaxed);
            }
            MarkFrameFailure();
        }

        refreshLock.lock();
        if(stopping_ || failed_)
        {
            break;
        }
        if(!sent)
        {
            continue;
        }
        sentGeneration_ = sendingGeneration;
        lastSuccessfulFrame_ = std::chrono::steady_clock::now();
    }
}

void RGBController_DP104::StopRefresh()
{
    {
        std::lock_guard<std::mutex> lock(refreshMutex_);
        stopping_ = true;
    }
    refreshCv_.notify_all();
    if(refreshThread_.joinable())
    {
        refreshThread_.join();
    }
}

void RGBController_DP104::MarkFrameFailure()
{
    {
        std::lock_guard<std::mutex> lock(refreshMutex_);
        if(stopping_ || failed_ || recovering_)
        {
            return;
        }
        recovering_ = true;
        recoveryFailures_ = 0u;
        recoveryWindowStarted_ = std::chrono::steady_clock::now();
        nextRecoveryAttempt_ = recoveryWindowStarted_ + kRecoveryBackoff;
        g_dp104RuntimeStatus.recoveryState.store(
            dp104::openrgb::DP104_RUNTIME_RECOVERY_WAITING,
            std::memory_order_relaxed);
    }
    protocolReady_ = false;
    g_dp104RuntimeStatus.protocolReady.store(0u, std::memory_order_relaxed);
    refreshCv_.notify_all();
}
