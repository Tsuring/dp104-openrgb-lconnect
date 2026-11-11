#include "dp104_calibration.h"
#include "dp104_version.h"

#include <cstring>
#include <random>

namespace dp104 { namespace host {

namespace {

bool fill_fresh_nonce(uint8_t *nonce, unsigned length)
{
    try
    {
        std::random_device random;
        unsigned nonzero = 0;
        for(unsigned i = 0; i < length; ++i)
        {
            nonce[i] = static_cast<uint8_t>(random());
            nonzero |= nonce[i];
        }
        return nonzero != 0;
    }
    catch(...) { return false; }
}

} // namespace

bool IsColorOrder(const ColorOrder& order)
{
    unsigned seen = 0;
    for(uint8_t channel : order)
    {
        if(channel > 2 || (seen & (1u << channel))) return false;
        seen |= 1u << channel;
    }
    return seen == 7;
}

bool ApplyVerifiedColorOrder(ITransport& transport, const ColorOrder& order,
                            uint8_t transaction)
{
    if(!transaction || !IsColorOrder(order)) return false;
    Report request{}, response{};
    request[0] = DP104_COMMAND;
    request[1] = 0xf1;
    request[2] = transaction;
    for(unsigned i = 0; i < 3; ++i) request[4 + i] = order[i];
    request[7] = 'U'; request[8] = 'S'; request[9] = 'E'; request[10] = 'R';
    if(!fill_fresh_nonce(request.data() + 11, 16)) return false;
    if(!transport.Exchange(request, response)) return false;
    if(response[0] != DP104_COMMAND || response[1] != 0xf1 ||
       response[2] != transaction || response[3] != DP104_OK) return false;
    for(unsigned i = 0; i < 3; ++i)
        if(response[4 + i] != order[i]) return false;
    for(unsigned i = 0; i < 16; ++i)
        if(response[7 + i] != request[11 + i]) return false;
    for(unsigned i = 23; i < response.size(); ++i)
        if(response[i]) return false;
    return true;
}

bool QueryApplicationVersion(ITransport& transport, ApplicationVersion& version,
                             uint8_t transaction)
{
    version = ApplicationVersion{0, 0, 0, false};
    if(!transaction) return false;

    Report request{}, response{};
    request[0] = DP104_COMMAND;
    request[1] = APP_INFO_OP;
    request[2] = transaction;
    if(!fill_fresh_nonce(request.data() + 4, 16)) return false;
    if(!transport.Exchange(request, response)) return false;

    if(response[0] != DP104_COMMAND || response[1] != APP_INFO_OP ||
       response[2] != transaction || response[3] != DP104_OK ||
       std::memcmp(response.data() + 4, DP104_CAPS_MAGIC, 8) != 0 ||
       (response[15] & 0xfeu) != 0 ||
       std::memcmp(response.data() + 16, request.data() + 4, 16) != 0)
        return false;

    version.major = response[12];
    version.minor = response[13];
    version.patch = response[14];
    version.ready = (response[15] & 1u) != 0;
    return true;
}

} }
