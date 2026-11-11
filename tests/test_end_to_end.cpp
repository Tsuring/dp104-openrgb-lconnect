#include "dp104_host_client.h"
#include "dp104_receiver.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do { if(!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while(0)

using dp104::host::Client;
using dp104::host::Frame;
using dp104::host::Report;

/* Real host encoder -> real portable C receiver. Only the physical link and
 * LED publication are simulated; these are not two copies of a mock protocol. */
class PlcLink final : public dp104::host::ITransport
{
public:
    dp104_receiver receiver{};
    std::array<uint8_t, DP104_FRAME_BYTES> visible{};
    uint32_t now = 0xfffffff0u;
    unsigned published = 0, released = 0, begins = 0;
    bool ready = true, offline = false, loseCommitAck = false;

    PlcLink()
    {
        dp104_output_hooks hooks = {this, Ready, Publish, Release};
        dp104_receiver_init(&receiver, hooks);
    }
    bool Exchange(const Report& request, Report& response) override
    {
        if(offline) return false;
        ++now;
        if(request[1] == DP104_OP_BEGIN) ++begins;
        dp104_receiver_handle(&receiver, request.data(), request.size(), response.data(), now);
        if(request[1] == DP104_OP_COMMIT && loseCommitAck)
        {
            loseCommitAck = false;
            return false; /* PLC committed, but host cannot know that. */
        }
        return true;
    }
    static int Ready(void *p) { return static_cast<PlcLink *>(p)->ready; }
    static int Publish(void *p, const uint8_t *rgb, unsigned leds)
    {
        auto& link = *static_cast<PlcLink *>(p);
        CHECK(leds == 104);
        std::memcpy(link.visible.data(), rgb, link.visible.size());
        ++link.published;
        return 1;
    }
    static void Release(void *p) { ++static_cast<PlcLink *>(p)->released; }
};

static Frame make_frame(unsigned seed)
{
    Frame frame;
    for(unsigned i = 0; i < frame.size(); ++i)
    {
        frame[i].r = static_cast<uint8_t>(i * 13u + seed);
        frame[i].g = static_cast<uint8_t>(i * 7u + seed * 3u);
        frame[i].b = static_cast<uint8_t>(i * 11u + seed * 5u);
    }
    return frame;
}
static void check_visible(const PlcLink& link, const Frame& frame)
{
    for(unsigned i = 0; i < frame.size(); ++i)
    {
        CHECK(link.visible[i * 3u] == frame[i].r);
        CHECK(link.visible[i * 3u + 1u] == frame[i].g);
        CHECK(link.visible[i * 3u + 2u] == frame[i].b);
    }
}

int main()
{
    PlcLink link;
    Client client(link);
    CHECK(client.Connect());
    CHECK(link.published == 0 && link.begins == 0);
    CHECK(client.SetFrame(make_frame(1)));
    check_visible(link, make_frame(1));
    CHECK(link.receiver.direct_active && link.published == 1);

    /* Includes transaction and uint32 timer rollover with distinct full frames. */
    for(unsigned frame = 2; frame < 270; ++frame)
    {
        CHECK(client.SetFrame(make_frame(frame)));
        check_visible(link, make_frame(frame));
    }
    CHECK(link.published == 269);

    link.loseCommitAck = true;
    CHECK(!client.SetFrame(make_frame(42)));
    CHECK(!client.IsReady());
    CHECK(link.released == 1 && !link.receiver.direct_active);
    CHECK(!client.SetFrame(make_frame(43)));
    CHECK(client.Connect());
    CHECK(client.SetFrame(make_frame(44)));

    link.offline = true;
    CHECK(!client.SetFrame(make_frame(45)));
    CHECK(link.receiver.direct_active); /* Host cannot send its release. */
    link.now += DP104_LEASE_MS;
    dp104_receiver_poll(&link.receiver, link.now);
    CHECK(!link.receiver.direct_active && link.released == 2);
    link.offline = false;
    CHECK(client.Connect()); /* Reconciles pending release before CAPS. */
    CHECK(client.SetFrame(make_frame(46)));
    CHECK(client.Disconnect());
    CHECK(link.released == 3 && !link.receiver.direct_active);

    PlcLink unbound;
    unbound.ready = false;
    Client unsupported(unbound);
    CHECK(!unsupported.Connect());
    CHECK(!unsupported.SetFrame(make_frame(47)));
    CHECK(unbound.begins == 0 && unbound.published == 0);
    std::puts("end-to-end: exact104RGB, transaction/timer rollover, lost ACK, disconnect lease and capability gate PASS");
    return 0;
}
