#include "Dp104HidTransport.h"
#include "Dp104RuntimeStatus.h"
#include "dp104_protocol.h"

#if DP104_CAPTURE_FAILURE_DIAGNOSTICS
#include "Dp104FailureCapture.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <thread>
#include <vector>

struct hid_device_
{
    int tag;
};

namespace
{
using dp104::host::Report;
using dp104::openrgb::Dp104HidTransport;

#if DP104_CAPTURE_FAILURE_DIAGNOSTICS
using dp104::openrgb::DP104_FAILURE_CAPTURE_F5_BYTES;
using dp104::openrgb::DP104_FAILURE_CAPTURE_F5_PAGES;
#endif

int g_failures = 0;

void Check(bool condition, const char* expression, int line)
{
    if(!condition)
    {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++g_failures;
    }
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

enum class StubMode
{
    Normal,
    BadApplicationVersion,
    BadPlcSchema,
    FailMasterCapture,
    FailFreeze,
    FailReceiver,
    FailPage,
    EarlyZeroNoAck
};

struct HidStub
{
    StubMode mode = StubMode::Normal;
    std::deque<Report> queuedReports;
    std::vector<Report> writes;
    std::vector<int> readTimeouts;
    std::array<uint8_t, 2112u> f5Blob = {};
    bool waitOnEmptyRead = false;
    bool returnZeroOnNextRead = false;
    bool injectOriginalForeign = false;
    bool injectLateAckBeforeMasterCapture = false;
    bool failurePhase = false;
    uint8_t failedOperation = 0u;
    uint8_t failedTransaction = 0u;
    uint16_t failingPageOffset = 0u;
    uint32_t queriesOnCurrentPage = 0u;
    std::chrono::steady_clock::time_point originalFailureWriteAt = {};
    std::chrono::steady_clock::time_point failureMasterWriteAt = {};
    uint32_t readsAtFailureMasterWrite = 0u;
    int closeCalls = 0;
};

HidStub g_stub;

void ResetStub(StubMode mode = StubMode::Normal)
{
    g_stub = HidStub{};
    g_stub.mode = mode;
    for(size_t i = 0; i < g_stub.f5Blob.size(); ++i)
        g_stub.f5Blob[i] = static_cast<uint8_t>(i * 13u + 7u);
    g_stub.f5Blob[0] = 'P';
    g_stub.f5Blob[1] = '5';
    g_stub.f5Blob[2] = 'L';
    g_stub.f5Blob[3] = 'G';
}

void ResetRuntimeStatus()
{
    g_dp104RuntimeStatus.exchangeCount.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.exchangeSuccesses.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.exchangeFailures.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.firstFailureState.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.firstFailureOp.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.firstFailureTxn.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.firstFailureHeader0.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.firstFailureHeader1.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.firstFailureHeader2.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.firstFailureHeader3.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.firstFailureReason.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.firstFailureElapsedMs.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.firstFailureBytesRead.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.lastFailureSequence.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.lastFailureOp.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.lastFailureTxn.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.lastFailureHeader0.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.lastFailureHeader1.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.lastFailureHeader2.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.lastFailureHeader3.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.lastFailureReason.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.lastFailureElapsedMs.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.lastFailureBytesRead.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.framesQueued.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.framesStarted.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.framesCommitted.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.framesFailed.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.recoveryAttempts.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.recoverySucceeded.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.recoveryFailures.store(0u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.recoveryState.store(
        dp104::openrgb::DP104_RUNTIME_RECOVERY_READY, std::memory_order_relaxed);
    g_dp104RuntimeStatus.protocolReady.store(1u, std::memory_order_relaxed);
    g_dp104RuntimeStatus.reserved.store(0u, std::memory_order_relaxed);
}

#if DP104_CAPTURE_FAILURE_DIAGNOSTICS
void ResetCapture()
{
    using dp104::openrgb::DP104_CAPTURE_BUSY;
    using dp104::openrgb::DP104_CAPTURE_IDLE;
    using dp104::openrgb::DP104_FAILURE_CAPTURE_MAGIC;
    using dp104::openrgb::DP104_FAILURE_CAPTURE_SCHEMA;
    using dp104::openrgb::Dp104FailureCapture;
    Dp104FailureCapture& capture = g_dp104FailureCapture;
    capture.state.store(DP104_CAPTURE_BUSY, std::memory_order_release);
    const size_t clearOffset = offsetof(Dp104FailureCapture, originalFailureReason);
    std::memset(reinterpret_cast<uint8_t*>(&capture) + clearOffset, 0,
                sizeof(capture) - clearOffset);
    capture.magic = DP104_FAILURE_CAPTURE_MAGIC;
    capture.schemaVersion = DP104_FAILURE_CAPTURE_SCHEMA;
    capture.byteSize = static_cast<uint32_t>(sizeof(capture));
    capture.f5FirstFailedOffset = UINT32_MAX;
    capture.state.store(DP104_CAPTURE_IDLE, std::memory_order_release);
}

Report MakeF2Reply(const Report& request)
{
    Report response = {};
    response[0] = DP104_COMMAND;
    response[1] = 0xf2u;
    response[2] = request[2];
    response[3] = DP104_OK;
    std::memcpy(response.data() + 4u, "DP104RGB", 8u);
    response[12] = 1u;
    response[13] = g_stub.mode == StubMode::BadApplicationVersion ? 1u : 2u;
    response[14] = 1u;
    response[15] = 1u;
    std::memcpy(response.data() + 16u, request.data() + 4u, 16u);
    return response;
}

Report MakeF4Reply(const Report& request)
{
    Report response = {};
    response[0] = DP104_COMMAND;
    response[1] = 0xf4u;
    response[2] = request[2];
    response[3] = DP104_OK;
    std::memcpy(response.data() + 4u, request.data() + 4u, 4u);
    std::memcpy(response.data() + 8u, "DP104M4", 7u);
    dp104_write32(response.data() + 16u, 0x00010002u);
    dp104_write32(response.data() + 20u, 0x80300000u);
    return response;
}

Report MakeF5Reply(const Report& request)
{
    Report response = {};
    const uint16_t offset = dp104_read16(request.data() + 8u);
    response[0] = DP104_COMMAND;
    response[1] = 0xf5u;
    response[2] = request[2];
    response[3] = DP104_OK;
    std::memcpy(response.data() + 4u, request.data() + 4u, 4u);
    std::memcpy(response.data() + 8u, "P5RX", 4u);
    dp104_write16(response.data() + 12u, offset);
    dp104_write16(response.data() + 14u,
                  static_cast<uint16_t>(DP104_FAILURE_CAPTURE_F5_BYTES));
    if(offset == 0xffffu)
    {
        dp104_write32(response.data() + 16u, 1u);
        dp104_write32(response.data() + 20u, 32u);
        dp104_write32(response.data() + 24u, 64u);
        dp104_write32(response.data() + 28u,
                      static_cast<uint32_t>(DP104_FAILURE_CAPTURE_F5_BYTES));
        if(g_stub.mode == StubMode::BadPlcSchema)
            dp104_write32(response.data() + 28u, 2111u);
    }
    else if(offset != 0xfffeu && offset < DP104_FAILURE_CAPTURE_F5_BYTES)
    {
        std::memcpy(response.data() + 16u, g_stub.f5Blob.data() + offset, 16u);
    }
    return response;
}

Report MakeF3Reply(const Report& request)
{
    Report response = {};
    response[0] = DP104_COMMAND;
    response[1] = 0xf3u;
    response[2] = request[2];
    response[3] = DP104_OK;
    std::memcpy(response.data() + 4u, request.data() + 4u, 4u);
    response[8] = 3u;
    response[9] = 0x40u;
    response[10] = 0x3fu;
    dp104_write32(response.data() + 24u, 0x13572468u);
    dp104_write32(response.data() + 28u, 0x13572468u);
    return response;
}

void QueueFailureForeign(HidStub& stub)
{
    Report foreign = {};
    foreign[0] = DP104_COMMAND;
    foreign[1] = stub.failedOperation;
    foreign[2] = stub.failedTransaction;
    foreign[3] = DP104_OK;
    stub.queuedReports.push_back(foreign);
}

void HandleWrite(const Report& request)
{
    if(request[1] == DP104_OP_CAPS)
    {
        Report response = {};
        response[0] = DP104_COMMAND;
        response[1] = DP104_OP_CAPS;
        response[2] = request[2];
        response[3] = DP104_OK;
        std::memcpy(response.data() + 4u, "DP104RGB", 8u);
        response[12] = 1u;
        response[13] = 0u;
        dp104_write16(response.data() + 14u, DP104_LED_COUNT);
        response[16] = DP104_CHUNK_LEDS;
        response[17] = 1u;
        dp104_write16(response.data() + 18u, DP104_LEASE_MS);
        g_stub.queuedReports.push_back(response);
        return;
    }
    if(request[1] == DP104_OP_BEGIN || request[1] == DP104_OP_DATA ||
       request[1] == DP104_OP_COMMIT)
    {
        g_stub.failurePhase = true;
        g_stub.failedOperation = request[1];
        g_stub.failedTransaction = request[2];
        g_stub.originalFailureWriteAt = std::chrono::steady_clock::now();
        if(g_stub.mode == StubMode::EarlyZeroNoAck)
        {
            g_stub.returnZeroOnNextRead = true;
            g_stub.waitOnEmptyRead = true;
        }
        if(g_stub.injectOriginalForeign)
        {
            Report foreign = {};
            foreign[0] = DP104_COMMAND;
            foreign[1] = DP104_OP_STATUS;
            foreign[2] = 0x54u;
            g_stub.queuedReports.push_back(foreign);
        }
        return;
    }

    if(request[1] == 0xf2u)
    {
        g_stub.queuedReports.push_back(MakeF2Reply(request));
        return;
    }
    if(request[1] == 0xf4u)
    {
        if(g_stub.failurePhase)
        {
            g_stub.failureMasterWriteAt = std::chrono::steady_clock::now();
            g_stub.readsAtFailureMasterWrite = static_cast<uint32_t>(g_stub.readTimeouts.size());
            if(g_stub.mode == StubMode::FailMasterCapture) return;
        }
        if(g_stub.failurePhase && g_stub.injectLateAckBeforeMasterCapture)
            QueueFailureForeign(g_stub);
        g_stub.queuedReports.push_back(MakeF4Reply(request));
        return;
    }
    if(request[1] == 0xf5u)
    {
        const uint16_t offset = dp104_read16(request.data() + 8u);
        if(g_stub.failurePhase && offset == 0u && g_stub.mode == StubMode::FailFreeze)
            return;
        if(g_stub.failurePhase && offset != 0u && offset != 0xffffu &&
           offset != 0xfffeu && g_stub.mode == StubMode::FailPage &&
           offset == g_stub.failingPageOffset)
        {
            ++g_stub.queriesOnCurrentPage;
            return;
        }
        g_stub.queuedReports.push_back(MakeF5Reply(request));
        return;
    }
    if(request[1] == 0xf3u)
    {
        if(g_stub.failurePhase && g_stub.mode == StubMode::FailReceiver)
            return;
        g_stub.queuedReports.push_back(MakeF3Reply(request));
        return;
    }
    if(request[1] == DP104_OP_RELEASE)
    {
        Report response = {};
        response[0] = DP104_COMMAND;
        response[1] = request[1];
        response[2] = request[2];
        response[3] = DP104_OK;
        g_stub.queuedReports.push_back(response);
    }
}

unsigned CountWrites(uint8_t operation, uint16_t offset = 0u,
                     bool checkOffset = false)
{
    return static_cast<unsigned>(std::count_if(g_stub.writes.begin(), g_stub.writes.end(),
        [=](const Report& report)
        {
            return report[1] == operation &&
                (!checkOffset || dp104_read16(report.data() + 8u) == offset);
        }));
}

void CheckF5NonceConsistency()
{
    const auto& capture = g_dp104FailureCapture;
    for(const Report& request : g_stub.writes)
    {
        if(request[1] == 0xf3u || request[1] == 0xf4u || request[1] == 0xf5u)
            CHECK(std::memcmp(request.data() + 4u, capture.nonce, 4u) == 0);
    }
}
#else
void HandleWrite(const Report& request)
{
    if(request[1] != DP104_OP_CAPS) return;
    Report response = {};
    response[0] = DP104_COMMAND;
    response[1] = DP104_OP_CAPS;
    response[2] = request[2];
    response[3] = DP104_OK;
    std::memcpy(response.data() + 4u, "DP104RGB", 8u);
    response[12] = 1u;
    response[13] = 0u;
    dp104_write16(response.data() + 14u, DP104_LED_COUNT);
    response[16] = DP104_CHUNK_LEDS;
    response[17] = 1u;
    dp104_write16(response.data() + 18u, DP104_LEASE_MS);
    g_stub.queuedReports.push_back(response);
}
#endif

void SetupCaps(Dp104HidTransport& transport)
{
    Report request = {};
    Report response = {};
    request[0] = DP104_COMMAND;
    request[1] = DP104_OP_CAPS;
    request[2] = 0x31u;
    CHECK(transport.Exchange(request, response));
    CHECK(response[1] == DP104_OP_CAPS);
}

} // namespace

extern "C"
{
int HID_API_CALL hid_write(hid_device* device, const unsigned char* data, size_t length)
{
    if(device == nullptr || data == nullptr || length != DP104_REPORT_BYTES + 1u)
        return -1;
    Report request = {};
    std::memcpy(request.data(), data + 1u, DP104_REPORT_BYTES);
    g_stub.writes.push_back(request);
    HandleWrite(request);
    return static_cast<int>(length);
}

int HID_API_CALL hid_read_timeout(hid_device* device, unsigned char* data,
                                  size_t length, int milliseconds)
{
    if(device == nullptr || data == nullptr || length != DP104_REPORT_BYTES)
        return -1;
    bool returnEarlyZero = false;
    g_stub.readTimeouts.push_back(milliseconds);
    if(g_stub.returnZeroOnNextRead)
    {
        g_stub.returnZeroOnNextRead = false;
        returnEarlyZero = true;
    }
    else if(!g_stub.queuedReports.empty())
    {
        const Report report = g_stub.queuedReports.front();
        g_stub.queuedReports.pop_front();
        std::memcpy(data, report.data(), DP104_REPORT_BYTES);
        return DP104_REPORT_BYTES;
    }
    if(returnEarlyZero) return 0;
    if(g_stub.waitOnEmptyRead && milliseconds > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    return 0;
}

void HID_API_CALL hid_close(hid_device* device)
{
    if(device != nullptr) ++g_stub.closeCalls;
}
}

int main()
{
#if DP104_CAPTURE_FAILURE_DIAGNOSTICS
    using dp104::openrgb::DP104_CAPTURE_ARMED;
    using dp104::openrgb::DP104_CAPTURE_COMPLETE;
    using dp104::openrgb::DP104_CAPTURE_DISABLED;
    using dp104::openrgb::DP104_CAPTURE_PARTIAL;
    using dp104::openrgb::DP104_CAPTURE_QUERY_BAD_PAYLOAD;
    using dp104::openrgb::DP104_CAPTURE_QUERY_OK;
    using dp104::openrgb::DP104_CAPTURE_QUERY_TIMEOUT;
    using dp104::openrgb::DP104_CAPTURE_COUNTER_EXCHANGES;
    using dp104::openrgb::DP104_CAPTURE_COUNTER_FAILURES;
    using dp104::openrgb::DP104_CAPTURE_QUERY_APP_INFO;
    using dp104::openrgb::DP104_CAPTURE_QUERY_F5_PAGES;
    using dp104::openrgb::DP104_CAPTURE_QUERY_MASTER_FAILURE;
    using dp104::openrgb::DP104_CAPTURE_QUERY_PLC_FREEZE;
    using dp104::openrgb::DP104_CAPTURE_QUERY_PLC_PREFLIGHT;
    using dp104::openrgb::DP104_CAPTURE_QUERY_RECEIVER;

    {
        ResetRuntimeStatus();
        ResetCapture();
        ResetStub();
        hid_device_ device = {1};
        Dp104HidTransport transport(&device);
        SetupCaps(transport);
        CHECK(g_dp104FailureCapture.state.load(std::memory_order_acquire) ==
              DP104_CAPTURE_ARMED);
        CHECK(g_stub.writes.size() == 5u);
        CHECK(g_dp104FailureCapture.queryStatus[DP104_CAPTURE_QUERY_APP_INFO] ==
              DP104_CAPTURE_QUERY_OK);
        CHECK(g_dp104FailureCapture.queryStatus[DP104_CAPTURE_QUERY_PLC_PREFLIGHT] ==
              DP104_CAPTURE_QUERY_OK);
        CheckF5NonceConsistency();
        transport.Close();
    }

    {
        ResetRuntimeStatus();
        ResetCapture();
        ResetStub(StubMode::BadApplicationVersion);
        hid_device_ device = {2};
        Dp104HidTransport transport(&device);
        SetupCaps(transport);
        CHECK(g_dp104FailureCapture.state.load(std::memory_order_acquire) ==
              DP104_CAPTURE_DISABLED);
        CHECK(g_dp104FailureCapture.queryStatus[DP104_CAPTURE_QUERY_APP_INFO] ==
              DP104_CAPTURE_QUERY_BAD_PAYLOAD);
        CHECK(g_stub.writes.size() == 2u);
        Report failedRequest = {};
        Report ignored = {};
        failedRequest[0] = DP104_COMMAND;
        failedRequest[1] = DP104_OP_DATA;
        failedRequest[2] = 0x42u;
        CHECK(!transport.Exchange(failedRequest, ignored));
        CHECK(g_stub.writes.size() == 3u);
        CHECK(g_dp104FailureCapture.state.load(std::memory_order_acquire) ==
              DP104_CAPTURE_DISABLED);
        transport.Close();
    }

    {
        ResetRuntimeStatus();
        ResetCapture();
        ResetStub(StubMode::BadPlcSchema);
        hid_device_ device = {3};
        Dp104HidTransport transport(&device);
        SetupCaps(transport);
        CHECK(g_dp104FailureCapture.state.load(std::memory_order_acquire) ==
              DP104_CAPTURE_DISABLED);
        CHECK(g_dp104FailureCapture.queryStatus[DP104_CAPTURE_QUERY_PLC_PREFLIGHT] ==
              DP104_CAPTURE_QUERY_BAD_PAYLOAD);
        CHECK(g_stub.writes.size() == 4u);
        CHECK(CountWrites(0xf5u, 0xfffeu, true) == 0u);
        transport.Close();
    }

    {
        ResetRuntimeStatus();
        ResetCapture();
        ResetStub();
        g_stub.injectOriginalForeign = true;
        g_stub.injectLateAckBeforeMasterCapture = true;
        g_stub.waitOnEmptyRead = true;
        hid_device_ device = {4};
        Dp104HidTransport transport(&device);
        SetupCaps(transport);
        Report failedRequest = {};
        Report response = {};
        failedRequest[0] = DP104_COMMAND;
        failedRequest[1] = DP104_OP_DATA;
        failedRequest[2] = 0x77u;
        failedRequest[4] = 0x10u;
        CHECK(!transport.Exchange(failedRequest, response));
        auto& capture = g_dp104FailureCapture;
        CHECK(capture.state.load(std::memory_order_acquire) == DP104_CAPTURE_COMPLETE);
        CHECK(capture.originalFailureOperation == DP104_OP_DATA);
        CHECK(capture.originalFailureTransaction == 0x77u);
        CHECK(std::memcmp(capture.originalFailureRequest, failedRequest.data(),
                          DP104_REPORT_BYTES) == 0);
        CHECK(capture.originalFailureReason ==
              dp104::openrgb::DP104_RUNTIME_FAILURE_MISMATCH_TIMEOUT);
        CHECK(capture.originalFailureBytesRead == 0u);
        CHECK(capture.originalFailureElapsedMs ==
              g_dp104RuntimeStatus.lastFailureElapsedMs.load(
                  std::memory_order_relaxed));
        CHECK(capture.originalForeignCount == 1u);
        CHECK(capture.originalForeignFirstSeen == 1u);
        CHECK(capture.originalForeignFirstReport[1] == DP104_OP_STATUS);
        CHECK(capture.originalForeignLastReport[1] == DP104_OP_STATUS);
        CHECK(capture.foreignReportsDrained == 1u);
        CHECK(capture.foreignFirstReport[1] == DP104_OP_DATA);
        CHECK(capture.foreignLastReport[2] == 0x77u);
        CHECK(capture.queryForeignReports[DP104_CAPTURE_QUERY_MASTER_FAILURE] == 1u);
        CHECK(capture.runtimeCounters[DP104_CAPTURE_COUNTER_EXCHANGES] == 2u);
        CHECK(capture.runtimeCounters[DP104_CAPTURE_COUNTER_FAILURES] == 1u);
        CHECK(capture.runtimeCounters[DP104_CAPTURE_COUNTER_EXCHANGES] ==
              g_dp104RuntimeStatus.exchangeCount.load(
                  std::memory_order_relaxed));
        CHECK(capture.runtimeCounters[DP104_CAPTURE_COUNTER_FAILURES] ==
              g_dp104RuntimeStatus.exchangeFailures.load(
                  std::memory_order_relaxed));
        CHECK(capture.f5PagesAttempted == DP104_FAILURE_CAPTURE_F5_PAGES);
        CHECK(capture.f5PagesSucceeded == DP104_FAILURE_CAPTURE_F5_PAGES);
        CHECK(capture.f5PagesFailed == 0u);
        CHECK(capture.queryStatus[DP104_CAPTURE_QUERY_MASTER_FAILURE] ==
              DP104_CAPTURE_QUERY_OK);
        CHECK(capture.queryStatus[DP104_CAPTURE_QUERY_PLC_FREEZE] ==
              DP104_CAPTURE_QUERY_OK);
        CHECK(capture.queryStatus[DP104_CAPTURE_QUERY_RECEIVER] ==
              DP104_CAPTURE_QUERY_OK);
        CHECK(capture.queryStatus[DP104_CAPTURE_QUERY_F5_PAGES] ==
              DP104_CAPTURE_QUERY_OK);
        CHECK(std::memcmp(capture.f5Blob, g_stub.f5Blob.data(),
                          DP104_FAILURE_CAPTURE_F5_BYTES) == 0);
        CHECK((capture.f5PageSuccessBitmap[16] & 0x0fu) == 0x0fu);
        CHECK(capture.diagnosticQueriesAttempted == 138u);
        CHECK(capture.diagnosticQueriesMatched == 138u);
        CHECK(std::memcmp(capture.plcArmResponse + 8u, "P5RX", 4u) == 0);
        CHECK(dp104_read16(capture.plcArmResponse + 12u) == 0xfffeu);
        CHECK(CountWrites(0xf5u, 0x0000u, true) == 1u);
        CHECK(CountWrites(0xf5u, 0x0010u, true) == 1u);
        CHECK(CountWrites(0xf5u, 0x0830u, true) == 1u);
        CheckF5NonceConsistency();

        size_t failureWrite = 0u;
        size_t masterCaptureWrite = 0u;
        size_t freezeWrite = 0u;
        size_t receiverWrite = 0u;
        for(size_t i = 0; i < g_stub.writes.size(); ++i)
        {
            const Report& write = g_stub.writes[i];
            if(write[1] == DP104_OP_DATA && write[2] == 0x77u) failureWrite = i;
            if(write[1] == 0xf4u && i > failureWrite) masterCaptureWrite = i;
            if(write[1] == 0xf5u && dp104_read16(write.data() + 8u) == 0u &&
               i > failureWrite) freezeWrite = i;
            if(write[1] == 0xf3u && i > failureWrite) receiverWrite = i;
        }
        CHECK(failureWrite < masterCaptureWrite);
        CHECK(masterCaptureWrite < freezeWrite);
        CHECK(freezeWrite < receiverWrite);

        const size_t writesBeforeRelease = g_stub.writes.size();
        Report release = {};
        release[0] = DP104_COMMAND;
        release[1] = DP104_OP_RELEASE;
        release[2] = 0x78u;
        CHECK(transport.Exchange(release, response));
        CHECK(g_stub.writes.size() == writesBeforeRelease + 1u);
        CHECK(capture.state.load(std::memory_order_acquire) == DP104_CAPTURE_COMPLETE);
        transport.Close();
    }

    {
        ResetRuntimeStatus();
        ResetCapture();
        ResetStub(StubMode::EarlyZeroNoAck);
        hid_device_ device = {10};
        Dp104HidTransport transport(&device);
        SetupCaps(transport);
        const size_t readsBeforeFailure = g_stub.readTimeouts.size();
        Report failedRequest = {};
        Report response = {};
        failedRequest[0] = DP104_COMMAND;
        failedRequest[1] = DP104_OP_COMMIT;
        failedRequest[2] = 0x6au;

        CHECK(!transport.Exchange(failedRequest, response));
        const auto& capture = g_dp104FailureCapture;
        CHECK(capture.state.load(std::memory_order_acquire) == DP104_CAPTURE_COMPLETE);
        CHECK(capture.originalFailureReason ==
              dp104::openrgb::DP104_RUNTIME_FAILURE_TIMEOUT);
        CHECK(capture.originalFailureBytesRead == 0u);
        CHECK(capture.originalLateReportSeen == 0u);
        CHECK(capture.runtimeCounters[DP104_CAPTURE_COUNTER_FAILURES] == 1u);
        CHECK(CountWrites(DP104_OP_COMMIT) == 1u);
        const uint32_t originalReadCount =
            g_stub.readsAtFailureMasterWrite -
            static_cast<uint32_t>(readsBeforeFailure);
        CHECK(originalReadCount >= 2u && originalReadCount <= 3u);
        CHECK(capture.originalFailureElapsedMs >= 99u &&
              capture.originalFailureElapsedMs < 250u);
        const auto elapsedToF4 = std::chrono::duration_cast<std::chrono::milliseconds>(
            g_stub.failureMasterWriteAt - g_stub.originalFailureWriteAt);
        CHECK(elapsedToF4.count() >= 99);
        CHECK(elapsedToF4.count() < 250);
        transport.Close();
    }

    {
        ResetRuntimeStatus();
        ResetCapture();
        ResetStub(StubMode::FailPage);
        g_stub.failingPageOffset = 48u;
        hid_device_ device = {5};
        Dp104HidTransport transport(&device);
        SetupCaps(transport);
        g_stub.waitOnEmptyRead = true;
        Report failedRequest = {};
        Report response = {};
        failedRequest[0] = DP104_COMMAND;
        failedRequest[1] = DP104_OP_COMMIT;
        failedRequest[2] = 0x22u;
        CHECK(!transport.Exchange(failedRequest, response));
        const auto& capture = g_dp104FailureCapture;
        CHECK(capture.state.load(std::memory_order_acquire) == DP104_CAPTURE_PARTIAL);
        CHECK(capture.f5PagesAttempted == DP104_FAILURE_CAPTURE_F5_PAGES);
        CHECK(capture.f5PagesSucceeded == DP104_FAILURE_CAPTURE_F5_PAGES - 1u);
        CHECK(capture.f5PagesFailed == 1u);
        CHECK(capture.f5FirstFailedOffset == 48u);
        CHECK(capture.f5FirstFailedStatus == DP104_CAPTURE_QUERY_TIMEOUT);
        CHECK((capture.f5PageSuccessBitmap[0] & (1u << 3u)) == 0u);
        CHECK(capture.f5Blob[64] == g_stub.f5Blob[64]);
        CHECK(CountWrites(0xf5u, 48u, true) == 1u);
        CHECK(capture.runtimeCounters[DP104_CAPTURE_COUNTER_FAILURES] == 1u);
        transport.Close();
    }

    {
        ResetRuntimeStatus();
        ResetCapture();
        ResetStub(StubMode::FailMasterCapture);
        hid_device_ device = {6};
        Dp104HidTransport transport(&device);
        SetupCaps(transport);
        g_stub.waitOnEmptyRead = true;
        Report failedRequest = {};
        Report response = {};
        failedRequest[0] = DP104_COMMAND;
        failedRequest[1] = DP104_OP_DATA;
        failedRequest[2] = 0x23u;
        CHECK(!transport.Exchange(failedRequest, response));
        const auto& capture = g_dp104FailureCapture;
        CHECK(capture.state.load(std::memory_order_acquire) == DP104_CAPTURE_PARTIAL);
        CHECK(capture.queryStatus[DP104_CAPTURE_QUERY_MASTER_FAILURE] ==
              DP104_CAPTURE_QUERY_TIMEOUT);
        CHECK(capture.queryStatus[DP104_CAPTURE_QUERY_PLC_FREEZE] ==
              DP104_CAPTURE_QUERY_OK);
        CHECK(capture.queryStatus[DP104_CAPTURE_QUERY_RECEIVER] ==
              DP104_CAPTURE_QUERY_OK);
        CHECK(CountWrites(0xf4u) == 2u); /* preflight and the one failure snapshot */
        CHECK(CountWrites(0xf3u) == 1u);
        CHECK(capture.f5PagesSucceeded == DP104_FAILURE_CAPTURE_F5_PAGES);
        transport.Close();
    }

    {
        ResetRuntimeStatus();
        ResetCapture();
        ResetStub(StubMode::FailFreeze);
        hid_device_ device = {7};
        Dp104HidTransport transport(&device);
        SetupCaps(transport);
        g_stub.waitOnEmptyRead = true;
        Report failedRequest = {};
        Report response = {};
        failedRequest[0] = DP104_COMMAND;
        failedRequest[1] = DP104_OP_DATA;
        failedRequest[2] = 0x24u;
        CHECK(!transport.Exchange(failedRequest, response));
        const auto& capture = g_dp104FailureCapture;
        CHECK(capture.state.load(std::memory_order_acquire) == DP104_CAPTURE_PARTIAL);
        CHECK(capture.queryStatus[DP104_CAPTURE_QUERY_PLC_FREEZE] ==
              DP104_CAPTURE_QUERY_TIMEOUT);
        CHECK(capture.queryStatus[DP104_CAPTURE_QUERY_RECEIVER] ==
              DP104_CAPTURE_QUERY_OK);
        CHECK(CountWrites(0xf3u) == 1u);
        CHECK(CountWrites(0xf5u, 16u, true) == 0u);
        CHECK(capture.f5PagesAttempted == 1u);
        CHECK(capture.f5PagesFailed == 1u);
        transport.Close();
    }
#else
    ResetRuntimeStatus();
    ResetStub();
    hid_device_ device = {8};
    Dp104HidTransport transport(&device);
    SetupCaps(transport);
    const size_t readsBeforeRequest = g_stub.readTimeouts.size();
    g_stub.waitOnEmptyRead = true;
    Report request = {};
    Report response = {};
    request[0] = DP104_COMMAND;
    request[1] = DP104_OP_DATA;
    request[2] = 0x55u;
    CHECK(!transport.Exchange(request, response));
    CHECK(g_stub.writes.size() == 2u);
    CHECK(g_stub.readTimeouts.size() >= readsBeforeRequest + 1u &&
          g_stub.readTimeouts.size() <= readsBeforeRequest + 2u);
    CHECK(g_dp104RuntimeStatus.exchangeFailures.load(
              std::memory_order_relaxed) == 1u);
    transport.Close();
#endif

    if(g_failures != 0)
    {
        std::cerr << g_failures << " failure capture test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "DP104 failure capture tests passed.\n";
    return EXIT_SUCCESS;
}
