#include "dp104_host_client.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#ifdef DP104_TEST_OPENRGB_HID
#include "Dp104HidTransport.h"
#include "Dp104RuntimeStatus.h"
#include <hidapi.h>

struct hid_device_
{
    int tag;
};
#endif

namespace
{
using dp104::host::Client;
using dp104::host::Frame;
using dp104::host::ITransport;
using dp104::host::Report;

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

#ifdef DP104_TEST_OPENRGB_HID
struct HidStubState
{
    HidStubState()
        : writeResult(DP104_REPORT_BYTES + 1),
          readResult(DP104_REPORT_BYTES),
          readCalls(0),
          writeCalls(0),
          closeCalls(0),
          waitOnEmptyRead(false),
          returnZeroBeforeNextQueuedReport(false),
          nextQueuedReport(0)
    {
        readData.fill(0);
    }

    int writeResult;
    int readResult;
    int readCalls;
    int writeCalls;
    int closeCalls;
    bool waitOnEmptyRead;
    bool returnZeroBeforeNextQueuedReport;
    std::size_t nextQueuedReport;
    std::vector<unsigned char> lastWrite;
    std::array<unsigned char, DP104_REPORT_BYTES> readData;
    std::vector<Report> queuedReports;
    std::vector<int> readTimeouts;
    std::vector<Report> writes;
};

HidStubState g_hidStub;
std::mutex g_hidStubMutex;
std::mutex g_hidBarrierMutex;
std::condition_variable g_hidBarrierCv;
int g_hidBlockOperation = -1;
bool g_hidWriteBlocked = false;
bool g_hidReleaseBlockedWrite = false;

void ResetHidStub()
{
    {
        std::lock_guard<std::mutex> lock(g_hidStubMutex);
        g_hidStub = HidStubState();
    }
    {
        std::lock_guard<std::mutex> lock(g_hidBarrierMutex);
        g_hidBlockOperation = -1;
        g_hidWriteBlocked = false;
        g_hidReleaseBlockedWrite = false;
    }
}

void ResetRuntimeStatusCounters()
{
    dp104::openrgb::Dp104RuntimeStatus& status = g_dp104RuntimeStatus;
    status.exchangeCount.store(0u, std::memory_order_relaxed);
    status.exchangeSuccesses.store(0u, std::memory_order_relaxed);
    status.exchangeFailures.store(0u, std::memory_order_relaxed);
    status.firstFailureState.store(0u, std::memory_order_relaxed);
    status.firstFailureOp.store(0u, std::memory_order_relaxed);
    status.firstFailureTxn.store(0u, std::memory_order_relaxed);
    status.firstFailureHeader0.store(0u, std::memory_order_relaxed);
    status.firstFailureHeader1.store(0u, std::memory_order_relaxed);
    status.firstFailureHeader2.store(0u, std::memory_order_relaxed);
    status.firstFailureHeader3.store(0u, std::memory_order_relaxed);
    status.firstFailureReason.store(dp104::openrgb::DP104_RUNTIME_FAILURE_NONE,
                                    std::memory_order_relaxed);
    status.firstFailureElapsedMs.store(0u, std::memory_order_relaxed);
    status.firstFailureBytesRead.store(0u, std::memory_order_relaxed);
    status.lastFailureSequence.store(0u, std::memory_order_relaxed);
    status.lastFailureOp.store(0u, std::memory_order_relaxed);
    status.lastFailureTxn.store(0u, std::memory_order_relaxed);
    status.lastFailureHeader0.store(0u, std::memory_order_relaxed);
    status.lastFailureHeader1.store(0u, std::memory_order_relaxed);
    status.lastFailureHeader2.store(0u, std::memory_order_relaxed);
    status.lastFailureHeader3.store(0u, std::memory_order_relaxed);
    status.lastFailureReason.store(dp104::openrgb::DP104_RUNTIME_FAILURE_NONE,
                                   std::memory_order_relaxed);
    status.lastFailureElapsedMs.store(0u, std::memory_order_relaxed);
    status.lastFailureBytesRead.store(0u, std::memory_order_relaxed);
    status.framesQueued.store(0u, std::memory_order_relaxed);
    status.framesStarted.store(0u, std::memory_order_relaxed);
    status.framesCommitted.store(0u, std::memory_order_relaxed);
    status.framesFailed.store(0u, std::memory_order_relaxed);
    status.recoveryAttempts.store(0u, std::memory_order_relaxed);
    status.recoverySucceeded.store(0u, std::memory_order_relaxed);
    status.recoveryFailures.store(0u, std::memory_order_relaxed);
    status.recoveryState.store(dp104::openrgb::DP104_RUNTIME_RECOVERY_READY,
                               std::memory_order_relaxed);
    status.protocolReady.store(0u, std::memory_order_relaxed);
    status.reserved.store(0u, std::memory_order_relaxed);
}

bool WaitForBlockedHidWrite(unsigned timeoutMs)
{
    std::unique_lock<std::mutex> lock(g_hidBarrierMutex);
    return g_hidBarrierCv.wait_for(lock,
                                   std::chrono::milliseconds(timeoutMs),
                                   []() { return g_hidWriteBlocked; });
}

void ReleaseBlockedHidWrite()
{
    {
        std::lock_guard<std::mutex> lock(g_hidBarrierMutex);
        g_hidReleaseBlockedWrite = true;
    }
    g_hidBarrierCv.notify_all();
}
#endif

Report MakeCaps(uint8_t transaction)
{
    Report response = {};
    const char magic[] = DP104_CAPS_MAGIC;
    response[0] = DP104_COMMAND;
    response[1] = DP104_OP_CAPS;
    response[2] = transaction;
    response[3] = DP104_OK;
    for(unsigned index = 0; index < sizeof(magic) - 1u; ++index)
    {
        response[4u + index] = static_cast<uint8_t>(magic[index]);
    }
    response[12] = DP104_PROTOCOL_MAJOR;
    response[13] = 0;
    dp104_write16(&response[14], DP104_LED_COUNT);
    response[16] = DP104_CHUNK_LEDS;
    response[17] = 1;
    dp104_write16(&response[18], DP104_LEASE_MS);
    return response;
}

Report MakeAck(uint8_t operation, uint8_t transaction, uint8_t status = DP104_OK)
{
    Report response = {};
    response[0] = DP104_COMMAND;
    response[1] = operation;
    response[2] = transaction;
    response[3] = status;
    return response;
}

enum class Fault
{
    None,
    WrongCommand,
    WrongOperation,
    WrongTransaction,
    NonzeroStatus,
    NonzeroReserved,
    TransportFailure
};

class FakeTransport final : public ITransport
{
public:
    FakeTransport()
        : fault_(Fault::None),
          faultAt_(-1),
          caps_(MakeCaps(0)),
          echoCapsTransaction_(true),
          stockFixture_(false)
    {
    }

    bool Exchange(const Report& request, Report& response) override
    {
        const int call = static_cast<int>(requests.size());
        requests.push_back(request);

        if(stockFixture_ && request[1] == DP104_OP_CAPS)
        {
            response = stockResponse_;
        }
        else if(request[1] == DP104_OP_CAPS)
        {
            response = caps_;
            if(echoCapsTransaction_)
            {
                response[2] = request[2];
            }
        }
        else
        {
            response.fill(0);
            response[0] = DP104_COMMAND;
            response[1] = request[1];
            response[2] = request[2];
            response[3] = DP104_OK;
        }

        if(call == faultAt_)
        {
            switch(fault_)
            {
                case Fault::WrongCommand:
                    response[0] ^= 0xffu;
                    break;
                case Fault::WrongOperation:
                    response[1] = DP104_OP_COMMIT;
                    break;
                case Fault::WrongTransaction:
                    response[2] = response[2] == 0xffu ? 1u : static_cast<uint8_t>(response[2] + 1u);
                    break;
                case Fault::NonzeroStatus:
                    response[3] = DP104_BAD_CRC;
                    break;
                case Fault::NonzeroReserved:
                    response[4] = 1;
                    break;
                case Fault::TransportFailure:
                    return false;
                case Fault::None:
                    break;
            }
        }

        responses.push_back(response);
        return true;
    }

    void SetCaps(const Report& response, bool echoTransaction = false)
    {
        caps_ = response;
        echoCapsTransaction_ = echoTransaction;
    }

    void SetStockFixture()
    {
        stockFixture_ = true;
        stockResponse_.fill(0);
        stockResponse_[0] = 0xff;
        stockResponse_[1] = 0x00;
        stockResponse_[2] = 0x51;
        stockResponse_[3] = 0x00;
    }

    std::vector<Report> requests;
    std::vector<Report> responses;
    Fault fault_;
    int faultAt_;

private:
    Report caps_;
    Report stockResponse_;
    bool echoCapsTransaction_;
    bool stockFixture_;
};

bool ZeroBytes(const Report& report, unsigned first, unsigned last)
{
    for(unsigned index = first; index < last; ++index)
    {
        if(report[index] != 0)
        {
            return false;
        }
    }
    return true;
}

Frame MakeFrame()
{
    Frame frame;
    for(unsigned index = 0; index < DP104_LED_COUNT; ++index)
    {
        frame[index] = dp104::host::Color(static_cast<uint8_t>(index),
                                          static_cast<uint8_t>(255u - index),
                                          static_cast<uint8_t>(index ^ 0x5au));
    }
    return frame;
}

void TestExactStockResponseIsRejected()
{
    FakeTransport transport;
    transport.SetStockFixture();
    Client client(transport, 0x51);

    CHECK(!client.Connect());
    CHECK(!client.IsReady());
    CHECK(transport.requests.size() == 1u);
    CHECK(transport.responses.size() == 1u);
    CHECK(transport.requests[0][0] == 0xd2u);
    CHECK(transport.requests[0][1] == 0x00u);
    CHECK(transport.requests[0][2] == 0x51u);
    CHECK(transport.requests[0][3] == 0x00u);
    CHECK(ZeroBytes(transport.requests[0], 4, DP104_REPORT_BYTES));
    CHECK(transport.responses[0][0] == 0xffu);
    CHECK(transport.responses[0][1] == 0x00u);
    CHECK(transport.responses[0][2] == 0x51u);
    CHECK(transport.responses[0][3] == 0x00u);
    CHECK(ZeroBytes(transport.responses[0], 4, DP104_REPORT_BYTES));

    CHECK(!client.SetFrame(MakeFrame()));
    CHECK(transport.requests.size() == 1u);
    CHECK(client.Disconnect());
}

void TestCapabilityGateRejectsMalformedReplies()
{
    typedef void (*Mutator)(Report&);
    const Mutator mutations[] = {
        [](Report& report) { report[4] ^= 1u; },
        [](Report& report) { report[12] = 2; },
        [](Report& report) { report[13] = 1; },
        [](Report& report) { dp104_write16(&report[14], 103); },
        [](Report& report) { report[16] = 7; },
        [](Report& report) { report[17] = 0; },
        [](Report& report) { report[17] = 3; },
        [](Report& report) { dp104_write16(&report[18], 999); },
        [](Report& report) { report[20] = 1; },
        [](Report& report) { report[0] = 0xff; },
        [](Report& report) { report[1] = DP104_OP_BEGIN; },
        [](Report& report) { report[2] = 0; },
        [](Report& report) { report[3] = DP104_UNSUPPORTED; }
    };

    for(unsigned index = 0; index < sizeof(mutations) / sizeof(mutations[0]); ++index)
    {
        FakeTransport transport;
        Report malformed = MakeCaps(1);
        mutations[index](malformed);
        transport.SetCaps(malformed);
        Client client(transport);
        CHECK(!client.Connect());
        CHECK(!client.IsReady());
        CHECK(transport.requests.size() == 1u);
        CHECK(!client.SetFrame(MakeFrame()));
        CHECK(transport.requests.size() == 1u);
    }
}

void TestFramePacketsAndCrc()
{
    FakeTransport transport;
    Client client(transport);
    const Frame frame = MakeFrame();

    CHECK(client.Connect());
    CHECK(client.SetFrame(frame));
    CHECK(transport.requests.size() == 16u);
    CHECK(transport.requests[0][1] == DP104_OP_CAPS);

    const Report& begin = transport.requests[1];
    CHECK(begin[0] == DP104_COMMAND);
    CHECK(begin[1] == DP104_OP_BEGIN);
    CHECK(begin[2] != 0);
    CHECK(begin[3] == 0);
    CHECK(dp104_read16(&begin[4]) == DP104_LED_COUNT);
    CHECK(dp104_read16(&begin[6]) == 0);

    uint8_t packed[DP104_FRAME_BYTES];
    for(unsigned index = 0; index < DP104_LED_COUNT; ++index)
    {
        packed[index * 3u] = frame[index].r;
        packed[index * 3u + 1u] = frame[index].g;
        packed[index * 3u + 2u] = frame[index].b;
    }
    CHECK(dp104_read32(&begin[8]) == dp104_crc32(packed, DP104_FRAME_BYTES));

    for(unsigned chunk = 0; chunk < 13u; ++chunk)
    {
        const Report& data = transport.requests[2u + chunk];
        CHECK(data[0] == DP104_COMMAND);
        CHECK(data[1] == DP104_OP_DATA);
        CHECK(data[2] == begin[2]);
        CHECK(data[3] == 0);
        CHECK(dp104_read16(&data[4]) == chunk * DP104_CHUNK_LEDS);
        CHECK(data[6] == DP104_CHUNK_LEDS);
        for(unsigned offset = 0; offset < DP104_CHUNK_LEDS; ++offset)
        {
            const unsigned ledIndex = chunk * DP104_CHUNK_LEDS + offset;
            const unsigned reportIndex = 7u + offset * 3u;
            CHECK(data[reportIndex] == frame[ledIndex].r);
            CHECK(data[reportIndex + 1u] == frame[ledIndex].g);
            CHECK(data[reportIndex + 2u] == frame[ledIndex].b);
        }
        CHECK(data[31] == 0);
    }

    const Report& commit = transport.requests[15];
    CHECK(commit[0] == DP104_COMMAND);
    CHECK(commit[1] == DP104_OP_COMMIT);
    CHECK(commit[2] == begin[2]);
    CHECK(ZeroBytes(commit, 4, DP104_REPORT_BYTES));

    CHECK(client.Disconnect());
    CHECK(transport.requests.size() == 17u);
    CHECK(transport.requests[16][1] == DP104_OP_RELEASE);
    CHECK(transport.requests[16][2] != 0);
}

void TestInterleavedAndFailedResponsesAbortAndRelease()
{
    const Fault faults[] = {
        Fault::WrongCommand,
        Fault::WrongOperation,
        Fault::WrongTransaction,
        Fault::NonzeroStatus,
        Fault::NonzeroReserved,
        Fault::TransportFailure
    };
    for(unsigned index = 0; index < sizeof(faults) / sizeof(faults[0]); ++index)
    {
        FakeTransport transport;
        Client client(transport);
        CHECK(client.Connect());
        transport.fault_ = faults[index];
        transport.faultAt_ = 2; /* first DATA reply, after CAPS and BEGIN */

        CHECK(!client.SetFrame(MakeFrame()));
        CHECK(!client.IsReady());
        CHECK(transport.requests.size() == 4u);
        CHECK(transport.requests[1][1] == DP104_OP_BEGIN);
        CHECK(transport.requests[2][1] == DP104_OP_DATA);
        CHECK(transport.requests[3][1] == DP104_OP_RELEASE);

        const std::size_t requestCount = transport.requests.size();
        CHECK(!client.SetFrame(MakeFrame()));
        CHECK(transport.requests.size() == requestCount);
        CHECK(client.Disconnect());
    }
}

void TestReleaseReconnectAndTransactionWrap()
{
    FakeTransport transport;
    Client client(transport, 0xfe);
    const Frame frame = MakeFrame();

    CHECK(client.Connect());
    CHECK(transport.requests[0][2] == 0xfeu);
    CHECK(client.Release());
    CHECK(transport.requests[1][1] == DP104_OP_RELEASE);
    CHECK(transport.requests[1][2] == 0xffu);
    CHECK(client.Release());
    CHECK(transport.requests[2][1] == DP104_OP_RELEASE);
    CHECK(transport.requests[2][2] == 1u);
    CHECK(client.SetFrame(frame));
    CHECK(transport.requests[3][1] == DP104_OP_BEGIN);
    CHECK(transport.requests[3][2] == 2u);

    CHECK(client.Disconnect());
    CHECK(transport.requests.back()[1] == DP104_OP_RELEASE);
    const std::size_t beforeReconnect = transport.requests.size();
    CHECK(client.Connect());
    CHECK(transport.requests.size() == beforeReconnect + 1u);
    CHECK(transport.requests.back()[1] == DP104_OP_CAPS);
    CHECK(transport.requests.back()[2] != 0);
    CHECK(client.SetFrame(frame));
    CHECK(client.Disconnect());
}

#ifdef DP104_TEST_OPENRGB_HID
void TestNativeHidTransportPayloadLengths()
{
    hid_device_ device = {1};
    ResetHidStub();
    Report request;
    Report expectedResponse;
    for(unsigned index = 0; index < DP104_REPORT_BYTES; ++index)
    {
        request[index] = static_cast<uint8_t>(index + 1u);
        expectedResponse[index] = static_cast<uint8_t>(0xa0u + index);
    }
    request[0] = DP104_COMMAND;
    request[1] = DP104_OP_CAPS;
    request[2] = 0x44;
    expectedResponse[0] = DP104_COMMAND;
    expectedResponse[1] = DP104_OP_CAPS;
    expectedResponse[2] = 0x44;
    std::memcpy(g_hidStub.readData.data(), expectedResponse.data(), DP104_REPORT_BYTES);

    {
        dp104::openrgb::Dp104HidTransport transport(&device);
        Report response = {};
        CHECK(transport.Exchange(request, response));
        CHECK(g_hidStub.lastWrite.size() == DP104_REPORT_BYTES + 1u);
        CHECK(g_hidStub.lastWrite[0] == 0);
        CHECK(std::memcmp(&g_hidStub.lastWrite[1], request.data(), DP104_REPORT_BYTES) == 0);
        CHECK(g_hidStub.readCalls == 1);
        CHECK(response == expectedResponse);

        const std::size_t outputCount = g_hidStub.lastWrite.size();
        g_hidStub.readResult = DP104_REPORT_BYTES - 1;
        CHECK(!transport.Exchange(request, response));
        CHECK(g_hidStub.lastWrite.size() == outputCount);

        g_hidStub.readResult = -1;
        CHECK(!transport.Exchange(request, response));
        CHECK(g_hidStub.readCalls == 3);

        g_hidStub.readResult = DP104_REPORT_BYTES;
        const int readsBeforeShortWrite = g_hidStub.readCalls;
        g_hidStub.writeResult = DP104_REPORT_BYTES;
        CHECK(!transport.Exchange(request, response));
        CHECK(g_hidStub.readCalls == readsBeforeShortWrite);

        transport.Close();
        CHECK(!transport.IsOpen());
        CHECK(!transport.Exchange(request, response));
    }
    CHECK(g_hidStub.closeCalls == 1);
}

void TestNativeHidTransportSkipsForeignReports()
{
    hid_device_ device = {3};
    ResetHidStub();

    Report request = {};
    request[0] = DP104_COMMAND;
    request[1] = DP104_OP_DATA;
    request[2] = 0x63;

    Report nonD2 = request;
    nonD2[0] = 0xff;
    Report wrongOperation = request;
    wrongOperation[1] = DP104_OP_COMMIT;
    Report wrongTransaction = request;
    wrongTransaction[2] = 0x64;
    const Report matchingAck = MakeAck(DP104_OP_DATA, 0x63);

    g_hidStub.queuedReports.push_back(nonD2);
    g_hidStub.queuedReports.push_back(wrongOperation);
    g_hidStub.queuedReports.push_back(wrongTransaction);
    g_hidStub.queuedReports.push_back(matchingAck);

    {
        dp104::openrgb::Dp104HidTransport transport(&device);
        Report response = {};
        CHECK(transport.Exchange(request, response));
        CHECK(response == matchingAck);
        CHECK(g_hidStub.writeCalls == 1);
        CHECK(g_hidStub.readCalls == 4);
        CHECK(g_hidStub.lastWrite.size() == DP104_REPORT_BYTES + 1u);
        CHECK(std::memcmp(&g_hidStub.lastWrite[1], request.data(), DP104_REPORT_BYTES) == 0);
        CHECK(g_hidStub.readTimeouts.size() == 4u);
        for(std::size_t index = 0; index < g_hidStub.readTimeouts.size(); ++index)
        {
            CHECK(g_hidStub.readTimeouts[index] >= 1);
            CHECK(g_hidStub.readTimeouts[index] <= 100);
        }
    }
    CHECK(g_hidStub.closeCalls == 1);
}

void TestNativeHidTransportNoMatchTimesOutWithoutResend()
{
    hid_device_ device = {4};
    ResetHidStub();

    Report request = MakeAck(DP104_OP_RELEASE, 0x72);
    Report nonD2 = request;
    nonD2[0] = 0xff;
    Report wrongOperation = request;
    wrongOperation[1] = DP104_OP_CAPS;
    Report wrongTransaction = request;
    wrongTransaction[2] = 0x73;
    g_hidStub.queuedReports.push_back(nonD2);
    g_hidStub.queuedReports.push_back(wrongOperation);
    g_hidStub.queuedReports.push_back(wrongTransaction);
    g_hidStub.readResult = 0;
    g_hidStub.waitOnEmptyRead = true;

    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    {
        dp104::openrgb::Dp104HidTransport transport(&device);
        Report response = {};
        CHECK(!transport.Exchange(request, response));
        const std::chrono::milliseconds elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        CHECK(elapsed.count() >= 50);
        CHECK(elapsed.count() < 500);
        CHECK(g_hidStub.writeCalls == 1);
        /* Once zero-byte reads are retried until the real deadline, the
         * number of empty reads can vary with scheduler and millisecond
         * rounding. The three queued foreign reports must still be drained. */
        CHECK(g_hidStub.readCalls >= 4);
        CHECK(static_cast<std::size_t>(g_hidStub.readCalls) ==
              g_hidStub.readTimeouts.size());
        CHECK(g_hidStub.nextQueuedReport == 3u);
        for(const int timeout : g_hidStub.readTimeouts)
        {
            CHECK(timeout >= 1);
            CHECK(timeout <= 100);
        }
    }
    CHECK(g_hidStub.closeCalls == 1);
}

void TestZeroByteReadBeforeDeadlineKeepsWaitingForAck()
{
    hid_device_ device = {13};
    ResetHidStub();
    ResetRuntimeStatusCounters();

    const Report request = MakeAck(DP104_OP_DATA, 0x56u);
    const Report matchingAck = MakeAck(DP104_OP_DATA, 0x56u);
    g_hidStub.queuedReports.push_back(matchingAck);
    g_hidStub.returnZeroBeforeNextQueuedReport = true;

    {
        dp104::openrgb::Dp104HidTransport transport(&device);
        Report response = {};
        CHECK(transport.Exchange(request, response));
        CHECK(response == matchingAck);
        CHECK(g_hidStub.writeCalls == 1);
        CHECK(g_hidStub.readCalls == 2);
        CHECK(static_cast<std::size_t>(g_hidStub.readCalls) ==
              g_hidStub.readTimeouts.size());
        for(const int timeout : g_hidStub.readTimeouts)
        {
            CHECK(timeout >= 1);
            CHECK(timeout <= 100);
        }
        CHECK(g_hidStub.nextQueuedReport == 1u);
        CHECK(g_hidStub.readTimeouts.size() == 2u);
        CHECK(g_hidStub.readTimeouts[0] >= 1 && g_hidStub.readTimeouts[0] <= 100);
        CHECK(g_hidStub.readTimeouts[1] >= 1 && g_hidStub.readTimeouts[1] <= 100);
        CHECK(g_dp104RuntimeStatus.exchangeCount.load(std::memory_order_relaxed) == 1u);
        CHECK(g_dp104RuntimeStatus.exchangeSuccesses.load(std::memory_order_relaxed) == 1u);
        CHECK(g_dp104RuntimeStatus.exchangeFailures.load(std::memory_order_relaxed) == 0u);
        CHECK(g_dp104RuntimeStatus.firstFailureState.load(std::memory_order_relaxed) == 0u);
        transport.Close();
    }
    CHECK(g_hidStub.closeCalls == 1);
}

void TestRuntimeStatusExportLatchesFirstAndLastFailures()
{
    hid_device_ device = {12};
    ResetHidStub();
    ResetRuntimeStatusCounters();

    Report request = MakeAck(DP104_OP_DATA, 0x41u);
    g_hidStub.readResult = -1;
    {
        dp104::openrgb::Dp104HidTransport transport(&device);
        Report response = {};
        CHECK(!transport.Exchange(request, response));
        CHECK(g_dp104RuntimeStatus.magic.load(std::memory_order_relaxed) == 0x44503134u);
        CHECK(g_dp104RuntimeStatus.schemaVersion.load(std::memory_order_relaxed) == 1u);
        CHECK(g_dp104RuntimeStatus.byteSize.load(std::memory_order_relaxed) == 144u);
        CHECK(g_dp104RuntimeStatus.exchangeCount.load(std::memory_order_relaxed) == 1u);
        CHECK(g_dp104RuntimeStatus.exchangeSuccesses.load(std::memory_order_relaxed) == 0u);
        CHECK(g_dp104RuntimeStatus.exchangeFailures.load(std::memory_order_relaxed) == 1u);
        CHECK(g_dp104RuntimeStatus.firstFailureState.load(std::memory_order_acquire) == 2u);
        CHECK(g_dp104RuntimeStatus.firstFailureOp.load(std::memory_order_relaxed) == DP104_OP_DATA);
        CHECK(g_dp104RuntimeStatus.firstFailureTxn.load(std::memory_order_relaxed) == 0x41u);
        CHECK(g_dp104RuntimeStatus.firstFailureHeader0.load(std::memory_order_relaxed) == DP104_COMMAND);
        CHECK(g_dp104RuntimeStatus.firstFailureHeader1.load(std::memory_order_relaxed) == DP104_OP_DATA);
        CHECK(g_dp104RuntimeStatus.firstFailureHeader2.load(std::memory_order_relaxed) == 0x41u);
        CHECK(g_dp104RuntimeStatus.firstFailureReason.load(std::memory_order_relaxed) ==
              dp104::openrgb::DP104_RUNTIME_FAILURE_READ);
        CHECK(g_dp104RuntimeStatus.firstFailureBytesRead.load(std::memory_order_relaxed) ==
              static_cast<uint32_t>(-1));

        request = MakeAck(DP104_OP_COMMIT, 0x42u);
        g_hidStub.readResult = DP104_REPORT_BYTES - 1;
        CHECK(!transport.Exchange(request, response));
        CHECK(g_dp104RuntimeStatus.exchangeCount.load(std::memory_order_relaxed) == 2u);
        CHECK(g_dp104RuntimeStatus.exchangeFailures.load(std::memory_order_relaxed) == 2u);
        CHECK(g_dp104RuntimeStatus.firstFailureOp.load(std::memory_order_relaxed) == DP104_OP_DATA);
        CHECK(g_dp104RuntimeStatus.firstFailureTxn.load(std::memory_order_relaxed) == 0x41u);
        CHECK(g_dp104RuntimeStatus.lastFailureSequence.load(std::memory_order_acquire) == 4u);
        CHECK(g_dp104RuntimeStatus.lastFailureOp.load(std::memory_order_relaxed) == DP104_OP_COMMIT);
        CHECK(g_dp104RuntimeStatus.lastFailureTxn.load(std::memory_order_relaxed) == 0x42u);
        CHECK(g_dp104RuntimeStatus.lastFailureReason.load(std::memory_order_relaxed) ==
              dp104::openrgb::DP104_RUNTIME_FAILURE_SHORT_READ);
        CHECK(g_dp104RuntimeStatus.lastFailureBytesRead.load(std::memory_order_relaxed) ==
              static_cast<uint32_t>(DP104_REPORT_BYTES - 1u));
        CHECK(g_dp104RuntimeStatus.reserved.load(std::memory_order_relaxed) == 0u);
    }
    CHECK(g_hidStub.closeCalls == 1);
}

void TestMatchingHidErrorStatusFailsClient()
{
    hid_device_ device = {5};
    ResetHidStub();

    g_hidStub.queuedReports.push_back(MakeCaps(1));
    g_hidStub.queuedReports.push_back(MakeAck(DP104_OP_BEGIN, 2));
    g_hidStub.queuedReports.push_back(MakeAck(DP104_OP_DATA, 2, DP104_BAD_CRC));
    g_hidStub.queuedReports.push_back(MakeAck(DP104_OP_RELEASE, 3));

    {
        dp104::openrgb::Dp104HidTransport transport(&device);
        Client client(transport, 1);
        CHECK(client.Connect());
        CHECK(!client.SetFrame(MakeFrame()));
        CHECK(!client.IsReady());
        CHECK(client.Disconnect());
        CHECK(g_hidStub.writeCalls == 4);
        CHECK(g_hidStub.readCalls == 4);
        CHECK(g_hidStub.nextQueuedReport == 4u);
        CHECK(g_hidStub.lastWrite[1] == DP104_COMMAND);
        CHECK(g_hidStub.lastWrite[2] == DP104_OP_RELEASE);
        CHECK(g_hidStub.lastWrite[3] == 3);
    }
    CHECK(g_hidStub.closeCalls == 1);
}

void TestNativeHidTransportPassesCapsAndStockPayloads()
{
    hid_device_ device = {2};
    ResetHidStub();
    Report caps = MakeCaps(0x51);
    std::memcpy(g_hidStub.readData.data(), caps.data(), DP104_REPORT_BYTES);
    {
        dp104::openrgb::Dp104HidTransport transport(&device);
        Client client(transport, 0x51);
        CHECK(client.Connect());
        CHECK(g_hidStub.lastWrite.size() == DP104_REPORT_BYTES + 1u);
        CHECK(g_hidStub.lastWrite[0] == 0);
        CHECK(g_hidStub.lastWrite[1] == DP104_COMMAND);
        CHECK(g_hidStub.lastWrite[2] == DP104_OP_CAPS);
        CHECK(g_hidStub.lastWrite[3] == 0x51);
        CHECK(g_hidStub.readCalls == 1);
        CHECK(client.Disconnect());
    }
    CHECK(g_hidStub.closeCalls == 1);

    ResetHidStub();
    g_hidStub.readData.fill(0);
    g_hidStub.readData[0] = 0xff;
    g_hidStub.readData[1] = 0x00;
    g_hidStub.readData[2] = 0x51;
    g_hidStub.readData[3] = 0x00;
    Report stockResponse = {};
    stockResponse[0] = 0xff;
    stockResponse[1] = 0x00;
    stockResponse[2] = 0x51;
    g_hidStub.queuedReports.push_back(stockResponse);
    g_hidStub.readResult = 0;
    g_hidStub.waitOnEmptyRead = true;
    {
        dp104::openrgb::Dp104HidTransport transport(&device);
        Client client(transport, 0x51);
        CHECK(!client.Connect());
        CHECK(!client.IsReady());
        CHECK(g_hidStub.lastWrite.size() == DP104_REPORT_BYTES + 1u);
        CHECK(g_hidStub.writeCalls == 1);
        CHECK(g_hidStub.readCalls >= 2);
        CHECK(static_cast<std::size_t>(g_hidStub.readCalls) ==
              g_hidStub.readTimeouts.size());
        for(const int timeout : g_hidStub.readTimeouts)
        {
            CHECK(timeout >= 1);
            CHECK(timeout <= 100);
        }
    }
    CHECK(g_hidStub.closeCalls == 1);
}

void TestNativeHidTransportMatchingErrorStatusIsPreserved()
{
    hid_device_ device = {6};
    ResetHidStub();
    const Report request = MakeAck(DP104_OP_RELEASE, 0x45);
    const Report errorResponse = MakeAck(DP104_OP_RELEASE, 0x45, DP104_BAD_CRC);
    g_hidStub.queuedReports.push_back(errorResponse);

    {
        dp104::openrgb::Dp104HidTransport transport(&device);
        Report response = {};
        CHECK(transport.Exchange(request, response));
        CHECK(response == errorResponse);
        CHECK(response[3] == DP104_BAD_CRC);
        CHECK(g_hidStub.writeCalls == 1);
        CHECK(g_hidStub.readCalls == 1);
    }
    CHECK(g_hidStub.closeCalls == 1);
}

void TestNativeHidTransportSerializesSingleExchangeAcrossHandles()
{
    hid_device_ firstDevice = {7};
    hid_device_ secondDevice = {8};
    ResetHidStub();
    const Report firstRequest = MakeAck(DP104_OP_CAPS, 0x41);
    const Report secondRequest = MakeAck(DP104_OP_CAPS, 0x42);
    g_hidStub.queuedReports.push_back(firstRequest);
    g_hidStub.queuedReports.push_back(secondRequest);
    {
        std::lock_guard<std::mutex> lock(g_hidBarrierMutex);
        g_hidBlockOperation = DP104_OP_CAPS;
    }

    dp104::openrgb::Dp104HidTransport firstTransport(&firstDevice);
    dp104::openrgb::Dp104HidTransport secondTransport(&secondDevice);
    std::atomic<bool> firstSucceeded(false);
    std::atomic<bool> secondStarted(false);
    std::atomic<bool> secondSucceeded(false);
    Report firstResponse = {};
    Report secondResponse = {};

    std::thread firstThread([&]()
    {
        firstSucceeded = firstTransport.Exchange(firstRequest, firstResponse);
    });
    const bool firstWriteBlocked = WaitForBlockedHidWrite(1000u);

    std::thread secondThread([&]()
    {
        secondStarted = true;
        secondSucceeded = secondTransport.Exchange(secondRequest, secondResponse);
    });
    for(unsigned wait = 0; wait < 100u && !secondStarted.load(); ++wait)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    unsigned writesWhileFirstBlocked = 0;
    {
        std::lock_guard<std::mutex> lock(g_hidStubMutex);
        writesWhileFirstBlocked = static_cast<unsigned>(g_hidStub.writeCalls);
    }
    ReleaseBlockedHidWrite();
    firstThread.join();
    secondThread.join();

    CHECK(firstWriteBlocked);
    CHECK(secondStarted.load());
    CHECK(writesWhileFirstBlocked == 1u);
    CHECK(firstSucceeded.load());
    CHECK(secondSucceeded.load());
    CHECK(firstResponse == firstRequest);
    CHECK(secondResponse == secondRequest);
    {
        std::lock_guard<std::mutex> lock(g_hidStubMutex);
        CHECK(g_hidStub.writes.size() == 2u);
        CHECK(g_hidStub.writes[0] == firstRequest);
        CHECK(g_hidStub.writes[1] == secondRequest);
        CHECK(g_hidStub.readCalls == 2);
    }
    firstTransport.Close();
    secondTransport.Close();
}

void TestNativeHidTransportKeepsDiscoveryOutsideWholeFrame()
{
    hid_device_ frameDevice = {9};
    hid_device_ discoveryDevice = {10};
    ResetHidStub();

    const uint8_t frameTransaction = 2;
    g_hidStub.queuedReports.push_back(MakeCaps(1));
    g_hidStub.queuedReports.push_back(MakeAck(DP104_OP_BEGIN, frameTransaction));
    for(unsigned chunk = 0; chunk < 13u; ++chunk)
    {
        g_hidStub.queuedReports.push_back(MakeAck(DP104_OP_DATA, frameTransaction));
    }
    g_hidStub.queuedReports.push_back(MakeAck(DP104_OP_COMMIT, frameTransaction));
    g_hidStub.queuedReports.push_back(MakeAck(DP104_OP_RELEASE, 3));
    const Report discoveryRequest = MakeAck(DP104_OP_CAPS, 0x55);
    const Report discoveryAck = MakeAck(DP104_OP_CAPS, 0x55);
    g_hidStub.queuedReports.push_back(discoveryAck);
    {
        std::lock_guard<std::mutex> lock(g_hidBarrierMutex);
        g_hidBlockOperation = DP104_OP_BEGIN;
    }

    dp104::openrgb::Dp104HidTransport frameTransport(&frameDevice);
    dp104::openrgb::Dp104HidTransport discoveryTransport(&discoveryDevice);
    std::atomic<bool> connected(false);
    std::atomic<bool> frameSent(false);
    std::atomic<bool> released(false);
    std::atomic<bool> discoveryStarted(false);
    std::atomic<bool> discoverySent(false);
    Report discoveryResponse = {};

    std::thread frameThread([&]()
    {
        Client client(frameTransport, 1);
        connected = client.Connect();
        if(connected.load())
        {
            dp104::openrgb::Dp104HidTransport::ProtocolSession frameSession;
            frameSent = client.SetFrame(MakeFrame());
            released = client.Disconnect();
        }
    });

    const bool beginWriteBlocked = WaitForBlockedHidWrite(1000u);
    std::thread discoveryThread([&]()
    {
        discoveryStarted = true;
        dp104::openrgb::Dp104HidTransport::ProtocolSession discoverySession;
        discoverySent = discoveryTransport.Exchange(discoveryRequest, discoveryResponse);
    });
    for(unsigned wait = 0; wait < 100u && !discoveryStarted.load(); ++wait)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    unsigned writesWhileBeginBlocked = 0;
    {
        std::lock_guard<std::mutex> lock(g_hidStubMutex);
        writesWhileBeginBlocked = static_cast<unsigned>(g_hidStub.writeCalls);
    }
    ReleaseBlockedHidWrite();
    frameThread.join();
    discoveryThread.join();

    CHECK(beginWriteBlocked);
    CHECK(discoveryStarted.load());
    CHECK(writesWhileBeginBlocked == 2u); /* Connect CAPS and BEGIN only. */
    CHECK(connected.load());
    CHECK(frameSent.load());
    CHECK(released.load());
    CHECK(discoverySent.load());
    CHECK(discoveryResponse == discoveryAck);
    {
        std::lock_guard<std::mutex> lock(g_hidStubMutex);
        CHECK(g_hidStub.writes.size() == 18u);
        if(g_hidStub.writes.size() == 18u)
        {
            CHECK(g_hidStub.writes[0][1] == DP104_OP_CAPS);
            CHECK(g_hidStub.writes[0][2] == 1);
            CHECK(g_hidStub.writes[1][1] == DP104_OP_BEGIN);
            CHECK(g_hidStub.writes[1][2] == frameTransaction);
            for(unsigned index = 2; index < 15u; ++index)
            {
                CHECK(g_hidStub.writes[index][1] == DP104_OP_DATA);
                CHECK(g_hidStub.writes[index][2] == frameTransaction);
            }
            CHECK(g_hidStub.writes[15][1] == DP104_OP_COMMIT);
            CHECK(g_hidStub.writes[15][2] == frameTransaction);
            CHECK(g_hidStub.writes[16][1] == DP104_OP_RELEASE);
            CHECK(g_hidStub.writes[16][2] == 3);
            CHECK(g_hidStub.writes[17] == discoveryRequest);
            CHECK(g_hidStub.readCalls == 18);
        }
    }

    frameTransport.Close();
    discoveryTransport.Close();
}

void TestProtocolSessionUnlocksAfterFrameAndRelease()
{
    hid_device_ device = {11};
    ResetHidStub();
    g_hidStub.queuedReports.push_back(MakeCaps(1));
    g_hidStub.queuedReports.push_back(MakeAck(DP104_OP_BEGIN, 2));
    for(unsigned chunk = 0; chunk < 13u; ++chunk)
    {
        g_hidStub.queuedReports.push_back(MakeAck(DP104_OP_DATA, 2));
    }
    g_hidStub.queuedReports.push_back(MakeAck(DP104_OP_COMMIT, 2));
    g_hidStub.queuedReports.push_back(MakeAck(DP104_OP_RELEASE, 3));

    dp104::openrgb::Dp104HidTransport transport(&device);
    Client client(transport, 1);
    CHECK(client.Connect());
    std::atomic<bool> senderStarted(false);
    std::atomic<bool> frameSent(false);
    std::thread sender([&]()
    {
        senderStarted = true;
        dp104::openrgb::Dp104HidTransport::ProtocolSession session;
        frameSent = client.SetFrame(MakeFrame());
    });

    for(unsigned wait = 0; wait < 100u && !senderStarted.load(); ++wait)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    sender.join();
    CHECK(senderStarted.load());
    CHECK(frameSent.load());
    CHECK(client.Disconnect());

    std::atomic<bool> gateReleased(false);
    std::thread gateProbe([&]()
    {
        dp104::openrgb::Dp104HidTransport::ProtocolSession session;
        gateReleased = true;
    });
    gateProbe.join();
    CHECK(gateReleased.load());
    transport.Close();
}
#endif

} // namespace

#ifdef DP104_TEST_OPENRGB_HID
extern "C"
{
int HID_API_CALL hid_write(hid_device* device, const unsigned char* data, size_t length)
{
    if(device == nullptr || data == nullptr)
    {
        return -1;
    }

    int writeResult = -1;
    int operation = -1;
    {
        std::lock_guard<std::mutex> lock(g_hidStubMutex);
        ++g_hidStub.writeCalls;
        g_hidStub.lastWrite.assign(data, data + length);
        if(length == DP104_REPORT_BYTES + 1u)
        {
            Report request = {};
            std::memcpy(request.data(), data + 1, DP104_REPORT_BYTES);
            operation = request[1];
            g_hidStub.writes.push_back(request);
        }
        writeResult = g_hidStub.writeResult;
    }

    {
        std::unique_lock<std::mutex> lock(g_hidBarrierMutex);
        if(operation == g_hidBlockOperation)
        {
            g_hidBlockOperation = -1;
            g_hidWriteBlocked = true;
            g_hidBarrierCv.notify_all();
            g_hidBarrierCv.wait(lock, []() { return g_hidReleaseBlockedWrite; });
        }
    }

    return writeResult;
}

int HID_API_CALL hid_read_timeout(hid_device* device,
                                 unsigned char* data,
                                 size_t length,
                                 int milliseconds)
{
    if(device == nullptr || data == nullptr)
    {
        return -1;
    }
    int readResult = -1;
    bool hasQueuedReport = false;
    Report queued = {};
    std::array<unsigned char, DP104_REPORT_BYTES> readData = {{0}};
    bool waitOnEmptyRead = false;
    {
        std::lock_guard<std::mutex> lock(g_hidStubMutex);
        ++g_hidStub.readCalls;
        g_hidStub.readTimeouts.push_back(milliseconds);
        if(g_hidStub.nextQueuedReport < g_hidStub.queuedReports.size())
        {
            if(g_hidStub.returnZeroBeforeNextQueuedReport)
            {
                g_hidStub.returnZeroBeforeNextQueuedReport = false;
                readResult = 0;
            }
            else
            {
                queued = g_hidStub.queuedReports[g_hidStub.nextQueuedReport++];
                hasQueuedReport = true;
                readResult = DP104_REPORT_BYTES;
            }
        }
        else
        {
            readResult = g_hidStub.readResult;
            readData = g_hidStub.readData;
            waitOnEmptyRead = g_hidStub.waitOnEmptyRead;
        }
    }

    if(hasQueuedReport)
    {
        const size_t bytesToCopy = std::min(length, queued.size());
        std::memcpy(data, queued.data(), bytesToCopy);
    }
    else if(readResult > 0)
    {
        const size_t bytesToCopy = static_cast<size_t>(readResult) < length
            ? static_cast<size_t>(readResult)
            : length;
        std::memcpy(data, readData.data(), bytesToCopy);
    }
    else if(readResult == 0 && waitOnEmptyRead && milliseconds > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }
    return readResult;
}

void HID_API_CALL hid_close(hid_device* device)
{
    if(device != nullptr)
    {
    std::lock_guard<std::mutex> lock(g_hidStubMutex);
    ++g_hidStub.closeCalls;
    }
}
}
#endif

int main()
{
    TestExactStockResponseIsRejected();
    TestCapabilityGateRejectsMalformedReplies();
    TestFramePacketsAndCrc();
    TestInterleavedAndFailedResponsesAbortAndRelease();
    TestReleaseReconnectAndTransactionWrap();
#ifdef DP104_TEST_OPENRGB_HID
    TestNativeHidTransportPayloadLengths();
    TestNativeHidTransportSkipsForeignReports();
    TestNativeHidTransportNoMatchTimesOutWithoutResend();
    TestZeroByteReadBeforeDeadlineKeepsWaitingForAck();
    TestRuntimeStatusExportLatchesFirstAndLastFailures();
    TestMatchingHidErrorStatusFailsClient();
    TestNativeHidTransportPassesCapsAndStockPayloads();
    TestNativeHidTransportMatchingErrorStatusIsPreserved();
    TestNativeHidTransportSerializesSingleExchangeAcrossHandles();
    TestNativeHidTransportKeepsDiscoveryOutsideWholeFrame();
    TestProtocolSessionUnlocksAfterFrameAndRelease();
#endif

    if(g_failures != 0)
    {
        std::cerr << g_failures << " host test(s) failed.\n";
        return EXIT_FAILURE;
    }

    std::cout << "All DP104 host tests passed.\n";
    return EXIT_SUCCESS;
}
