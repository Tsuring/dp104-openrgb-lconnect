#include "Dp104HidTransport.h"
#include "Dp104RuntimeStatus.h"

#if defined(DP104_CAPTURE_FAILURE_DIAGNOSTICS) && DP104_CAPTURE_FAILURE_DIAGNOSTICS
#include "Dp104FailureCapture.h"
#endif

#include <array>
#include <chrono>
#include <cstring>

#if defined(DP104_CAPTURE_FAILURE_DIAGNOSTICS) && DP104_CAPTURE_FAILURE_DIAGNOSTICS
#include <random>
#endif

namespace dp104
{
namespace openrgb
{
namespace
{
const int kHidOutputReportBytes = DP104_REPORT_BYTES + 1;
const int kHidInputReportBytes = DP104_REPORT_BYTES;
const int kReadTimeoutMs = 100;

#if defined(DP104_CAPTURE_FAILURE_DIAGNOSTICS) && DP104_CAPTURE_FAILURE_DIAGNOSTICS
const uint8_t kAppInfoOperation = 0xf2u;
const uint8_t kReceiverSnapshotOperation = 0xf3u;
const uint8_t kMasterSnapshotOperation = 0xf4u;
const uint8_t kPlcDiagnosticOperation = 0xf5u;
const uint16_t kPlcDiagnosticPreflight = 0xffffu;
const uint16_t kPlcDiagnosticArm = 0xfffeu;
const uint32_t kForeignReportsPerQueryLimit = 8u;
const uint32_t kForeignReportsCaptureLimit = 32u;
#endif

uint32_t ExchangeElapsedMilliseconds(
    const std::chrono::steady_clock::time_point& exchangeStarted)
{
    const std::chrono::milliseconds elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - exchangeStarted);
    return static_cast<uint32_t>(elapsed.count());
}

void StoreFailureFields(const host::Report& request,
                        uint32_t reason,
                        int bytesRead,
                        uint32_t elapsedMs,
                        std::atomic<uint32_t>& operation,
                        std::atomic<uint32_t>& transaction,
                        std::atomic<uint32_t>& header0,
                        std::atomic<uint32_t>& header1,
                        std::atomic<uint32_t>& header2,
                        std::atomic<uint32_t>& header3,
                        std::atomic<uint32_t>& failureReason,
                        std::atomic<uint32_t>& failureElapsedMs,
                        std::atomic<uint32_t>& failureBytesRead)
{
    operation.store(request[1], std::memory_order_relaxed);
    transaction.store(request[2], std::memory_order_relaxed);
    header0.store(request[0], std::memory_order_relaxed);
    header1.store(request[1], std::memory_order_relaxed);
    header2.store(request[2], std::memory_order_relaxed);
    header3.store(request[3], std::memory_order_relaxed);
    failureReason.store(reason, std::memory_order_relaxed);
    failureElapsedMs.store(elapsedMs, std::memory_order_relaxed);
    failureBytesRead.store(static_cast<uint32_t>(bytesRead), std::memory_order_relaxed);
}

void RecordFailure(const host::Report& request,
                   uint32_t reason,
                   int bytesRead,
                   const std::chrono::steady_clock::time_point& exchangeStarted)
{
    const uint32_t elapsedMs = ExchangeElapsedMilliseconds(exchangeStarted);
    g_dp104RuntimeStatus.exchangeFailures.fetch_add(1u, std::memory_order_relaxed);

    uint32_t expectedState = 0u;
    if(g_dp104RuntimeStatus.firstFailureState.compare_exchange_strong(
           expectedState, 1u, std::memory_order_acq_rel, std::memory_order_relaxed))
    {
        StoreFailureFields(request,
                           reason,
                           bytesRead,
                           elapsedMs,
                           g_dp104RuntimeStatus.firstFailureOp,
                           g_dp104RuntimeStatus.firstFailureTxn,
                           g_dp104RuntimeStatus.firstFailureHeader0,
                           g_dp104RuntimeStatus.firstFailureHeader1,
                           g_dp104RuntimeStatus.firstFailureHeader2,
                           g_dp104RuntimeStatus.firstFailureHeader3,
                           g_dp104RuntimeStatus.firstFailureReason,
                           g_dp104RuntimeStatus.firstFailureElapsedMs,
                           g_dp104RuntimeStatus.firstFailureBytesRead);
        g_dp104RuntimeStatus.firstFailureState.store(2u, std::memory_order_release);
    }

    const uint32_t previousSequence = g_dp104RuntimeStatus.lastFailureSequence.fetch_add(
        1u, std::memory_order_acq_rel);
    StoreFailureFields(request,
                       reason,
                       bytesRead,
                       elapsedMs,
                       g_dp104RuntimeStatus.lastFailureOp,
                       g_dp104RuntimeStatus.lastFailureTxn,
                       g_dp104RuntimeStatus.lastFailureHeader0,
                       g_dp104RuntimeStatus.lastFailureHeader1,
                       g_dp104RuntimeStatus.lastFailureHeader2,
                       g_dp104RuntimeStatus.lastFailureHeader3,
                       g_dp104RuntimeStatus.lastFailureReason,
                       g_dp104RuntimeStatus.lastFailureElapsedMs,
                       g_dp104RuntimeStatus.lastFailureBytesRead);
    g_dp104RuntimeStatus.lastFailureSequence.store(previousSequence + 2u,
                                                    std::memory_order_release);
}

#if defined(DP104_CAPTURE_FAILURE_DIAGNOSTICS) && DP104_CAPTURE_FAILURE_DIAGNOSTICS
struct OriginalForeignSummary
{
    uint32_t count = 0u;
    bool firstSeen = false;
    uint32_t lateReportKind = 0u;
    host::Report first = {};
    host::Report last = {};
    host::Report lateReport = {};
};

struct DirectQueryResult
{
    uint32_t status = DP104_CAPTURE_QUERY_NOT_ATTEMPTED;
    int bytesRead = 0;
    uint32_t foreignReports = 0u;
    uint32_t elapsedMs = 0u;
    bool fatal = false;
    bool responsePresent = false;
    host::Report response = {};
};

bool AllZero(const uint8_t* bytes, size_t begin, size_t end)
{
    for(size_t i = begin; i < end; ++i)
        if(bytes[i] != 0u) return false;
    return true;
}

bool Nonzero(const uint8_t* bytes, size_t length)
{
    for(size_t i = 0; i < length; ++i)
        if(bytes[i] != 0u) return true;
    return false;
}

void RecordOriginalForeign(OriginalForeignSummary& summary,
                           const host::Report& report)
{
    if(!summary.firstSeen)
    {
        summary.first = report;
        summary.firstSeen = true;
    }
    summary.last = report;
    ++summary.count;
}

void RecordCaptureForeign(Dp104FailureCapture& capture,
                          size_t queryIndex,
                          const host::Report& report)
{
    if(capture.foreignReportsDrained == 0u)
        std::memcpy(capture.foreignFirstReport, report.data(), DP104_REPORT_BYTES);
    std::memcpy(capture.foreignLastReport, report.data(), DP104_REPORT_BYTES);
    ++capture.foreignReportsDrained;
    ++capture.queryForeignReports[queryIndex];
}

uint8_t NextDiagnosticTransaction(Dp104FailureCapture& capture)
{
    uint32_t next = capture.nextDiagnosticTransaction;
    if(next == 0u || next > 255u) next = 1u;
    capture.nextDiagnosticTransaction = next == 255u ? 1u : next + 1u;
    return static_cast<uint8_t>(next);
}

host::Report MakeDiagnosticRequest(uint8_t operation, uint8_t transaction)
{
    host::Report request = {};
    request[0] = DP104_COMMAND;
    request[1] = operation;
    request[2] = transaction;
    return request;
}

bool GenerateCaptureNonce(uint8_t nonce[16])
{
    try
    {
        std::random_device random;
        for(unsigned attempt = 0; attempt < 8u; ++attempt)
        {
            for(size_t i = 0; i < 16u; i += sizeof(uint32_t))
            {
                const uint32_t word = static_cast<uint32_t>(random());
                for(size_t byte = 0; byte < sizeof(uint32_t); ++byte)
                    nonce[i + byte] = static_cast<uint8_t>(word >> (byte * 8u));
            }
            if(Nonzero(nonce, 16u) && Nonzero(nonce, 4u)) return true;
        }
    }
    catch(...)
    {
    }
    return false;
}

uint32_t ElapsedMilliseconds(
    const std::chrono::steady_clock::time_point& started)
{
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());
}

DirectQueryResult DirectDiagnosticExchange(hid_device* device,
                                           const host::Report& request,
                                           Dp104FailureCapture& capture,
                                           size_t queryIndex,
                                           bool priorityAfterFailure = false)
{
    DirectQueryResult result;
    const auto started = std::chrono::steady_clock::now();
    std::array<unsigned char, kHidOutputReportBytes> output = {{0}};
    std::array<unsigned char, kHidInputReportBytes> input = {{0}};
    ++capture.diagnosticQueriesAttempted;

    if(!priorityAfterFailure &&
       capture.foreignReportsDrained >= kForeignReportsCaptureLimit)
    {
        result.status = DP104_CAPTURE_QUERY_FOREIGN_LIMIT;
        ++capture.foreignDrainLimitHits;
        goto finished;
    }

    output[0] = 0u;
    std::memcpy(&output[1], request.data(), DP104_REPORT_BYTES);
    if(device == nullptr || hid_write(device, output.data(), output.size()) !=
                             kHidOutputReportBytes)
    {
        result.status = DP104_CAPTURE_QUERY_WRITE_ERROR;
        result.fatal = true;
        goto finished;
    }

    {
        const auto deadline = started + std::chrono::milliseconds(kReadTimeoutMs);
        for(;;)
        {
            const auto now = std::chrono::steady_clock::now();
            if(now >= deadline)
            {
                result.status = DP104_CAPTURE_QUERY_TIMEOUT;
                break;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now);
            const int timeoutMs = remaining.count() < 1
                ? 1 : static_cast<int>(remaining.count());
            input.fill(0u);
            const int bytesRead = hid_read_timeout(device, input.data(), input.size(), timeoutMs);
            result.bytesRead = bytesRead;
            if(bytesRead < 0)
            {
                result.status = DP104_CAPTURE_QUERY_READ_ERROR;
                result.fatal = true;
                break;
            }
            if(bytesRead == 0)
            {
                result.status = DP104_CAPTURE_QUERY_TIMEOUT;
                break;
            }
            if(bytesRead != kHidInputReportBytes)
            {
                result.status = DP104_CAPTURE_QUERY_SHORT_READ;
                break;
            }

            const bool matchingHeader = input[0] == DP104_COMMAND &&
                input[1] == request[1] && input[2] == request[2];
            if(std::chrono::steady_clock::now() >= deadline)
            {
                if(matchingHeader)
                {
                    std::memcpy(result.response.data(), input.data(), DP104_REPORT_BYTES);
                    result.responsePresent = true;
                    ++capture.diagnosticQueriesMatched;
                }
                result.status = DP104_CAPTURE_QUERY_LATE;
                break;
            }
            if(matchingHeader)
            {
                std::memcpy(result.response.data(), input.data(), DP104_REPORT_BYTES);
                result.responsePresent = true;
                ++capture.diagnosticQueriesMatched;
                result.status = input[3] == DP104_OK
                    ? DP104_CAPTURE_QUERY_OK : DP104_CAPTURE_QUERY_DEVICE_STATUS;
                break;
            }

            host::Report foreign = {};
            std::memcpy(foreign.data(), input.data(), DP104_REPORT_BYTES);
            RecordCaptureForeign(capture, queryIndex, foreign);
            ++result.foreignReports;
            if(result.foreignReports >= kForeignReportsPerQueryLimit ||
               (!priorityAfterFailure &&
                capture.foreignReportsDrained >= kForeignReportsCaptureLimit))
            {
                result.status = DP104_CAPTURE_QUERY_FOREIGN_LIMIT;
                ++capture.foreignDrainLimitHits;
                break;
            }
        }
    }

finished:
    result.elapsedMs = ElapsedMilliseconds(started);
    if(queryIndex == DP104_CAPTURE_QUERY_F5_PAGES)
    {
        capture.queryBytesRead[queryIndex] += static_cast<uint32_t>(result.bytesRead);
        capture.queryForeignReports[queryIndex] += 0u;
        capture.queryElapsedMs[queryIndex] += result.elapsedMs;
        if(capture.queryStatus[queryIndex] == DP104_CAPTURE_QUERY_NOT_ATTEMPTED ||
           capture.queryStatus[queryIndex] == DP104_CAPTURE_QUERY_OK)
            capture.queryStatus[queryIndex] = result.status;
    }
    else
    {
        capture.queryBytesRead[queryIndex] = static_cast<uint32_t>(result.bytesRead);
        capture.queryElapsedMs[queryIndex] = result.elapsedMs;
        capture.queryStatus[queryIndex] = result.status;
    }
    return result;
}

bool IsSuccessfulCaps(const host::Report& request, const host::Report& response)
{
    static const char magic[] = DP104_CAPS_MAGIC;
    return request[0] == DP104_COMMAND && request[1] == DP104_OP_CAPS &&
        request[3] == 0u && AllZero(request.data(), 4u, DP104_REPORT_BYTES) &&
        response[0] == DP104_COMMAND && response[1] == DP104_OP_CAPS &&
        response[2] == request[2] && response[3] == DP104_OK &&
        std::memcmp(response.data() + 4u, magic, sizeof(magic) - 1u) == 0 &&
        response[12] == 1u && response[13] == 0u &&
        response[14] == DP104_LED_COUNT && response[15] == 0u &&
        response[16] == DP104_CHUNK_LEDS && response[17] == 1u &&
        response[18] == static_cast<uint8_t>(DP104_LEASE_MS & 0xffu) &&
        response[19] == static_cast<uint8_t>(DP104_LEASE_MS >> 8) &&
        AllZero(response.data(), 20u, DP104_REPORT_BYTES);
}

bool ValidateAppInfo(const host::Report& request, const host::Report& response,
                     const uint8_t nonce[16])
{
    static const char magic[] = DP104_CAPS_MAGIC;
    return response[0] == DP104_COMMAND && response[1] == kAppInfoOperation &&
        response[2] == request[2] && response[3] == DP104_OK &&
        std::memcmp(response.data() + 4u, magic, sizeof(magic) - 1u) == 0 &&
        response[12] == 1u && response[13] == 2u && response[14] == 1u &&
        response[15] == 1u && std::memcmp(response.data() + 16u, nonce, 16u) == 0;
}

bool ValidateMasterSnapshot(const host::Report& request,
                            const host::Report& response,
                            const uint8_t nonce[4])
{
    static const uint8_t magic[8] = {'D', 'P', '1', '0', '4', 'M', '4', 0};
    return response[0] == DP104_COMMAND && response[1] == kMasterSnapshotOperation &&
        response[2] == request[2] && response[3] == DP104_OK &&
        std::memcmp(response.data() + 4u, nonce, 4u) == 0 &&
        std::memcmp(response.data() + 8u, magic, sizeof(magic)) == 0;
}

bool ValidateF5(const host::Report& request, const host::Report& response,
                const uint8_t nonce[4], uint16_t expectedOffset, bool preflight,
                bool arm)
{
    static const uint8_t magic[4] = {'P', '5', 'R', 'X'};
    if(response[0] != DP104_COMMAND || response[1] != kPlcDiagnosticOperation ||
       response[2] != request[2] || response[3] != DP104_OK ||
       std::memcmp(response.data() + 4u, nonce, 4u) != 0 ||
       std::memcmp(response.data() + 8u, magic, sizeof(magic)) != 0 ||
       dp104_read16(response.data() + 12u) != expectedOffset ||
       dp104_read16(response.data() + 14u) != DP104_FAILURE_CAPTURE_F5_BYTES)
        return false;
    if(preflight)
        return dp104_read32(response.data() + 16u) == 1u &&
            dp104_read32(response.data() + 20u) == 32u &&
            dp104_read32(response.data() + 24u) == 64u &&
            dp104_read32(response.data() + 28u) == DP104_FAILURE_CAPTURE_F5_BYTES;
    if(arm) return AllZero(response.data(), 16u, DP104_REPORT_BYTES);
    return true;
}

void SetFailureStatus(Dp104FailureCapture& capture, size_t queryIndex,
                      uint32_t status)
{
    capture.queryStatus[queryIndex] = status;
}

void MarkF5Failure(Dp104FailureCapture& capture, uint16_t offset, uint32_t status)
{
    ++capture.f5PagesFailed;
    if(capture.f5FirstFailedOffset == UINT32_MAX)
    {
        capture.f5FirstFailedOffset = offset;
        capture.f5FirstFailedStatus = status;
    }
}

void MarkF5Success(Dp104FailureCapture& capture, uint16_t offset,
                   const uint8_t data[16])
{
    std::memcpy(capture.f5Blob + offset, data, 16u);
    const size_t page = offset / 16u;
    capture.f5PageSuccessBitmap[page / 8u] |=
        static_cast<uint8_t>(1u << (page % 8u));
    ++capture.f5PagesSucceeded;
}

void CaptureFailure(hid_device* device, const host::Report& request,
                    uint32_t reason, int bytesRead,
                    const OriginalForeignSummary& foreign)
{
    Dp104FailureCapture& capture = g_dp104FailureCapture;
    uint32_t expected = DP104_CAPTURE_ARMED;
    if(request[0] != DP104_COMMAND || request[1] < DP104_OP_BEGIN ||
       request[1] > DP104_OP_COMMIT ||
       !(reason == DP104_RUNTIME_FAILURE_READ ||
         reason == DP104_RUNTIME_FAILURE_TIMEOUT ||
         reason == DP104_RUNTIME_FAILURE_SHORT_READ ||
         reason == DP104_RUNTIME_FAILURE_LATE ||
         reason == DP104_RUNTIME_FAILURE_MISMATCH_TIMEOUT) ||
       !capture.state.compare_exchange_strong(expected, DP104_CAPTURE_BUSY,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return;

    capture.originalFailureReason = reason;
    capture.originalFailureElapsedMs =
        g_dp104RuntimeStatus.lastFailureElapsedMs.load(std::memory_order_relaxed);
    capture.originalFailureBytesRead = static_cast<uint32_t>(bytesRead);
    capture.originalFailureOperation = request[1];
    capture.originalFailureTransaction = request[2];
    std::memcpy(capture.originalFailureRequest, request.data(), DP104_REPORT_BYTES);
    capture.runtimeCounters[DP104_CAPTURE_COUNTER_EXCHANGES] =
        g_dp104RuntimeStatus.exchangeCount.load(std::memory_order_relaxed);
    capture.runtimeCounters[DP104_CAPTURE_COUNTER_SUCCESSES] =
        g_dp104RuntimeStatus.exchangeSuccesses.load(std::memory_order_relaxed);
    capture.runtimeCounters[DP104_CAPTURE_COUNTER_FAILURES] =
        g_dp104RuntimeStatus.exchangeFailures.load(std::memory_order_relaxed);
    capture.runtimeCounters[DP104_CAPTURE_COUNTER_FRAMES_QUEUED] =
        g_dp104RuntimeStatus.framesQueued.load(std::memory_order_relaxed);
    capture.runtimeCounters[DP104_CAPTURE_COUNTER_FRAMES_STARTED] =
        g_dp104RuntimeStatus.framesStarted.load(std::memory_order_relaxed);
    capture.runtimeCounters[DP104_CAPTURE_COUNTER_FRAMES_COMMITTED] =
        g_dp104RuntimeStatus.framesCommitted.load(std::memory_order_relaxed);
    capture.runtimeCounters[DP104_CAPTURE_COUNTER_FRAMES_FAILED] =
        g_dp104RuntimeStatus.framesFailed.load(std::memory_order_relaxed);
    capture.runtimeCounters[DP104_CAPTURE_COUNTER_RECOVERY_ATTEMPTS] =
        g_dp104RuntimeStatus.recoveryAttempts.load(std::memory_order_relaxed);
    capture.runtimeCounters[DP104_CAPTURE_COUNTER_RECOVERY_SUCCEEDED] =
        g_dp104RuntimeStatus.recoverySucceeded.load(std::memory_order_relaxed);
    capture.runtimeCounters[DP104_CAPTURE_COUNTER_RECOVERY_FAILURES] =
        g_dp104RuntimeStatus.recoveryFailures.load(std::memory_order_relaxed);
    capture.runtimeCounters[DP104_CAPTURE_COUNTER_RECOVERY_STATE] =
        g_dp104RuntimeStatus.recoveryState.load(std::memory_order_relaxed);
    capture.runtimeCounters[DP104_CAPTURE_COUNTER_PROTOCOL_READY] =
        g_dp104RuntimeStatus.protocolReady.load(std::memory_order_relaxed);
    capture.originalForeignCount = foreign.count;
    capture.originalForeignFirstSeen = foreign.firstSeen ? 1u : 0u;
    capture.originalLateReportSeen = foreign.lateReportKind;
    if(foreign.firstSeen)
    {
        std::memcpy(capture.originalForeignFirstReport, foreign.first.data(),
                    DP104_REPORT_BYTES);
        std::memcpy(capture.originalForeignLastReport, foreign.last.data(),
                    DP104_REPORT_BYTES);
    }
    if(foreign.lateReportKind != 0u)
        std::memcpy(capture.originalLateReport, foreign.lateReport.data(),
                    DP104_REPORT_BYTES);
    capture.state.store(DP104_CAPTURE_CAPTURING, std::memory_order_release);

    const uint8_t* diagnosticNonce = capture.nonce;
    bool fatal = false;
    bool freezeValid = false;

    host::Report masterRequest = MakeDiagnosticRequest(
        kMasterSnapshotOperation, NextDiagnosticTransaction(capture));
    std::memcpy(masterRequest.data() + 4u, diagnosticNonce, 4u);
    DirectQueryResult master = DirectDiagnosticExchange(device, masterRequest,
        capture, DP104_CAPTURE_QUERY_MASTER_FAILURE, true);
    if(master.responsePresent)
        std::memcpy(capture.masterFailureResponse, master.response.data(),
                    DP104_REPORT_BYTES);
    if(master.status == DP104_CAPTURE_QUERY_OK &&
       !ValidateMasterSnapshot(masterRequest, master.response, diagnosticNonce))
        SetFailureStatus(capture, DP104_CAPTURE_QUERY_MASTER_FAILURE,
                         DP104_CAPTURE_QUERY_BAD_PAYLOAD);
    fatal = master.fatal;

    if(!fatal)
    {
        host::Report freezeRequest = MakeDiagnosticRequest(
            kPlcDiagnosticOperation, NextDiagnosticTransaction(capture));
        std::memcpy(freezeRequest.data() + 4u, diagnosticNonce, 4u);
        dp104_write16(freezeRequest.data() + 8u, 0u);
        ++capture.f5PagesAttempted;
        DirectQueryResult freeze = DirectDiagnosticExchange(device, freezeRequest,
            capture, DP104_CAPTURE_QUERY_PLC_FREEZE, true);
        if(freeze.status == DP104_CAPTURE_QUERY_OK &&
           ValidateF5(freezeRequest, freeze.response, diagnosticNonce, 0u, false, false))
        {
            MarkF5Success(capture, 0u, freeze.response.data() + 16u);
            freezeValid = true;
        }
        else
        {
            uint32_t status = freeze.status;
            if(status == DP104_CAPTURE_QUERY_OK)
                status = DP104_CAPTURE_QUERY_BAD_PAYLOAD;
            SetFailureStatus(capture, DP104_CAPTURE_QUERY_PLC_FREEZE, status);
            MarkF5Failure(capture, 0u, status);
        }
        fatal = freeze.fatal;
    }

    if(!fatal)
    {
        host::Report receiverRequest = MakeDiagnosticRequest(
            kReceiverSnapshotOperation, NextDiagnosticTransaction(capture));
        std::memcpy(receiverRequest.data() + 4u, diagnosticNonce, 4u);
        DirectQueryResult receiver = DirectDiagnosticExchange(device, receiverRequest,
            capture, DP104_CAPTURE_QUERY_RECEIVER, true);
        if(receiver.responsePresent)
            std::memcpy(capture.receiverFailureResponse, receiver.response.data(),
                        DP104_REPORT_BYTES);
        if(receiver.status == DP104_CAPTURE_QUERY_OK &&
           (receiver.response[4] != diagnosticNonce[0] ||
            receiver.response[5] != diagnosticNonce[1] ||
            receiver.response[6] != diagnosticNonce[2] ||
            receiver.response[7] != diagnosticNonce[3] ||
            (receiver.response[8] & 0xfcu) != 0u))
            SetFailureStatus(capture, DP104_CAPTURE_QUERY_RECEIVER,
                             DP104_CAPTURE_QUERY_BAD_PAYLOAD);
        fatal = receiver.fatal;
    }

    if(!fatal && freezeValid)
    {
        for(uint16_t offset = 16u; offset < DP104_FAILURE_CAPTURE_F5_BYTES;
            offset = static_cast<uint16_t>(offset + 16u))
        {
            if(capture.foreignReportsDrained >= kForeignReportsCaptureLimit)
            {
                capture.queryStatus[DP104_CAPTURE_QUERY_F5_PAGES] =
                    DP104_CAPTURE_QUERY_FOREIGN_LIMIT;
                ++capture.foreignDrainLimitHits;
                break;
            }
            host::Report pageRequest = MakeDiagnosticRequest(
                kPlcDiagnosticOperation, NextDiagnosticTransaction(capture));
            std::memcpy(pageRequest.data() + 4u, diagnosticNonce, 4u);
            dp104_write16(pageRequest.data() + 8u, offset);
            ++capture.f5PagesAttempted;
            DirectQueryResult page = DirectDiagnosticExchange(device, pageRequest,
                capture, DP104_CAPTURE_QUERY_F5_PAGES);
            if(page.status == DP104_CAPTURE_QUERY_OK &&
               ValidateF5(pageRequest, page.response, diagnosticNonce,
                          offset, false, false))
                MarkF5Success(capture, offset, page.response.data() + 16u);
            else
            {
                uint32_t status = page.status;
                if(status == DP104_CAPTURE_QUERY_OK)
                    status = DP104_CAPTURE_QUERY_BAD_PAYLOAD;
                capture.queryStatus[DP104_CAPTURE_QUERY_F5_PAGES] = status;
                MarkF5Failure(capture, offset, status);
            }
            if(page.fatal)
            {
                fatal = true;
                break;
            }
        }
    }

    const bool allF5Pages = capture.f5PagesSucceeded == DP104_FAILURE_CAPTURE_F5_PAGES;
    const bool complete = !fatal &&
        capture.queryStatus[DP104_CAPTURE_QUERY_MASTER_FAILURE] == DP104_CAPTURE_QUERY_OK &&
        freezeValid &&
        capture.queryStatus[DP104_CAPTURE_QUERY_RECEIVER] == DP104_CAPTURE_QUERY_OK &&
        allF5Pages;
    capture.state.store(complete ? DP104_CAPTURE_COMPLETE : DP104_CAPTURE_PARTIAL,
                        std::memory_order_release);
}

void ArmFailureCapture(hid_device* device, const host::Report& request,
                       const host::Report& response)
{
    Dp104FailureCapture& capture = g_dp104FailureCapture;
    uint32_t expected = DP104_CAPTURE_IDLE;
    if(!IsSuccessfulCaps(request, response) ||
       !capture.state.compare_exchange_strong(expected, DP104_CAPTURE_BUSY,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return;

    std::memcpy(capture.capsResponse, response.data(), DP104_REPORT_BYTES);
    capture.f5FirstFailedOffset = UINT32_MAX;
    capture.nextDiagnosticTransaction = 0xe0u;
    if(!GenerateCaptureNonce(capture.nonce))
    {
        capture.state.store(DP104_CAPTURE_DISABLED, std::memory_order_release);
        return;
    }

    host::Report appInfoRequest = MakeDiagnosticRequest(
        kAppInfoOperation, NextDiagnosticTransaction(capture));
    std::memcpy(appInfoRequest.data() + 4u, capture.nonce, sizeof(capture.nonce));
    DirectQueryResult appInfo = DirectDiagnosticExchange(device, appInfoRequest,
        capture, DP104_CAPTURE_QUERY_APP_INFO);
    if(appInfo.responsePresent)
        std::memcpy(capture.appInfoResponse, appInfo.response.data(), DP104_REPORT_BYTES);
    if(appInfo.status == DP104_CAPTURE_QUERY_OK &&
       !ValidateAppInfo(appInfoRequest, appInfo.response, capture.nonce))
        SetFailureStatus(capture, DP104_CAPTURE_QUERY_APP_INFO,
                         DP104_CAPTURE_QUERY_BAD_PAYLOAD);
    if(appInfo.status != DP104_CAPTURE_QUERY_OK ||
       capture.queryStatus[DP104_CAPTURE_QUERY_APP_INFO] != DP104_CAPTURE_QUERY_OK)
    {
        capture.state.store(DP104_CAPTURE_DISABLED, std::memory_order_release);
        return;
    }

    host::Report masterRequest = MakeDiagnosticRequest(
        kMasterSnapshotOperation, NextDiagnosticTransaction(capture));
    std::memcpy(masterRequest.data() + 4u, capture.nonce, 4u);
    DirectQueryResult master = DirectDiagnosticExchange(device, masterRequest,
        capture, DP104_CAPTURE_QUERY_MASTER_PREFLIGHT);
    if(master.responsePresent)
        std::memcpy(capture.masterPreflightResponse, master.response.data(),
                    DP104_REPORT_BYTES);
    if(master.status == DP104_CAPTURE_QUERY_OK &&
       !ValidateMasterSnapshot(masterRequest, master.response, capture.nonce))
        SetFailureStatus(capture, DP104_CAPTURE_QUERY_MASTER_PREFLIGHT,
                         DP104_CAPTURE_QUERY_BAD_PAYLOAD);
    if(master.status != DP104_CAPTURE_QUERY_OK ||
       capture.queryStatus[DP104_CAPTURE_QUERY_MASTER_PREFLIGHT] != DP104_CAPTURE_QUERY_OK)
    {
        capture.state.store(DP104_CAPTURE_DISABLED, std::memory_order_release);
        return;
    }

    host::Report plcPreflightRequest = MakeDiagnosticRequest(
        kPlcDiagnosticOperation, NextDiagnosticTransaction(capture));
    std::memcpy(plcPreflightRequest.data() + 4u, capture.nonce, 4u);
    dp104_write16(plcPreflightRequest.data() + 8u, kPlcDiagnosticPreflight);
    DirectQueryResult plcPreflight = DirectDiagnosticExchange(device, plcPreflightRequest,
        capture, DP104_CAPTURE_QUERY_PLC_PREFLIGHT);
    if(plcPreflight.responsePresent)
        std::memcpy(capture.plcPreflightResponse, plcPreflight.response.data(),
                    DP104_REPORT_BYTES);
    if(plcPreflight.status == DP104_CAPTURE_QUERY_OK &&
       !ValidateF5(plcPreflightRequest, plcPreflight.response, capture.nonce,
                   kPlcDiagnosticPreflight, true, false))
        SetFailureStatus(capture, DP104_CAPTURE_QUERY_PLC_PREFLIGHT,
                         DP104_CAPTURE_QUERY_BAD_PAYLOAD);
    if(plcPreflight.status != DP104_CAPTURE_QUERY_OK ||
       capture.queryStatus[DP104_CAPTURE_QUERY_PLC_PREFLIGHT] != DP104_CAPTURE_QUERY_OK)
    {
        capture.state.store(DP104_CAPTURE_DISABLED, std::memory_order_release);
        return;
    }

    host::Report plcArmRequest = MakeDiagnosticRequest(
        kPlcDiagnosticOperation, NextDiagnosticTransaction(capture));
    std::memcpy(plcArmRequest.data() + 4u, capture.nonce, 4u);
    dp104_write16(plcArmRequest.data() + 8u, kPlcDiagnosticArm);
    DirectQueryResult plcArm = DirectDiagnosticExchange(device, plcArmRequest,
        capture, DP104_CAPTURE_QUERY_PLC_ARM);
    if(plcArm.responsePresent)
        std::memcpy(capture.plcArmResponse, plcArm.response.data(), DP104_REPORT_BYTES);
    if(plcArm.status == DP104_CAPTURE_QUERY_OK &&
       !ValidateF5(plcArmRequest, plcArm.response, capture.nonce,
                   kPlcDiagnosticArm, false, true))
        SetFailureStatus(capture, DP104_CAPTURE_QUERY_PLC_ARM,
                         DP104_CAPTURE_QUERY_BAD_PAYLOAD);
    if(plcArm.status != DP104_CAPTURE_QUERY_OK ||
       capture.queryStatus[DP104_CAPTURE_QUERY_PLC_ARM] != DP104_CAPTURE_QUERY_OK)
    {
        capture.state.store(DP104_CAPTURE_DISABLED, std::memory_order_release);
        return;
    }

    capture.state.store(DP104_CAPTURE_ARMED, std::memory_order_release);
}
#endif
}

Dp104RuntimeStatus::Dp104RuntimeStatus()
    : magic(0x44503134u),
      schemaVersion(1u),
      byteSize(static_cast<uint32_t>(sizeof(Dp104RuntimeStatus))),
      exchangeCount(0u),
      exchangeSuccesses(0u),
      exchangeFailures(0u),
      firstFailureState(0u),
      firstFailureOp(0u),
      firstFailureTxn(0u),
      firstFailureHeader0(0u),
      firstFailureHeader1(0u),
      firstFailureHeader2(0u),
      firstFailureHeader3(0u),
      firstFailureReason(DP104_RUNTIME_FAILURE_NONE),
      firstFailureElapsedMs(0u),
      firstFailureBytesRead(0u),
      lastFailureSequence(0u),
      lastFailureOp(0u),
      lastFailureTxn(0u),
      lastFailureHeader0(0u),
      lastFailureHeader1(0u),
      lastFailureHeader2(0u),
      lastFailureHeader3(0u),
      lastFailureReason(DP104_RUNTIME_FAILURE_NONE),
      lastFailureElapsedMs(0u),
      lastFailureBytesRead(0u),
      framesQueued(0u),
      framesStarted(0u),
      framesCommitted(0u),
      framesFailed(0u),
      recoveryAttempts(0u),
      recoverySucceeded(0u),
      recoveryFailures(0u),
      recoveryState(DP104_RUNTIME_RECOVERY_READY),
      protocolReady(0u),
      reserved(0u)
{
}

std::recursive_mutex& Dp104HidTransport::SharedProtocolMutex()
{
    static std::recursive_mutex mutex;
    return mutex;
}

Dp104HidTransport::ProtocolSession::ProtocolSession()
    : lock_(Dp104HidTransport::SharedProtocolMutex())
{
}

Dp104HidTransport::Dp104HidTransport(hid_device* device)
    : device_(device)
{
}

Dp104HidTransport::~Dp104HidTransport()
{
    Close();
}

bool Dp104HidTransport::Exchange(const host::Report& request, host::Report& response)
{
    ProtocolSession protocolSession;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::chrono::steady_clock::time_point exchangeStarted =
        std::chrono::steady_clock::now();
#if defined(DP104_CAPTURE_FAILURE_DIAGNOSTICS) && DP104_CAPTURE_FAILURE_DIAGNOSTICS
    OriginalForeignSummary originalForeign;
#endif
    g_dp104RuntimeStatus.exchangeCount.fetch_add(1u, std::memory_order_relaxed);
    if(device_ == nullptr)
    {
        RecordFailure(request, DP104_RUNTIME_FAILURE_CLOSED, 0, exchangeStarted);
        return false;
    }

    std::array<unsigned char, kHidOutputReportBytes> output = {{0}};
    std::array<unsigned char, kHidInputReportBytes> input = {{0}};
    std::memcpy(&output[1], request.data(), DP104_REPORT_BYTES);

    const int bytesWritten = hid_write(device_, output.data(), output.size());
    if(bytesWritten != kHidOutputReportBytes)
    {
        RecordFailure(request, DP104_RUNTIME_FAILURE_WRITE, 0, exchangeStarted);
        return false;
    }

    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kReadTimeoutMs);
    bool sawMismatchedReport = false;
    for(;;)
    {
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if(now >= deadline)
        {
            const uint32_t reason = sawMismatchedReport
                ? DP104_RUNTIME_FAILURE_MISMATCH_TIMEOUT
                : DP104_RUNTIME_FAILURE_TIMEOUT;
            RecordFailure(request, reason, 0, exchangeStarted);
#if defined(DP104_CAPTURE_FAILURE_DIAGNOSTICS) && DP104_CAPTURE_FAILURE_DIAGNOSTICS
            CaptureFailure(device_, request, reason, 0, originalForeign);
#endif
            return false;
        }

        const std::chrono::milliseconds remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const int timeoutMs = remaining.count() < 1
            ? 1
            : static_cast<int>(remaining.count());
        const int bytesRead = hid_read_timeout(device_, input.data(), input.size(), timeoutMs);
        if(bytesRead == 0)
            continue;
        if(bytesRead != kHidInputReportBytes)
        {
            const uint32_t reason = bytesRead > 0
                ? DP104_RUNTIME_FAILURE_SHORT_READ : DP104_RUNTIME_FAILURE_READ;
            RecordFailure(request, reason, bytesRead, exchangeStarted);
#if defined(DP104_CAPTURE_FAILURE_DIAGNOSTICS) && DP104_CAPTURE_FAILURE_DIAGNOSTICS
            CaptureFailure(device_, request, reason, bytesRead, originalForeign);
#endif
            return false;
        }

        if(std::chrono::steady_clock::now() >= deadline)
        {
#if defined(DP104_CAPTURE_FAILURE_DIAGNOSTICS) && DP104_CAPTURE_FAILURE_DIAGNOSTICS
            std::memcpy(originalForeign.lateReport.data(), input.data(), DP104_REPORT_BYTES);
            originalForeign.lateReportKind = 1u;
#endif
            RecordFailure(request, DP104_RUNTIME_FAILURE_LATE, bytesRead, exchangeStarted);
#if defined(DP104_CAPTURE_FAILURE_DIAGNOSTICS) && DP104_CAPTURE_FAILURE_DIAGNOSTICS
            CaptureFailure(device_, request, DP104_RUNTIME_FAILURE_LATE,
                           bytesRead, originalForeign);
#endif
            return false;
        }

        if(input[0] != DP104_COMMAND || request[0] != DP104_COMMAND ||
           input[1] != request[1] || input[2] != request[2])
        {
            sawMismatchedReport = true;
#if defined(DP104_CAPTURE_FAILURE_DIAGNOSTICS) && DP104_CAPTURE_FAILURE_DIAGNOSTICS
            host::Report foreign = {};
            std::memcpy(foreign.data(), input.data(), DP104_REPORT_BYTES);
            RecordOriginalForeign(originalForeign, foreign);
#endif
            continue;
        }

        std::memcpy(response.data(), input.data(), DP104_REPORT_BYTES);
        if(input[3] != DP104_OK)
        {
            RecordFailure(request,
                          DP104_RUNTIME_FAILURE_DEVICE_STATUS,
                          bytesRead,
                          exchangeStarted);
        }
        else
        {
            g_dp104RuntimeStatus.exchangeSuccesses.fetch_add(1u, std::memory_order_relaxed);
#if defined(DP104_CAPTURE_FAILURE_DIAGNOSTICS) && DP104_CAPTURE_FAILURE_DIAGNOSTICS
            if(request[1] == DP104_OP_CAPS)
                ArmFailureCapture(device_, request, response);
#endif
        }
        return true;
    }
}

bool Dp104HidTransport::IsOpen() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return device_ != nullptr;
}

void Dp104HidTransport::Close()
{
    ProtocolSession protocolSession;
    std::lock_guard<std::mutex> lock(mutex_);
    if(device_ != nullptr)
    {
        hid_close(device_);
        device_ = nullptr;
    }
}

} // namespace openrgb
} // namespace dp104

extern "C"
{
DP104_RUNTIME_EXPORT dp104::openrgb::Dp104RuntimeStatus g_dp104RuntimeStatus;
}

#if defined(DP104_CAPTURE_FAILURE_DIAGNOSTICS) && DP104_CAPTURE_FAILURE_DIAGNOSTICS
extern "C"
{
DP104_FAILURE_CAPTURE_EXPORT dp104::openrgb::Dp104FailureCapture g_dp104FailureCapture = {
    dp104::openrgb::DP104_FAILURE_CAPTURE_MAGIC,
    dp104::openrgb::DP104_FAILURE_CAPTURE_SCHEMA,
    static_cast<uint32_t>(sizeof(dp104::openrgb::Dp104FailureCapture)),
    ATOMIC_VAR_INIT(dp104::openrgb::DP104_CAPTURE_IDLE)
};
}
#endif
