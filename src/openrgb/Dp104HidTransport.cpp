#include "Dp104HidTransport.h"
#include "Dp104RuntimeStatus.h"

#include <array>
#include <chrono>
#include <cstring>

namespace dp104
{
namespace openrgb
{
namespace
{
const int kHidOutputReportBytes = DP104_REPORT_BYTES + 1;
const int kHidInputReportBytes = DP104_REPORT_BYTES;
const int kReadTimeoutMs = 100;

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
            RecordFailure(request,
                          sawMismatchedReport
                              ? DP104_RUNTIME_FAILURE_MISMATCH_TIMEOUT
                              : DP104_RUNTIME_FAILURE_TIMEOUT,
                          0,
                          exchangeStarted);
            return false;
        }

        const std::chrono::milliseconds remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const int timeoutMs = remaining.count() < 1
            ? 1
            : static_cast<int>(remaining.count());
        const int bytesRead = hid_read_timeout(device_, input.data(), input.size(), timeoutMs);
        if(bytesRead != kHidInputReportBytes)
        {
            const uint32_t reason = bytesRead > 0
                ? DP104_RUNTIME_FAILURE_SHORT_READ
                : (bytesRead == 0
                       ? (sawMismatchedReport
                              ? DP104_RUNTIME_FAILURE_MISMATCH_TIMEOUT
                              : DP104_RUNTIME_FAILURE_TIMEOUT)
                       : DP104_RUNTIME_FAILURE_READ);
            RecordFailure(request, reason, bytesRead, exchangeStarted);
            return false;
        }

        if(std::chrono::steady_clock::now() >= deadline)
        {
            RecordFailure(request, DP104_RUNTIME_FAILURE_LATE, bytesRead, exchangeStarted);
            return false;
        }

        if(input[0] != DP104_COMMAND || request[0] != DP104_COMMAND ||
           input[1] != request[1] || input[2] != request[2])
        {
            sawMismatchedReport = true;
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
