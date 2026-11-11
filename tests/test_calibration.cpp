#include "dp104_calibration.h"
#include "dp104_version.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do { if(!(x)) { std::fprintf(stderr, "FAIL line %d\n", __LINE__); std::exit(1); } } while(0)

struct Transport : dp104::host::ITransport
{
    unsigned calls = 0;
    int corruption = 0;
    bool Exchange(const dp104::host::Report& req, dp104::host::Report& out) override
    {
        ++calls;
        CHECK(req[0] == DP104_COMMAND && req[1] == 0xf1 && req[2] == 0xe1);
        CHECK(req[3] == 0 && req[4] == 2 && req[5] == 0 && req[6] == 1);
        CHECK(req[7] == 'U' && req[8] == 'S' && req[9] == 'E' && req[10] == 'R');
        unsigned nonzero = 0;
        for(unsigned i = 11; i < 27; ++i) nonzero |= req[i];
        CHECK(nonzero != 0);
        for(unsigned i = 27; i < req.size(); ++i) CHECK(req[i] == 0);
        out.fill(0);
        out[0] = req[0]; out[1] = req[1]; out[2] = req[2];
        for(unsigned i = 0; i < 3; ++i) out[4 + i] = req[4 + i];
        for(unsigned i = 0; i < 16; ++i) out[7 + i] = req[11 + i];
        if(corruption == 1) return false;
        if(corruption == 2) out[2] ^= 1;
        if(corruption == 3) out[3] = DP104_BUSY;
        if(corruption == 4) out[31] = 1;
        if(corruption == 5) out[0] = 0xff;
        if(corruption == 6) out[4] ^= 1;
        if(corruption == 7) out[7] ^= 1;
        if(corruption == 8)
        {
            out.fill(0);
            out[0] = req[0]; out[1] = req[1]; out[2] = req[2];
        }
        return true;
    }
};

struct VersionTransport : dp104::host::ITransport
{
    unsigned calls = 0;
    int corruption = 0;
    bool targetReady = false;
    dp104::host::Report lastRequest{};

    bool Exchange(const dp104::host::Report& req,
                  dp104::host::Report& out) override
    {
        ++calls;
        lastRequest = req;
        CHECK(req[0] == DP104_COMMAND && req[1] == APP_INFO_OP);
        CHECK(req[2] != 0 && req[3] == 0);
        unsigned nonzero = 0;
        for(unsigned i = 4; i < 20; ++i) nonzero |= req[i];
        CHECK(nonzero != 0);
        for(unsigned i = 20; i < req.size(); ++i) CHECK(req[i] == 0);

        out.fill(0);
        out[0] = DP104_COMMAND;
        out[1] = APP_INFO_OP;
        out[2] = req[2];
        out[3] = DP104_OK;
        std::memcpy(out.data() + 4, DP104_CAPS_MAGIC, 8);
        out[12] = COMPAT_VERSION_MAJOR;
        out[13] = COMPAT_VERSION_MINOR;
        out[14] = COMPAT_VERSION_PATCH;
        out[15] = targetReady ? 1u : 0u;
        std::memcpy(out.data() + 16, req.data() + 4, 16);

        switch(corruption)
        {
            case 1: return false;
            case 2: out[2] ^= 1u; break; /* Stale transaction. */
            case 3: out[31] ^= 1u; break; /* Mismatched nonce. */
            case 4: out[4] ^= 1u; break; /* Wrong magic. */
            case 5: out[3] = DP104_BUSY; break;
            case 6: out[15] |= 2u; break; /* Reserved flag. */
            case 7: out[0] = 0; break;
            case 8: out[1] = DP104_OP_CAPS; break;
            default: break;
        }
        return true;
    }
};

int main()
{
    using namespace dp104::host;
    Transport t;
    CHECK(!ApplyVerifiedColorOrder(t, ColorOrder{{0, 0, 1}}));
    CHECK(!ApplyVerifiedColorOrder(t, ColorOrder{{0, 1, 3}}));
    CHECK(!ApplyVerifiedColorOrder(t, ColorOrder{{2, 0, 1}}, 0));
    CHECK(t.calls == 0);
    for(int failure = 0; failure <= 8; ++failure)
    {
        t.corruption = failure;
        CHECK(ApplyVerifiedColorOrder(t, ColorOrder{{2, 0, 1}}) == (failure == 0));
    }

    VersionTransport versionTransport;
    ApplicationVersion version{0, 0, 0, false};
    CHECK(QueryApplicationVersion(versionTransport, version));
    CHECK(version.major == 1 && version.minor == 2 && version.patch == 1);
    CHECK(!version.ready);
    CHECK(versionTransport.lastRequest[1] == APP_INFO_OP &&
          versionTransport.lastRequest[2] == 0xe2);

    versionTransport.targetReady = true;
    CHECK(QueryApplicationVersion(versionTransport, version, 0xf2));
    CHECK(version.major == 1 && version.minor == 2 && version.patch == 1);
    CHECK(version.ready);

    const unsigned callsBeforeZeroTxn = versionTransport.calls;
    CHECK(!QueryApplicationVersion(versionTransport, version, 0));
    CHECK(versionTransport.calls == callsBeforeZeroTxn);
    for(int failure = 1; failure <= 8; ++failure)
    {
        versionTransport.corruption = failure;
        version = ApplicationVersion{9, 9, 9, true};
        CHECK(!QueryApplicationVersion(versionTransport, version, 0xf3));
        CHECK(version.major == 0 && version.minor == 0 &&
              version.patch == 0 && !version.ready);
    }
    return 0;
}
