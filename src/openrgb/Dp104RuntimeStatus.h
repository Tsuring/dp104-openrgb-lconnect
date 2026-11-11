#ifndef DP104_RUNTIME_STATUS_H
#define DP104_RUNTIME_STATUS_H

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dp104
{
namespace openrgb
{

enum Dp104RuntimeFailureReason : uint32_t
{
    DP104_RUNTIME_FAILURE_NONE = 0,
    DP104_RUNTIME_FAILURE_CLOSED = 1,
    DP104_RUNTIME_FAILURE_WRITE = 2,
    DP104_RUNTIME_FAILURE_READ = 3,
    DP104_RUNTIME_FAILURE_TIMEOUT = 4,
    DP104_RUNTIME_FAILURE_SHORT_READ = 5,
    DP104_RUNTIME_FAILURE_LATE = 6,
    DP104_RUNTIME_FAILURE_MISMATCH_TIMEOUT = 7,
    DP104_RUNTIME_FAILURE_DEVICE_STATUS = 8
};

enum Dp104RuntimeRecoveryState : uint32_t
{
    DP104_RUNTIME_RECOVERY_READY = 0,
    DP104_RUNTIME_RECOVERY_WAITING = 1,
    DP104_RUNTIME_RECOVERY_TERMINAL = 2
};

/* Fixed remote-readable ABI. Every member is one atomic 32-bit word; update
 * seqlocks let an external ReadProcessMemory reader detect partial snapshots. */
struct Dp104RuntimeStatus
{
    Dp104RuntimeStatus();

    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> schemaVersion;
    std::atomic<uint32_t> byteSize;
    std::atomic<uint32_t> exchangeCount;
    std::atomic<uint32_t> exchangeSuccesses;
    std::atomic<uint32_t> exchangeFailures;
    /* 0 = no failure, 1 = first-failure fields being written, 2 = complete. */
    std::atomic<uint32_t> firstFailureState;
    std::atomic<uint32_t> firstFailureOp;
    std::atomic<uint32_t> firstFailureTxn;
    std::atomic<uint32_t> firstFailureHeader0;
    std::atomic<uint32_t> firstFailureHeader1;
    std::atomic<uint32_t> firstFailureHeader2;
    std::atomic<uint32_t> firstFailureHeader3;
    std::atomic<uint32_t> firstFailureReason;
    std::atomic<uint32_t> firstFailureElapsedMs;
    std::atomic<uint32_t> firstFailureBytesRead;
    /* Odd while the last-failure fields are being written; even when stable. */
    std::atomic<uint32_t> lastFailureSequence;
    std::atomic<uint32_t> lastFailureOp;
    std::atomic<uint32_t> lastFailureTxn;
    std::atomic<uint32_t> lastFailureHeader0;
    std::atomic<uint32_t> lastFailureHeader1;
    std::atomic<uint32_t> lastFailureHeader2;
    std::atomic<uint32_t> lastFailureHeader3;
    std::atomic<uint32_t> lastFailureReason;
    std::atomic<uint32_t> lastFailureElapsedMs;
    std::atomic<uint32_t> lastFailureBytesRead;
    std::atomic<uint32_t> framesQueued;
    std::atomic<uint32_t> framesStarted;
    std::atomic<uint32_t> framesCommitted;
    std::atomic<uint32_t> framesFailed;
    std::atomic<uint32_t> recoveryAttempts;
    std::atomic<uint32_t> recoverySucceeded;
    std::atomic<uint32_t> recoveryFailures;
    std::atomic<uint32_t> recoveryState;
    std::atomic<uint32_t> protocolReady;
    std::atomic<uint32_t> reserved;
};

static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t),
              "DP104 status ABI requires 32-bit atomics");
static_assert(alignof(std::atomic<uint32_t>) == alignof(uint32_t),
              "DP104 status ABI requires 32-bit atomic alignment");
static_assert(offsetof(Dp104RuntimeStatus, magic) == 0u, "DP104 ABI magic offset changed");
static_assert(offsetof(Dp104RuntimeStatus, exchangeCount) == 12u,
              "DP104 ABI exchange offset changed");
static_assert(offsetof(Dp104RuntimeStatus, firstFailureState) == 24u,
              "DP104 ABI first-failure offset changed");
static_assert(offsetof(Dp104RuntimeStatus, firstFailureReason) == 52u,
              "DP104 ABI first-failure reason offset changed");
static_assert(offsetof(Dp104RuntimeStatus, lastFailureSequence) == 64u,
              "DP104 ABI last-failure sequence offset changed");
static_assert(offsetof(Dp104RuntimeStatus, lastFailureReason) == 92u,
              "DP104 ABI last-failure reason offset changed");
static_assert(offsetof(Dp104RuntimeStatus, framesQueued) == 104u,
              "DP104 ABI frame counter offset changed");
static_assert(offsetof(Dp104RuntimeStatus, recoveryAttempts) == 120u,
              "DP104 ABI recovery counter offset changed");
static_assert(offsetof(Dp104RuntimeStatus, protocolReady) == 136u,
              "DP104 ABI ready offset changed");
static_assert(offsetof(Dp104RuntimeStatus, reserved) == 140u,
              "DP104 ABI reserved offset changed");
static_assert(sizeof(Dp104RuntimeStatus) == 144u, "DP104 ABI size changed");

} // namespace openrgb
} // namespace dp104

#if defined(_WIN32)
#define DP104_RUNTIME_EXPORT __declspec(dllexport)
#else
#define DP104_RUNTIME_EXPORT __attribute__((visibility("default")))
#endif

extern "C"
{
extern DP104_RUNTIME_EXPORT dp104::openrgb::Dp104RuntimeStatus g_dp104RuntimeStatus;
}

#endif
