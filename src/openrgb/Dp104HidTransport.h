#ifndef DP104_HID_TRANSPORT_H
#define DP104_HID_TRANSPORT_H

#include <mutex>

#include <hidapi.h>

#include "dp104_host_client.h"

namespace dp104
{
namespace openrgb
{

/* Owns one already-opened HID interface. ITransport exposes only the 32-byte
 * protocol payload; this adapter adds/removes HID report ID zero. */
class Dp104HidTransport final : public host::ITransport
{
public:
    /* Serializes DP104 protocol activity across all handles in this process.
     * Hold one across a multi-report frame or discovery handshake. */
    class ProtocolSession final
    {
    public:
        ProtocolSession();

        ProtocolSession(const ProtocolSession&) = delete;
        ProtocolSession& operator=(const ProtocolSession&) = delete;

    private:
        std::unique_lock<std::recursive_mutex> lock_;
    };

    explicit Dp104HidTransport(hid_device* device);
    ~Dp104HidTransport() override;

    Dp104HidTransport(const Dp104HidTransport&) = delete;
    Dp104HidTransport& operator=(const Dp104HidTransport&) = delete;

    bool Exchange(const host::Report& request, host::Report& response) override;
    bool IsOpen() const;
    void Close();

private:
    static std::recursive_mutex& SharedProtocolMutex();

    mutable std::mutex mutex_;
    hid_device* device_;
};

} // namespace openrgb
} // namespace dp104

#endif
