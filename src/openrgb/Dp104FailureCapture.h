#ifndef DP104_FAILURE_CAPTURE_H
#define DP104_FAILURE_CAPTURE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dp104
{
namespace openrgb
{

constexpr uint32_t DP104_FAILURE_CAPTURE_MAGIC = 0x44464331u; /* DFC1 */
constexpr uint32_t DP104_FAILURE_CAPTURE_SCHEMA = 1u;
constexpr size_t DP104_FAILURE_CAPTURE_F5_BYTES = 2112u;
constexpr size_t DP104_FAILURE_CAPTURE_F5_PAGES = 132u;
constexpr size_t DP104_FAILURE_CAPTURE_F5_BITMAP_BYTES = 17u;

enum Dp104FailureCaptureState : uint32_t
{
    DP104_CAPTURE_IDLE = 0u,
    DP104_CAPTURE_BUSY = 1u,
    DP104_CAPTURE_ARMED = 2u,
    DP104_CAPTURE_CAPTURING = 3u,
    DP104_CAPTURE_COMPLETE = 4u,
    DP104_CAPTURE_PARTIAL = 5u,
    DP104_CAPTURE_DISABLED = 6u
};

enum Dp104CaptureQueryStatus : uint32_t
{
    DP104_CAPTURE_QUERY_NOT_ATTEMPTED = 0u,
    DP104_CAPTURE_QUERY_OK = 1u,
    DP104_CAPTURE_QUERY_WRITE_ERROR = 2u,
    DP104_CAPTURE_QUERY_TIMEOUT = 3u,
    DP104_CAPTURE_QUERY_READ_ERROR = 4u,
    DP104_CAPTURE_QUERY_SHORT_READ = 5u,
    DP104_CAPTURE_QUERY_FOREIGN_LIMIT = 6u,
    DP104_CAPTURE_QUERY_BAD_HEADER = 7u,
    DP104_CAPTURE_QUERY_BAD_PAYLOAD = 8u,
    DP104_CAPTURE_QUERY_DEVICE_STATUS = 9u,
    DP104_CAPTURE_QUERY_LATE = 10u
};

enum Dp104CaptureQueryIndex : size_t
{
    DP104_CAPTURE_QUERY_APP_INFO = 0u,
    DP104_CAPTURE_QUERY_MASTER_PREFLIGHT = 1u,
    DP104_CAPTURE_QUERY_PLC_PREFLIGHT = 2u,
    DP104_CAPTURE_QUERY_PLC_ARM = 3u,
    DP104_CAPTURE_QUERY_MASTER_FAILURE = 4u,
    DP104_CAPTURE_QUERY_PLC_FREEZE = 5u,
    DP104_CAPTURE_QUERY_RECEIVER = 6u,
    DP104_CAPTURE_QUERY_F5_PAGES = 7u
};

enum Dp104CaptureRuntimeCounterIndex : size_t
{
    DP104_CAPTURE_COUNTER_EXCHANGES = 0u,
    DP104_CAPTURE_COUNTER_SUCCESSES = 1u,
    DP104_CAPTURE_COUNTER_FAILURES = 2u,
    DP104_CAPTURE_COUNTER_FRAMES_QUEUED = 3u,
    DP104_CAPTURE_COUNTER_FRAMES_STARTED = 4u,
    DP104_CAPTURE_COUNTER_FRAMES_COMMITTED = 5u,
    DP104_CAPTURE_COUNTER_FRAMES_FAILED = 6u,
    DP104_CAPTURE_COUNTER_RECOVERY_ATTEMPTS = 7u,
    DP104_CAPTURE_COUNTER_RECOVERY_SUCCEEDED = 8u,
    DP104_CAPTURE_COUNTER_RECOVERY_FAILURES = 9u,
    DP104_CAPTURE_COUNTER_RECOVERY_STATE = 10u,
    DP104_CAPTURE_COUNTER_PROTOCOL_READY = 11u,
    DP104_CAPTURE_RUNTIME_COUNTER_COUNT = 12u
};

/* Fixed, read-only-from-outside ABI. Only state is atomic; state transitions
 * publish the surrounding POD fields. Readers must acquire-load state and
 * only consume the slot in ARMED, COMPLETE, PARTIAL, or DISABLED states. */
struct Dp104FailureCapture
{
    uint32_t magic;
    uint32_t schemaVersion;
    uint32_t byteSize;
    std::atomic<uint32_t> state;

    uint32_t originalFailureReason;
    uint32_t originalFailureElapsedMs;
    uint32_t originalFailureBytesRead;
    uint32_t originalFailureOperation;
    uint32_t originalFailureTransaction;
    uint8_t originalFailureRequest[32];
    uint32_t runtimeCounters[DP104_CAPTURE_RUNTIME_COUNTER_COUNT];

    uint32_t originalForeignCount;
    uint32_t originalForeignFirstSeen;
    /* 0 = none; 1 = full report seen after the Exchange deadline; 2 is
     * reserved for decoding schema-1 captures from an older passive-drain build. */
    uint32_t originalLateReportSeen;
    uint8_t originalForeignFirstReport[32];
    uint8_t originalForeignLastReport[32];
    uint8_t originalLateReport[32];

    uint8_t nonce[16];
    uint8_t capsResponse[32];
    uint8_t appInfoResponse[32];
    uint8_t masterPreflightResponse[32];
    uint8_t plcPreflightResponse[32];
    uint8_t plcArmResponse[32];
    uint8_t masterFailureResponse[32];
    uint8_t receiverFailureResponse[32];
    uint8_t f5Blob[DP104_FAILURE_CAPTURE_F5_BYTES];
    uint8_t f5PageSuccessBitmap[DP104_FAILURE_CAPTURE_F5_BITMAP_BYTES];

    uint32_t queryStatus[8];
    uint32_t queryBytesRead[8];
    uint32_t queryForeignReports[8];
    uint32_t queryElapsedMs[8];
    uint32_t f5PagesAttempted;
    uint32_t f5PagesSucceeded;
    uint32_t f5PagesFailed;
    uint32_t f5FirstFailedOffset;
    uint32_t f5FirstFailedStatus;
    uint32_t foreignReportsDrained;
    uint8_t foreignFirstReport[32];
    uint8_t foreignLastReport[32];
    uint32_t diagnosticQueriesAttempted;
    uint32_t diagnosticQueriesMatched;
    uint32_t foreignDrainLimitHits;
    uint32_t nextDiagnosticTransaction;
};

static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t),
              "failure capture state requires 32-bit atomics");
static_assert(alignof(std::atomic<uint32_t>) == alignof(uint32_t),
              "failure capture state requires 32-bit alignment");
static_assert(std::is_standard_layout<Dp104FailureCapture>::value,
              "failure capture ABI must remain standard layout");
static_assert(offsetof(Dp104FailureCapture, state) == 12u,
              "failure capture state offset changed");
static_assert(offsetof(Dp104FailureCapture, originalFailureRequest) == 36u,
              "failure capture request offset changed");
static_assert(offsetof(Dp104FailureCapture, runtimeCounters) == 68u,
              "failure capture counters offset changed");
static_assert(offsetof(Dp104FailureCapture, nonce) == 224u,
              "failure capture nonce offset changed");
static_assert(offsetof(Dp104FailureCapture, masterFailureResponse) == 400u,
              "failure capture F4 offset changed");
static_assert(offsetof(Dp104FailureCapture, receiverFailureResponse) == 432u,
              "failure capture F3 offset changed");
static_assert(offsetof(Dp104FailureCapture, f5Blob) == 464u,
              "failure capture F5 blob offset changed");
static_assert(offsetof(Dp104FailureCapture, f5PageSuccessBitmap) == 2576u,
              "failure capture F5 bitmap offset changed");
static_assert(offsetof(Dp104FailureCapture, queryStatus) == 2596u,
              "failure capture query status offset changed");
static_assert(offsetof(Dp104FailureCapture, f5PagesAttempted) == 2724u,
              "failure capture page counter offset changed");
static_assert(offsetof(Dp104FailureCapture, foreignReportsDrained) == 2744u,
              "failure capture foreign count offset changed");
static_assert(offsetof(Dp104FailureCapture, foreignFirstReport) == 2748u,
              "failure capture first foreign report offset changed");
static_assert(offsetof(Dp104FailureCapture, nextDiagnosticTransaction) == 2824u,
              "failure capture next transaction offset changed");
static_assert(sizeof(Dp104FailureCapture) == 2828u,
              "failure capture ABI size changed");

} // namespace openrgb
} // namespace dp104

#if defined(_WIN32)
#define DP104_FAILURE_CAPTURE_EXPORT __declspec(dllexport)
#else
#define DP104_FAILURE_CAPTURE_EXPORT __attribute__((visibility("default")))
#endif

extern "C"
{
extern DP104_FAILURE_CAPTURE_EXPORT dp104::openrgb::Dp104FailureCapture
    g_dp104FailureCapture;
}

#endif
