#include "dp104_host_client.h"

#include <chrono>
#include <cstring>

namespace dp104
{
namespace host
{
namespace
{
const uint8_t kProtocolMinor = 0;
const uint8_t kReadyFlag = 0x01;

bool IsZeroRange(const Report& report, unsigned first, unsigned last)
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

void SetHeader(Report& request, uint8_t operation, uint8_t transaction)
{
    request.fill(0);
    request[0] = DP104_COMMAND;
    request[1] = operation;
    request[2] = transaction;
    request[3] = 0;
}

} // namespace

Client::Client(ITransport& transport, uint8_t firstTransaction)
    : transport_(transport),
      nextTransaction_(firstTransaction == 0 ? 1 : firstTransaction),
      connected_(false),
      mayNeedRelease_(false),
      leaseMs_(0)
{
}

Client::~Client()
{
    Disconnect();
}

uint8_t Client::NextTransaction()
{
    const uint8_t transaction = nextTransaction_;
    nextTransaction_ = transaction == 0xffu ? 1u : static_cast<uint8_t>(transaction + 1u);
    if(nextTransaction_ == 0)
    {
        nextTransaction_ = 1;
    }
    return transaction;
}

bool Client::ExchangeAndValidate(uint8_t operation,
                                 uint8_t transaction,
                                 const Report& request,
                                 Report& response)
{
    if(!transport_.Exchange(request, response))
    {
        return false;
    }

    return response[0] == DP104_COMMAND &&
           response[1] == operation &&
           response[2] == transaction &&
           response[3] == DP104_OK;
}

bool Client::ValidateEmptyAck(const Report& response,
                              uint8_t operation,
                              uint8_t transaction) const
{
    return response[0] == DP104_COMMAND &&
           response[1] == operation &&
           response[2] == transaction &&
           response[3] == DP104_OK &&
           IsZeroRange(response, 4, DP104_REPORT_BYTES);
}

bool Client::ValidateCaps(const Report& response, uint8_t transaction) const
{
    static const char magic[] = DP104_CAPS_MAGIC;
    return response[0] == DP104_COMMAND &&
           response[1] == DP104_OP_CAPS &&
           response[2] == transaction &&
           response[3] == DP104_OK &&
           std::memcmp(&response[4], magic, sizeof(magic) - 1u) == 0 &&
           response[12] == DP104_PROTOCOL_MAJOR &&
           response[13] == kProtocolMinor &&
           dp104_read16(&response[14]) == DP104_LED_COUNT &&
           response[16] == DP104_CHUNK_LEDS &&
           response[17] == kReadyFlag &&
           dp104_read16(&response[18]) == DP104_LEASE_MS &&
           IsZeroRange(response, 20, DP104_REPORT_BYTES);
}

bool Client::Connect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(connected_)
    {
        return true;
    }

    /* A reconnect after an uncertain frame first attempts to relinquish the
     * previous session. Never claim readiness while direct mode may be held. */
    if(mayNeedRelease_ && !ReleaseLocked())
    {
        return false;
    }

    Report request;
    Report response;
    const uint8_t transaction = NextTransaction();
    SetHeader(request, DP104_OP_CAPS, transaction);

    if(!transport_.Exchange(request, response) || !ValidateCaps(response, transaction))
    {
        connected_ = false;
        leaseMs_ = 0;
        return false;
    }

    leaseMs_ = dp104_read16(&response[18]);
    connected_ = true;
    return true;
}

bool Client::IsReady() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

bool Client::SetFrame(const Frame& frame)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(!connected_)
    {
        return false;
    }

    uint8_t bytes[DP104_FRAME_BYTES];
    for(unsigned index = 0; index < DP104_LED_COUNT; ++index)
    {
        bytes[index * 3u] = frame[index].r;
        bytes[index * 3u + 1u] = frame[index].g;
        bytes[index * 3u + 2u] = frame[index].b;
    }
    const uint32_t crc = dp104_crc32(bytes, DP104_FRAME_BYTES);
    const uint8_t transaction = NextTransaction();
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    mayNeedRelease_ = true;

    Report request;
    Report response;
    SetHeader(request, DP104_OP_BEGIN, transaction);
    dp104_write16(&request[4], DP104_LED_COUNT);
    dp104_write16(&request[6], 0);
    dp104_write32(&request[8], crc);
    if(!ExchangeAndValidate(DP104_OP_BEGIN, transaction, request, response) ||
       !ValidateEmptyAck(response, DP104_OP_BEGIN, transaction))
    {
        return FailFrameLocked();
    }

    for(unsigned firstLed = 0; firstLed < DP104_LED_COUNT; firstLed += DP104_CHUNK_LEDS)
    {
        if(std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started).count() >= leaseMs_)
        {
            return FailFrameLocked();
        }

        SetHeader(request, DP104_OP_DATA, transaction);
        dp104_write16(&request[4], static_cast<uint16_t>(firstLed));
        request[6] = DP104_CHUNK_LEDS;
        for(unsigned offset = 0; offset < DP104_CHUNK_LEDS; ++offset)
        {
            const unsigned byteIndex = (firstLed + offset) * 3u;
            const unsigned reportIndex = 7u + offset * 3u;
            request[reportIndex] = bytes[byteIndex];
            request[reportIndex + 1u] = bytes[byteIndex + 1u];
            request[reportIndex + 2u] = bytes[byteIndex + 2u];
        }
        request[DP104_REPORT_BYTES - 1u] = 0;

        if(!ExchangeAndValidate(DP104_OP_DATA, transaction, request, response) ||
           !ValidateEmptyAck(response, DP104_OP_DATA, transaction))
        {
            return FailFrameLocked();
        }
    }

    if(std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::steady_clock::now() - started).count() >= leaseMs_)
    {
        return FailFrameLocked();
    }

    SetHeader(request, DP104_OP_COMMIT, transaction);
    if(!ExchangeAndValidate(DP104_OP_COMMIT, transaction, request, response) ||
       !ValidateEmptyAck(response, DP104_OP_COMMIT, transaction))
    {
        return FailFrameLocked();
    }

    return true;
}

bool Client::ReleaseLocked()
{
    Report request;
    Report response;
    const uint8_t transaction = NextTransaction();
    SetHeader(request, DP104_OP_RELEASE, transaction);
    if(!ExchangeAndValidate(DP104_OP_RELEASE, transaction, request, response) ||
       !ValidateEmptyAck(response, DP104_OP_RELEASE, transaction))
    {
        connected_ = false;
        leaseMs_ = 0;
        return false;
    }

    mayNeedRelease_ = false;
    return true;
}

bool Client::FailFrameLocked()
{
    /* Release also cancels an incomplete staged frame. It is best effort; a
     * failed acknowledgement closes the host-side readiness gate. */
    ReleaseLocked();
    connected_ = false;
    leaseMs_ = 0;
    return false;
}

bool Client::Release()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(!connected_ && !mayNeedRelease_)
    {
        return false;
    }

    return ReleaseLocked();
}

bool Client::Disconnect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    bool released = true;
    if(mayNeedRelease_)
    {
        released = ReleaseLocked();
    }
    connected_ = false;
    leaseMs_ = 0;
    return released;
}

} // namespace host
} // namespace dp104
