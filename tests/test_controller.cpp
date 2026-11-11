#include "RGBController_DP104.h"

#include "Dp104PhysicalLayout.h"
#include "Dp104RuntimeStatus.h"

#include "dp104_receiver.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <hidapi.h>

struct FakeHidContext
{
    FakeHidContext()
        : now(0),
          begins(0),
          published(0),
          released(0),
          ready(true),
          failAckOperation(0),
          failAckCount(0),
          failReadPending(false),
          blockNextRead(false),
          blockReadOperation(0),
          blockReadOperationEnabled(false),
          blockReadOperationUsed(false),
          readBlocked(false),
          releaseBlockedRead(false),
          awaitingRead(false),
          closed(false),
          writesAfterClose(0),
          overlappingExchanges(0),
          closesWithReadPending(0)
    {
        visible.fill(0);
        response.fill(0);
        dp104_output_hooks hooks = {this, Ready, Publish, Release};
        dp104_receiver_init(&receiver, hooks);
    }

    static int Ready(void* value)
    {
        return static_cast<FakeHidContext*>(value)->ready ? 1 : 0;
    }

    static int Publish(void* value, const uint8_t* rgb, unsigned leds)
    {
        FakeHidContext* context = static_cast<FakeHidContext*>(value);
        if(leds != DP104_LED_COUNT)
        {
            return 0;
        }
        std::memcpy(context->visible.data(), rgb, context->visible.size());
        ++context->published;
        context->cv.notify_all();
        return 1;
    }

    static void Release(void* value)
    {
        FakeHidContext* context = static_cast<FakeHidContext*>(value);
        ++context->released;
        context->cv.notify_all();
    }

    dp104_receiver receiver;
    std::array<uint8_t, DP104_FRAME_BYTES> visible;
    std::array<uint8_t, DP104_REPORT_BYTES> response;
    uint32_t now;
    unsigned begins;
    unsigned published;
    unsigned released;
    bool ready;
    uint8_t failAckOperation;
    unsigned failAckCount;
    bool failReadPending;
    bool blockNextRead;
    uint8_t blockReadOperation;
    bool blockReadOperationEnabled;
    bool blockReadOperationUsed;
    bool readBlocked;
    bool releaseBlockedRead;
    bool awaitingRead;
    bool closed;
    unsigned writesAfterClose;
    unsigned overlappingExchanges;
    unsigned closesWithReadPending;
    std::vector<unsigned int> events;
    std::vector<std::array<uint8_t, DP104_REPORT_BYTES> > reports;
    std::mutex mutex;
    std::condition_variable cv;
};

struct hid_device_
{
    FakeHidContext* context;
};

namespace
{
int g_failures = 0;
const unsigned int kCloseEvent = 0x100u;

void Check(bool condition, const char* expression, int line)
{
    if(!condition)
    {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        ++g_failures;
    }
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

bool WaitForPublished(FakeHidContext& context, unsigned count, unsigned timeoutMs)
{
    std::unique_lock<std::mutex> lock(context.mutex);
    return context.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&context, count]()
    {
        return context.published >= count;
    });
}

void FillFrame(RGBController_DP104& controller, unsigned seed = 0)
{
    for(unsigned index = 0; index < DP104_LED_COUNT; ++index)
    {
        controller.colors[index] = ToRGBColor(index * 3u + 1u + seed,
                                              255u - index - seed,
                                              index ^ (0x5au + seed));
    }
}

bool WaitForReadBlocked(FakeHidContext& context, unsigned timeoutMs)
{
    std::unique_lock<std::mutex> lock(context.mutex);
    return context.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&context]()
    {
        return context.readBlocked;
    });
}

void ReleaseBlockedRead(FakeHidContext& context)
{
    std::lock_guard<std::mutex> lock(context.mutex);
    context.releaseBlockedRead = true;
    context.cv.notify_all();
}

bool UpdateReturnsQuickly(FakeHidContext& context, RGBController_DP104& controller,
                          unsigned timeoutMs = 150)
{
    std::atomic<bool> completed(false);
    std::thread update([&controller, &completed]()
    {
        controller.DeviceUpdateLEDs();
        completed.store(true);
    });
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while(!completed.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool returned = completed.load();
    if(!returned)
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        if(context.readBlocked)
        {
            context.releaseBlockedRead = true;
            context.cv.notify_all();
        }
    }
    update.join();
    return returned;
}

dp104::host::Frame HostFrameFromController(const RGBController_DP104& controller)
{
    dp104::host::Frame frame;
    for(unsigned index = 0; index < DP104_LED_COUNT; ++index)
    {
        const RGBColor color = controller.colors[index];
        frame[index] = dp104::host::Color(
            static_cast<uint8_t>(RGBGetRValue(color)),
            static_cast<uint8_t>(RGBGetGValue(color)),
            static_cast<uint8_t>(RGBGetBValue(color)));
    }
    return frame;
}

bool VisibleMatchesFrame(const FakeHidContext& context,
                         const dp104::host::Frame& frame)
{
    if(context.visible.size() != DP104_FRAME_BYTES)
    {
        return false;
    }
    for(unsigned index = 0; index < DP104_LED_COUNT; ++index)
    {
        if(context.visible[index * 3u] != frame[index].r ||
           context.visible[index * 3u + 1u] != frame[index].g ||
           context.visible[index * 3u + 2u] != frame[index].b)
        {
            return false;
        }
    }
    return true;
}

bool VisibleMatches(const FakeHidContext& context,
                    const RGBController_DP104& controller)
{
    return controller.colors.size() == DP104_LED_COUNT &&
           VisibleMatchesFrame(context, HostFrameFromController(controller));
}

bool VisibleMatchesProjected(const FakeHidContext& context,
                              const RGBController_DP104& controller)
{
    return controller.colors.size() == DP104_LED_COUNT &&
           VisibleMatchesFrame(
               context,
               dp104::openrgb::Dp104FrameForControllerMode(
                   HostFrameFromController(controller), true));
}

bool WaitForVisibleMatches(FakeHidContext& context,
                          const RGBController_DP104& controller,
                          unsigned timeoutMs)
{
    std::unique_lock<std::mutex> lock(context.mutex);
    return context.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&context, &controller]()
    {
        return VisibleMatches(context, controller);
    });
}

bool WaitForVisibleMatchesProjected(FakeHidContext& context,
                                   const RGBController_DP104& controller,
                                   unsigned timeoutMs)
{
    std::unique_lock<std::mutex> lock(context.mutex);
    return context.cv.wait_for(lock,
                               std::chrono::milliseconds(timeoutMs),
                               [&context, &controller]()
    {
        return VisibleMatchesProjected(context, controller);
    });
}

bool WaitForIoIdleAfterPublishes(FakeHidContext& context,
                                 unsigned publishCount,
                                 unsigned timeoutMs)
{
    std::unique_lock<std::mutex> lock(context.mutex);
    return context.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&context, publishCount]()
    {
        return context.published >= publishCount && !context.awaitingRead && !context.readBlocked;
    });
}

bool WaitForRuntimeWord(std::atomic<uint32_t>& word, uint32_t expected, unsigned timeoutMs)
{
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while(std::chrono::steady_clock::now() < deadline)
    {
        if(word.load(std::memory_order_acquire) == expected)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return word.load(std::memory_order_acquire) == expected;
}

unsigned CountOperation(const FakeHidContext& context, uint8_t operation)
{
    unsigned count = 0u;
    for(std::size_t index = 0; index < context.reports.size(); ++index)
    {
        if(context.reports[index][1] == operation)
        {
            ++count;
        }
    }
    return count;
}

bool WaitForOperation(FakeHidContext& context,
                      uint8_t operation,
                      unsigned count,
                      unsigned timeoutMs)
{
    std::unique_lock<std::mutex> lock(context.mutex);
    return context.cv.wait_for(lock,
                               std::chrono::milliseconds(timeoutMs),
                               [&context, operation, count]()
    {
        return CountOperation(context, operation) >= count;
    });
}

void TestNoInitialFrameRefreshAndDestructorOrdering()
{
    FakeHidContext context;
    hid_device_ device = {&context};
    std::unique_ptr<RGBController_DP104> controller(
        new RGBController_DP104(std::unique_ptr<dp104::openrgb::Dp104HidTransport>(
            new dp104::openrgb::Dp104HidTransport(&device))));

    CHECK(controller->IsProtocolReady());
    CHECK(controller->type == DEVICE_TYPE_KEYBOARD);
    CHECK(controller->description == "DP104 host-controlled lighting");
    CHECK(controller->zones.size() == 1u);
    CHECK(controller->zones[0].type == ZONE_TYPE_LINEAR);
    CHECK(controller->zones[0].leds_count == DP104_LED_COUNT);
    CHECK(controller->leds.size() == DP104_LED_COUNT);
    CHECK(controller->modes.size() == 1u);
    CHECK(controller->modes[0].name == "Direct");

    controller->ResizeZone(0, 12);
    controller->UpdateSingleLED(-1);
    controller->UpdateSingleLED(DP104_LED_COUNT);
    controller->UpdateZoneLEDs(1);
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        CHECK(context.events.size() == 1u);
        CHECK(context.events[0] == DP104_OP_CAPS);
        CHECK(context.begins == 0u);
        CHECK(context.published == 0u);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(320));
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        CHECK(context.events.size() == 1u);
        CHECK(context.begins == 0u);
        CHECK(context.published == 0u);
    }

    FillFrame(*controller);
    controller->DeviceUpdateLEDs();
    CHECK(WaitForPublished(context, 2u, 900u));
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        CHECK(context.published >= 2u);
        CHECK(VisibleMatches(context, *controller));
    }

    controller.reset();
    std::size_t finalEventCount = 0;
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        CHECK(context.closed);
        CHECK(context.released == 1u);
        CHECK(context.writesAfterClose == 0u);
        CHECK(context.events.size() >= 3u);
        CHECK(context.events.back() == kCloseEvent);
        CHECK(context.events[context.events.size() - 2u] == DP104_OP_RELEASE);
        finalEventCount = context.events.size();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        CHECK(context.events.size() == finalEventCount);
        CHECK(context.writesAfterClose == 0u);
    }
}

void TestTransientFrameFailureRechecksCapsAndSendsLatestFrame()
{
    FakeHidContext context;
    context.failAckOperation = DP104_OP_DATA;
    context.failAckCount = 1u;
    hid_device_ device = {&context};
    const uint32_t queuedBefore =
        g_dp104RuntimeStatus.framesQueued.load(std::memory_order_relaxed);
    const uint32_t committedBefore =
        g_dp104RuntimeStatus.framesCommitted.load(std::memory_order_relaxed);
    const uint32_t failedBefore =
        g_dp104RuntimeStatus.framesFailed.load(std::memory_order_relaxed);
    const uint32_t attemptsBefore =
        g_dp104RuntimeStatus.recoveryAttempts.load(std::memory_order_relaxed);
    const uint32_t recoveriesBefore =
        g_dp104RuntimeStatus.recoverySucceeded.load(std::memory_order_relaxed);
    std::unique_ptr<RGBController_DP104> controller(
        new RGBController_DP104(
            std::unique_ptr<dp104::openrgb::Dp104HidTransport>(
                new dp104::openrgb::Dp104HidTransport(&device)),
            "1.2.1",
            true));

    {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.blockReadOperation = DP104_OP_CAPS;
        context.blockReadOperationEnabled = true;
    }

    FillFrame(*controller, 1u);
    controller->DeviceUpdateLEDs();
    CHECK(WaitForRuntimeWord(g_dp104RuntimeStatus.recoveryState,
                             dp104::openrgb::DP104_RUNTIME_RECOVERY_WAITING,
                             1200u));
    CHECK(!controller->IsProtocolReady());
    CHECK(WaitForOperation(context, DP104_OP_RELEASE, 1u, 100u));

    FillFrame(*controller, 23u);
    CHECK(UpdateReturnsQuickly(context, *controller));
    CHECK(WaitForReadBlocked(context, 1000u));
    FillFrame(*controller, 35u);
    CHECK(UpdateReturnsQuickly(context, *controller));
    ReleaseBlockedRead(context);
    CHECK(WaitForVisibleMatchesProjected(context, *controller, 1800u));
    CHECK(WaitForIoIdleAfterPublishes(context, 1u, 500u));
    CHECK(controller->IsProtocolReady());

    {
        std::lock_guard<std::mutex> lock(context.mutex);
        CHECK(context.begins == 2u);
        CHECK(context.published == 1u);
        CHECK(context.released == 0u);
        CHECK(CountOperation(context, DP104_OP_CAPS) == 2u);
        CHECK(CountOperation(context, DP104_OP_RELEASE) == 1u);

        std::size_t firstBeginIndex = context.reports.size();
        std::size_t recoveryCapsIndex = context.reports.size();
        std::size_t recoveryBeginIndex = context.reports.size();
        for(std::size_t index = 0; index < context.reports.size(); ++index)
        {
            if(context.reports[index][1] == DP104_OP_BEGIN &&
               firstBeginIndex == context.reports.size())
            {
                firstBeginIndex = index;
            }
            else if(context.reports[index][1] == DP104_OP_CAPS)
            {
                recoveryCapsIndex = index;
            }
            else if(context.reports[index][1] == DP104_OP_BEGIN)
            {
                recoveryBeginIndex = index;
            }
        }
        CHECK(firstBeginIndex < context.reports.size());
        CHECK(recoveryCapsIndex < recoveryBeginIndex);
        CHECK(recoveryBeginIndex < context.reports.size());
        if(firstBeginIndex < context.reports.size() &&
           recoveryCapsIndex < recoveryBeginIndex &&
           recoveryBeginIndex < context.reports.size())
        {
            const std::array<uint8_t, DP104_REPORT_BYTES>& failedBegin =
                context.reports[firstBeginIndex];
            const std::array<uint8_t, DP104_REPORT_BYTES>& freshCaps =
                context.reports[recoveryCapsIndex];
            const std::array<uint8_t, DP104_REPORT_BYTES>& latestBegin =
                context.reports[recoveryBeginIndex];
            CHECK(recoveryBeginIndex == recoveryCapsIndex + 1u);
            CHECK(failedBegin[2] != freshCaps[2]);
            CHECK(failedBegin[2] != latestBegin[2]);
            CHECK(freshCaps[2] != latestBegin[2]);
            CHECK(std::memcmp(&failedBegin[8], &latestBegin[8], 4u) != 0);
        }
    }
    CHECK(g_dp104RuntimeStatus.framesQueued.load(std::memory_order_relaxed) ==
          queuedBefore + 3u);
    CHECK(g_dp104RuntimeStatus.framesCommitted.load(std::memory_order_relaxed) ==
          committedBefore + 1u);
    CHECK(g_dp104RuntimeStatus.framesFailed.load(std::memory_order_relaxed) ==
          failedBefore + 1u);
    CHECK(g_dp104RuntimeStatus.recoveryAttempts.load(std::memory_order_relaxed) ==
          attemptsBefore + 1u);
    CHECK(g_dp104RuntimeStatus.recoverySucceeded.load(std::memory_order_relaxed) ==
          recoveriesBefore + 1u);
    CHECK(g_dp104RuntimeStatus.recoveryState.load(std::memory_order_relaxed) ==
          dp104::openrgb::DP104_RUNTIME_RECOVERY_READY);

    controller.reset();
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        CHECK(context.closed);
        CHECK(context.released == 1u);
        CHECK(CountOperation(context, DP104_OP_RELEASE) == 2u);
        CHECK(context.writesAfterClose == 0u);
        CHECK(context.events.back() == kCloseEvent);
    }
}

void TestRecoveryFailuresStopAfterThreeAttemptsAcrossCapsAndFrames()
{
    FakeHidContext context;
    context.failAckOperation = DP104_OP_DATA;
    context.failAckCount = 1u;
    hid_device_ device = {&context};
    const uint32_t attemptsBefore =
        g_dp104RuntimeStatus.recoveryAttempts.load(std::memory_order_relaxed);
    const uint32_t failuresBefore =
        g_dp104RuntimeStatus.recoveryFailures.load(std::memory_order_relaxed);
    const uint32_t frameFailuresBefore =
        g_dp104RuntimeStatus.framesFailed.load(std::memory_order_relaxed);
    std::unique_ptr<RGBController_DP104> controller(
        new RGBController_DP104(std::unique_ptr<dp104::openrgb::Dp104HidTransport>(
            new dp104::openrgb::Dp104HidTransport(&device))));

    FillFrame(*controller, 7u);
    controller->DeviceUpdateLEDs();
    CHECK(WaitForRuntimeWord(g_dp104RuntimeStatus.recoveryState,
                             dp104::openrgb::DP104_RUNTIME_RECOVERY_WAITING,
                             1200u));
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.failAckOperation = DP104_OP_CAPS;
        context.failAckCount = 1u;
    }
    CHECK(WaitForRuntimeWord(g_dp104RuntimeStatus.recoveryFailures,
                             failuresBefore + 1u,
                             1000u));
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.failAckOperation = DP104_OP_DATA;
        context.failAckCount = 2u;
    }
    CHECK(WaitForRuntimeWord(g_dp104RuntimeStatus.recoveryState,
                             dp104::openrgb::DP104_RUNTIME_RECOVERY_TERMINAL,
                             3000u));
    CHECK(WaitForOperation(context, DP104_OP_RELEASE, 3u, 500u));
    CHECK(!controller->IsProtocolReady());

    unsigned capsBeforeUpdate = 0u;
    unsigned beginsBeforeUpdate = 0u;
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        CHECK(context.begins == 3u);
        CHECK(context.published == 0u);
        CHECK(context.released == 0u);
        CHECK(CountOperation(context, DP104_OP_CAPS) == 4u);
        CHECK(CountOperation(context, DP104_OP_RELEASE) == 3u);
        capsBeforeUpdate = CountOperation(context, DP104_OP_CAPS);
        beginsBeforeUpdate = CountOperation(context, DP104_OP_BEGIN);
    }
    const uint32_t queuedBeforeTerminalUpdate =
        g_dp104RuntimeStatus.framesQueued.load(std::memory_order_relaxed);
    FillFrame(*controller, 29u);
    controller->DeviceUpdateLEDs();
    std::this_thread::sleep_for(std::chrono::milliseconds(320));
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        CHECK(CountOperation(context, DP104_OP_CAPS) == capsBeforeUpdate);
        CHECK(CountOperation(context, DP104_OP_BEGIN) == beginsBeforeUpdate);
        CHECK(context.published == 0u);
    }
    CHECK(g_dp104RuntimeStatus.framesQueued.load(std::memory_order_relaxed) ==
          queuedBeforeTerminalUpdate);
    CHECK(g_dp104RuntimeStatus.recoveryAttempts.load(std::memory_order_relaxed) ==
          attemptsBefore + 3u);
    CHECK(g_dp104RuntimeStatus.recoveryFailures.load(std::memory_order_relaxed) ==
          failuresBefore + 3u);
    CHECK(g_dp104RuntimeStatus.framesFailed.load(std::memory_order_relaxed) ==
          frameFailuresBefore + 3u);
    CHECK(g_dp104RuntimeStatus.recoveryState.load(std::memory_order_relaxed) ==
          dp104::openrgb::DP104_RUNTIME_RECOVERY_TERMINAL);

    controller.reset();
    std::lock_guard<std::mutex> lock(context.mutex);
    CHECK(context.closed);
    CHECK(context.released == 0u);
    CHECK(CountOperation(context, DP104_OP_RELEASE) == 3u);
    CHECK(context.events.back() == kCloseEvent);
}

void TestRecoveryHardWindowExpiresAfterTenSeconds()
{
    FakeHidContext context;
    context.failAckOperation = DP104_OP_DATA;
    context.failAckCount = 1u;
    hid_device_ device = {&context};
    const uint32_t attemptsBefore =
        g_dp104RuntimeStatus.recoveryAttempts.load(std::memory_order_relaxed);
    const uint32_t failuresBefore =
        g_dp104RuntimeStatus.recoveryFailures.load(std::memory_order_relaxed);
    std::unique_ptr<RGBController_DP104> controller(
        new RGBController_DP104(std::unique_ptr<dp104::openrgb::Dp104HidTransport>(
            new dp104::openrgb::Dp104HidTransport(&device))));
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.blockReadOperation = DP104_OP_CAPS;
        context.blockReadOperationEnabled = true;
    }

    FillFrame(*controller, 11u);
    controller->DeviceUpdateLEDs();
    CHECK(WaitForRuntimeWord(g_dp104RuntimeStatus.recoveryState,
                             dp104::openrgb::DP104_RUNTIME_RECOVERY_WAITING,
                             1200u));
    CHECK(WaitForOperation(context, DP104_OP_RELEASE, 1u, 100u));
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.failAckOperation = DP104_OP_CAPS;
        context.failAckCount = 1u;
    }
    CHECK(WaitForReadBlocked(context, 1000u));
    std::this_thread::sleep_for(std::chrono::milliseconds(10100));
    ReleaseBlockedRead(context);
    CHECK(WaitForRuntimeWord(g_dp104RuntimeStatus.recoveryState,
                             dp104::openrgb::DP104_RUNTIME_RECOVERY_TERMINAL,
                             1000u));
    CHECK(!controller->IsProtocolReady());
    CHECK(g_dp104RuntimeStatus.recoveryAttempts.load(std::memory_order_relaxed) ==
          attemptsBefore + 1u);
    CHECK(g_dp104RuntimeStatus.recoveryFailures.load(std::memory_order_relaxed) ==
          failuresBefore + 1u);

    {
        std::lock_guard<std::mutex> lock(context.mutex);
        CHECK(context.begins == 1u);
        CHECK(context.published == 0u);
        CHECK(CountOperation(context, DP104_OP_CAPS) == 2u);
        CHECK(CountOperation(context, DP104_OP_RELEASE) == 1u);
    }
    controller.reset();
    std::lock_guard<std::mutex> lock(context.mutex);
    CHECK(context.closed);
    CHECK(context.writesAfterClose == 0u);
    CHECK(context.events.back() == kCloseEvent);
}

void TestShutdownInterruptsRecoveryBackoff()
{
    FakeHidContext context;
    context.failAckOperation = DP104_OP_DATA;
    context.failAckCount = 1u;
    hid_device_ device = {&context};
    std::unique_ptr<RGBController_DP104> controller(
        new RGBController_DP104(std::unique_ptr<dp104::openrgb::Dp104HidTransport>(
            new dp104::openrgb::Dp104HidTransport(&device))));

    FillFrame(*controller, 5u);
    controller->DeviceUpdateLEDs();
    CHECK(WaitForRuntimeWord(g_dp104RuntimeStatus.recoveryState,
                             dp104::openrgb::DP104_RUNTIME_RECOVERY_WAITING,
                             1200u));
    CHECK(WaitForOperation(context, DP104_OP_RELEASE, 1u, 100u));
    std::this_thread::sleep_for(std::chrono::milliseconds(35));

    const std::chrono::steady_clock::time_point destroyStarted =
        std::chrono::steady_clock::now();
    controller.reset();
    const std::chrono::milliseconds destroyElapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - destroyStarted);
    CHECK(destroyElapsed.count() < 200);

    std::lock_guard<std::mutex> lock(context.mutex);
    CHECK(context.closed);
    CHECK(context.released == 0u);
    CHECK(CountOperation(context, DP104_OP_RELEASE) == 1u);
    CHECK(CountOperation(context, DP104_OP_CAPS) == 1u);
    CHECK(CountOperation(context, DP104_OP_BEGIN) == 1u);
    CHECK(context.writesAfterClose == 0u);
    CHECK(context.events.back() == kCloseEvent);
}

void TestLConnectCompatibilityTypeKeepsCapsGate()
{
    {
        FakeHidContext context;
        hid_device_ device = {&context};
        std::unique_ptr<RGBController_DP104> controller(
            new RGBController_DP104(
                std::unique_ptr<dp104::openrgb::Dp104HidTransport>(
                    new dp104::openrgb::Dp104HidTransport(&device)),
                "1.2.1",
                true));

        CHECK(controller->IsProtocolReady());
        /* DEVICE_TYPE_GPU is 2 in the pinned OpenRGB API. */
        CHECK(controller->type == 2);
        CHECK(controller->version == "1.2.1");
        CHECK(controller->description == "DP104 104 linear outputs (L-Connect compatibility)");
        CHECK(controller->zones.size() == 1u);
        CHECK(controller->zones[0].type == ZONE_TYPE_LINEAR);
        CHECK(controller->zones[0].leds_count == DP104_LED_COUNT);
        CHECK(controller->zones[0].name == "DP104 Physical Key LEDs (X-projected)");
        CHECK(controller->leds.size() == DP104_LED_COUNT);
        CHECK(controller->modes.size() == 1u);
        CHECK(controller->modes[0].name == "Direct");
        {
            std::lock_guard<std::mutex> lock(context.mutex);
            CHECK(context.events.size() == 1u);
            CHECK(context.events[0] == DP104_OP_CAPS);
            CHECK(context.begins == 0u);
            CHECK(context.published == 0u);
        }

        controller.reset();
        std::lock_guard<std::mutex> lock(context.mutex);
        CHECK(context.closed);
        CHECK(context.writesAfterClose == 0u);
        CHECK(context.events.size() == 2u);
        CHECK(context.events.back() == kCloseEvent);
    }

    {
        FakeHidContext context;
        context.failReadPending = true;
        hid_device_ device = {&context};
        std::unique_ptr<RGBController_DP104> controller(
            new RGBController_DP104(
                std::unique_ptr<dp104::openrgb::Dp104HidTransport>(
                    new dp104::openrgb::Dp104HidTransport(&device)),
                "1.2.1",
                true));

        CHECK(controller->type == 2);
        CHECK(!controller->IsProtocolReady());
        FillFrame(*controller);
        controller->DeviceUpdateLEDs();
        {
            std::lock_guard<std::mutex> lock(context.mutex);
            CHECK(context.events.size() == 1u);
            CHECK(context.events[0] == DP104_OP_CAPS);
            CHECK(context.begins == 0u);
            CHECK(context.published == 0u);
        }

        controller.reset();
        std::lock_guard<std::mutex> lock(context.mutex);
        CHECK(context.closed);
        CHECK(context.writesAfterClose == 0u);
        CHECK(context.events.size() == 2u);
        CHECK(context.events.back() == kCloseEvent);
    }
}

void TestPhysicalLayoutProjectionAnchorsAndKeyboardIdentity()
{
    using dp104::openrgb::Dp104FrameForControllerMode;
    using dp104::openrgb::Dp104LinearSourceIndexForX;
    using dp104::openrgb::kDp104PhysicalCoordinates;

    CHECK(kDp104PhysicalCoordinates[0].x == 0u);
    CHECK(kDp104PhysicalCoordinates[0].y == 0u);
    CHECK(kDp104PhysicalCoordinates[100].x == 66u);
    CHECK(kDp104PhysicalCoordinates[100].y == 64u);
    CHECK(kDp104PhysicalCoordinates[61].x == 139u);
    CHECK(kDp104PhysicalCoordinates[61].y == 40u);
    CHECK(kDp104PhysicalCoordinates[16].x == 224u);
    CHECK(Dp104LinearSourceIndexForX(0u) == 0u);
    CHECK(Dp104LinearSourceIndexForX(66u) == 30u);
    CHECK(Dp104LinearSourceIndexForX(139u) == 64u);
    CHECK(Dp104LinearSourceIndexForX(224u) == 103u);

    dp104::host::Frame linearFrame;
    for(unsigned index = 0; index < DP104_LED_COUNT; ++index)
    {
        linearFrame[index] = dp104::host::Color(
            static_cast<uint8_t>(index),
            static_cast<uint8_t>(255u - index),
            static_cast<uint8_t>(index * 7u));
    }

    const dp104::host::Frame keyboardFrame = Dp104FrameForControllerMode(linearFrame, false);
    const dp104::host::Frame projectedFrame = Dp104FrameForControllerMode(linearFrame, true);
    for(unsigned ledIndex = 0; ledIndex < DP104_LED_COUNT; ++ledIndex)
    {
        CHECK(keyboardFrame[ledIndex].r == linearFrame[ledIndex].r);
        CHECK(keyboardFrame[ledIndex].g == linearFrame[ledIndex].g);
        CHECK(keyboardFrame[ledIndex].b == linearFrame[ledIndex].b);

        const unsigned sourceIndex =
            Dp104LinearSourceIndexForX(kDp104PhysicalCoordinates[ledIndex].x);
        CHECK(projectedFrame[ledIndex].r == linearFrame[sourceIndex].r);
        CHECK(projectedFrame[ledIndex].g == linearFrame[sourceIndex].g);
        CHECK(projectedFrame[ledIndex].b == linearFrame[sourceIndex].b);
    }
}

void TestLConnectCompatibilitySendsProjectedFrame()
{
    FakeHidContext context;
    hid_device_ device = {&context};
    std::unique_ptr<RGBController_DP104> controller(
        new RGBController_DP104(
            std::unique_ptr<dp104::openrgb::Dp104HidTransport>(
                new dp104::openrgb::Dp104HidTransport(&device)),
            "1.2.1",
            true));

    CHECK(controller->IsProtocolReady());
    CHECK(controller->type == 2);
    CHECK(controller->zones.size() == 1u);
    CHECK(controller->zones[0].name == "DP104 Physical Key LEDs (X-projected)");
    FillFrame(*controller, 19u);
    controller->DeviceUpdateLEDs();
    CHECK(WaitForPublished(context, 1u, 1500u));
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        CHECK(context.published == 1u);
        CHECK(context.begins == 1u);
        CHECK(VisibleMatchesProjected(context, *controller));
    }

    controller.reset();
    std::lock_guard<std::mutex> lock(context.mutex);
    CHECK(context.closed);
    CHECK(context.released == 1u);
    CHECK(context.writesAfterClose == 0u);
    CHECK(context.events.back() == kCloseEvent);
}

void TestMailboxCoalescesUpdatesWhileHidIsBlocked()
{
    FakeHidContext context;
    hid_device_ device = {&context};
    std::unique_ptr<RGBController_DP104> controller(
        new RGBController_DP104(std::unique_ptr<dp104::openrgb::Dp104HidTransport>(
            new dp104::openrgb::Dp104HidTransport(&device))));

    {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.blockNextRead = true;
    }

    FillFrame(*controller, 1u);
    CHECK(UpdateReturnsQuickly(context, *controller));
    CHECK(WaitForReadBlocked(context, 500u));

    FillFrame(*controller, 2u);
    CHECK(UpdateReturnsQuickly(context, *controller));
    FillFrame(*controller, 3u);
    CHECK(UpdateReturnsQuickly(context, *controller));

    {
        std::lock_guard<std::mutex> lock(context.mutex);
        CHECK(context.readBlocked);
        CHECK(context.begins == 1u);
        CHECK(context.published == 0u);
        CHECK(context.overlappingExchanges == 0u);
    }

    ReleaseBlockedRead(context);
    CHECK(WaitForVisibleMatches(context, *controller, 1500u));
    CHECK(WaitForIoIdleAfterPublishes(context, 2u, 500u));
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        CHECK(context.begins == 2u);
        CHECK(context.published == 2u);
        CHECK(context.overlappingExchanges == 0u);
        CHECK(context.closesWithReadPending == 0u);
        CHECK(VisibleMatches(context, *controller));
    }

    controller.reset();
    std::lock_guard<std::mutex> lock(context.mutex);
    CHECK(context.closed);
    CHECK(context.released == 1u);
    CHECK(context.writesAfterClose == 0u);
    CHECK(context.closesWithReadPending == 0u);
    CHECK(context.events.size() >= 3u);
    if(context.events.size() >= 2u)
    {
        CHECK(context.events[context.events.size() - 2u] == DP104_OP_RELEASE);
        CHECK(context.events.back() == kCloseEvent);
    }
}

void TestShutdownJoinsWorkerBeforeClosingBlockedHid()
{
    FakeHidContext context;
    hid_device_ device = {&context};
    std::unique_ptr<RGBController_DP104> controller(
        new RGBController_DP104(std::unique_ptr<dp104::openrgb::Dp104HidTransport>(
            new dp104::openrgb::Dp104HidTransport(&device))));

    {
        std::lock_guard<std::mutex> lock(context.mutex);
        context.blockNextRead = true;
    }
    FillFrame(*controller, 4u);
    CHECK(UpdateReturnsQuickly(context, *controller));
    CHECK(WaitForReadBlocked(context, 500u));

    std::atomic<bool> destroyStarted(false);
    std::atomic<bool> destroyCompleted(false);
    std::thread destroyer([&controller, &destroyStarted, &destroyCompleted]()
    {
        destroyStarted.store(true);
        controller.reset();
        destroyCompleted.store(true);
    });

    while(!destroyStarted.load())
    {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(!destroyCompleted.load());
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        CHECK(context.readBlocked);
        CHECK(!context.closed);
        CHECK(context.closesWithReadPending == 0u);
    }

    ReleaseBlockedRead(context);
    destroyer.join();
    CHECK(destroyCompleted.load());
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        CHECK(context.closed);
        CHECK(context.released == 1u);
        CHECK(context.writesAfterClose == 0u);
        CHECK(context.closesWithReadPending == 0u);
        CHECK(context.events.size() >= 3u);
        if(context.events.size() >= 2u)
        {
            CHECK(context.events[context.events.size() - 2u] == DP104_OP_RELEASE);
            CHECK(context.events.back() == kCloseEvent);
        }
    }
}

} // namespace

extern "C"
{
int HID_API_CALL hid_write(hid_device* device, const unsigned char* data, size_t length)
{
    if(device == nullptr || device->context == nullptr || data == nullptr || length != DP104_REPORT_BYTES + 1u)
    {
        return -1;
    }

    FakeHidContext& context = *device->context;
    std::lock_guard<std::mutex> lock(context.mutex);
    if(context.closed)
    {
        ++context.writesAfterClose;
        return -1;
    }
    if(data[0] != 0 || data[1] != DP104_COMMAND)
    {
        return -1;
    }
    if(context.awaitingRead)
    {
        ++context.overlappingExchanges;
    }
    context.awaitingRead = true;

    const uint8_t* request = &data[1];
    context.events.push_back(request[1]);
    std::array<uint8_t, DP104_REPORT_BYTES> recordedRequest;
    std::memcpy(recordedRequest.data(), request, DP104_REPORT_BYTES);
    context.reports.push_back(recordedRequest);
    ++context.now;
    if(request[1] == DP104_OP_BEGIN)
    {
        ++context.begins;
    }
    dp104_receiver_handle(&context.receiver,
                         request,
                         DP104_REPORT_BYTES,
                         context.response.data(),
                         context.now);
    if(request[1] == context.failAckOperation && context.failAckCount > 0u)
    {
        --context.failAckCount;
        context.failReadPending = true;
    }
    context.cv.notify_all();
    return static_cast<int>(length);
}

int HID_API_CALL hid_read_timeout(hid_device* device,
                                 unsigned char* data,
                                 size_t length,
                                 int /*milliseconds*/)
{
    if(device == nullptr || device->context == nullptr || data == nullptr)
    {
        return -1;
    }

    FakeHidContext& context = *device->context;
    std::unique_lock<std::mutex> lock(context.mutex);
    const bool blockMatchingOperation =
        context.blockReadOperationEnabled && !context.blockReadOperationUsed &&
        context.response[1] == context.blockReadOperation;
    if(context.blockNextRead || blockMatchingOperation)
    {
        context.blockNextRead = false;
        if(blockMatchingOperation)
        {
            context.blockReadOperationUsed = true;
        }
        context.readBlocked = true;
        context.cv.notify_all();
        context.cv.wait(lock, [&context]()
        {
            return context.releaseBlockedRead || context.closed;
        });
        context.readBlocked = false;
    }
    if(context.failReadPending)
    {
        context.failReadPending = false;
        context.awaitingRead = false;
        context.cv.notify_all();
        return -1;
    }
    if(length != DP104_REPORT_BYTES)
    {
        context.awaitingRead = false;
        context.cv.notify_all();
        return -1;
    }
    std::memcpy(data, context.response.data(), DP104_REPORT_BYTES);
    context.awaitingRead = false;
    context.cv.notify_all();
    return DP104_REPORT_BYTES;
}

void HID_API_CALL hid_close(hid_device* device)
{
    if(device == nullptr || device->context == nullptr)
    {
        return;
    }

    FakeHidContext& context = *device->context;
    std::lock_guard<std::mutex> lock(context.mutex);
    if(context.awaitingRead)
    {
        ++context.closesWithReadPending;
    }
    context.closed = true;
    context.events.push_back(kCloseEvent);
    context.cv.notify_all();
}
}

int main()
{
    TestNoInitialFrameRefreshAndDestructorOrdering();
    TestTransientFrameFailureRechecksCapsAndSendsLatestFrame();
    TestRecoveryFailuresStopAfterThreeAttemptsAcrossCapsAndFrames();
    TestRecoveryHardWindowExpiresAfterTenSeconds();
    TestShutdownInterruptsRecoveryBackoff();
    TestLConnectCompatibilityTypeKeepsCapsGate();
    TestPhysicalLayoutProjectionAnchorsAndKeyboardIdentity();
    TestLConnectCompatibilitySendsProjectedFrame();
    TestMailboxCoalescesUpdatesWhileHidIsBlocked();
    TestShutdownJoinsWorkerBeforeClosingBlockedHid();
    if(g_failures != 0)
    {
        std::fprintf(stderr, "%d controller test(s) failed.\n", g_failures);
        return EXIT_FAILURE;
    }

    std::puts("OpenRGB controller lifecycle tests passed.");
    return EXIT_SUCCESS;
}
