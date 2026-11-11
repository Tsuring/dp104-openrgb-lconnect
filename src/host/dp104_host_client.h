#ifndef DP104_HOST_CLIENT_H
#define DP104_HOST_CLIENT_H

#include <array>
#include <cstdint>
#include <mutex>

#include "dp104_protocol.h"

namespace dp104
{
namespace host
{

struct Color
{
    uint8_t r;
    uint8_t g;
    uint8_t b;

    Color() : r(0), g(0), b(0) {}
    Color(uint8_t red, uint8_t green, uint8_t blue)
        : r(red), g(green), b(blue) {}
};

typedef std::array<Color, DP104_LED_COUNT> Frame;
typedef std::array<uint8_t, DP104_REPORT_BYTES> Report;

/* Transport operates on the 32-byte HID payload. A platform adapter adds or
 * removes any HID report-ID byte around this interface. */
class ITransport
{
public:
    virtual ~ITransport() {}
    virtual bool Exchange(const Report& request, Report& response) = 0;
};

/* Transport-neutral client for the proposed D2 extension. Connect() gates all
 * color traffic on an exact, ready capability response. */
class Client
{
public:
    explicit Client(ITransport& transport, uint8_t firstTransaction = 1);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool Connect();
    bool IsReady() const;
    bool SetFrame(const Frame& frame);
    bool Release();
    bool Disconnect();

private:
    uint8_t NextTransaction();
    bool ExchangeAndValidate(uint8_t operation,
                             uint8_t transaction,
                             const Report& request,
                             Report& response);
    bool ValidateEmptyAck(const Report& response,
                          uint8_t operation,
                          uint8_t transaction) const;
    bool ValidateCaps(const Report& response, uint8_t transaction) const;
    bool ReleaseLocked();
    bool FailFrameLocked();

    ITransport& transport_;
    mutable std::mutex mutex_;
    uint8_t nextTransaction_;
    bool connected_;
    bool mayNeedRelease_;
    uint16_t leaseMs_;
};

} // namespace host
} // namespace dp104

#endif
