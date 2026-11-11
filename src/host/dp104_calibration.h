#ifndef DP104_CALIBRATION_H
#define DP104_CALIBRATION_H

#include "dp104_host_client.h"

namespace dp104 { namespace host {

/* Physical channels must be observed on the specific keyboard. No default is
 * selected. Applying this assertion changes volatile PLC state, not flash. */
typedef std::array<uint8_t, 3> ColorOrder;
struct ApplicationVersion
{
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
    bool ready;
};

bool IsColorOrder(const ColorOrder& order);
bool ApplyVerifiedColorOrder(ITransport& transport, const ColorOrder& order,
                            uint8_t transaction = 0xe1);
bool QueryApplicationVersion(ITransport& transport, ApplicationVersion& version,
                             uint8_t transaction = 0xe2);

} }
#endif
