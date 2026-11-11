#include "dp104_master_bridge.h"
#include "dp104_receiver.h"
#include "dp104_wire.h"
#include "dp104_host_client.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do { if(!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while(0)

using dp104::host::Report;

class Link final : public dp104::host::ITransport
{
public:
    dp104_master_bridge bridge{};
    dp104_receiver receiver{};
    Report usb{};
    std::array<uint8_t, DP104_FRAME_BYTES> visible{};
    unsigned uartWrites = 0, usbWrites = 0, publishes = 0;
    unsigned criticalDepth = 0, criticalEnters = 0, criticalLeaves = 0;
    bool plcOnline = true, uartFailure = false, usbReady = false;
    bool usbBusy = false, supersedeDuringUsb = false;
    uint32_t clock = 0;

    Link()
    {
        dp104_bridge_hooks bridgeHooks = {this, Enter, Leave, SendPlc, SendUsb};
        dp104_output_hooks outputHooks = {this, Ready, Publish, Release};
        dp104_bridge_init(&bridge, bridgeHooks);
        dp104_receiver_init(&receiver, outputHooks);
    }
    bool Exchange(const Report& request, Report& response) override
    {
        usbReady = false;
        unsigned before = uartWrites;
        CHECK(dp104_bridge_on_host(&bridge, 0x1234, request.data(), request.size()) == 1);
        CHECK(uartWrites == before); /* IRQ-facing callback must not transmit. */
        dp104_bridge_pump(&bridge);
        dp104_bridge_pump(&bridge);
        if(!usbReady) return false;
        response = usb;
        return true;
    }
    static uint32_t Enter(void *p)
    {
        auto& l = *static_cast<Link *>(p);
        CHECK(l.criticalDepth == 0);
        ++l.criticalDepth; ++l.criticalEnters;
        return 0x51;
    }
    static void Leave(void *p, uint32_t saved)
    {
        auto& l = *static_cast<Link *>(p);
        CHECK(saved == 0x51 && l.criticalDepth == 1);
        --l.criticalDepth; ++l.criticalLeaves;
    }
    static int SendPlc(void *p, const uint8_t *data, unsigned length)
    {
        auto& l = *static_cast<Link *>(p);
        CHECK(l.criticalDepth == 0 && length == 32);
        ++l.uartWrites;
        if(l.uartFailure) return 0;
        if(l.plcOnline)
        {
            uint8_t wire[DP104_WIRE_BYTES];
            Report received{};
            Report response{};
            CHECK(dp104_wire_encode(DP104_WIRE_REQUEST, data, length, wire, sizeof(wire)));
            CHECK(dp104_wire_decode(DP104_WIRE_REQUEST, wire, dp104_wire_frame_size(wire, sizeof(wire)), received.data(), received.size()));
            dp104_receiver_handle(&l.receiver, received.data(), received.size(), response.data(), ++l.clock);
            CHECK(dp104_wire_encode(DP104_WIRE_RESPONSE, response.data(), response.size(), wire, sizeof(wire)));
            CHECK(dp104_wire_decode(DP104_WIRE_RESPONSE, wire, dp104_wire_frame_size(wire, sizeof(wire)), received.data(), received.size()));
            dp104_bridge_on_plc(&l.bridge, received.data(), received.size());
        }
        return 1;
    }
    static int SendUsb(void *p, uintptr_t port, const uint8_t *data, unsigned length)
    {
        auto& l = *static_cast<Link *>(p);
        CHECK(l.criticalDepth == 0 && port == 0x1234 && length == 32);
        if(l.supersedeDuringUsb)
        {
            l.supersedeDuringUsb = false;
            Report replacement{};
            replacement[0] = DP104_COMMAND;
            replacement[1] = DP104_OP_CAPS;
            replacement[2] = 0x77;
            CHECK(dp104_bridge_on_host(&l.bridge, port, replacement.data(), replacement.size()));
            return 0;
        }
        if(l.usbBusy) return 0;
        std::memcpy(l.usb.data(), data, length);
        ++l.usbWrites; l.usbReady = true;
        return 1;
    }
    static int Ready(void *) { return 1; }
    static int Publish(void *p, const uint8_t *data, unsigned leds)
    {
        auto& l = *static_cast<Link *>(p);
        CHECK(leds == 104);
        std::memcpy(l.visible.data(), data, l.visible.size()); ++l.publishes;
        return 1;
    }
    static void Release(void *) {}
};

static Report request(uint8_t operation, uint8_t transaction)
{
    Report result{}; result[0] = DP104_COMMAND; result[1] = operation; result[2] = transaction;
    return result;
}

int main()
{
    uint8_t buffer[DP104_WIRE_BYTES] = {DP104_COMMAND};
    uint8_t original[DP104_REPORT_BYTES] = {DP104_COMMAND};
    CHECK(dp104_wire_encode(1, buffer, 32, buffer, sizeof(buffer))); /* In-place. */
    CHECK(buffer[0] == DP104_WIRE_REQUEST && buffer[1] == DP104_WIRE_TYPE &&
          buffer[2] == DP104_WIRE_ENCODED_BYTES);
    const size_t encodedSize = dp104_wire_frame_size(buffer, sizeof(buffer));
    CHECK(encodedSize == DP104_WIRE_BYTES);
    CHECK(!dp104_wire_decode(0, buffer, encodedSize, buffer, sizeof(buffer)));
    CHECK(!dp104_wire_decode(1, buffer, 34, buffer, sizeof(buffer)));
    CHECK(!dp104_wire_decode(1, buffer, encodedSize, buffer, 31));
    CHECK(dp104_wire_decode(1, buffer, encodedSize, buffer, sizeof(buffer)));
    CHECK(std::memcmp(buffer, original, sizeof(original)) == 0);
    Link link;
    dp104::host::Client client(link);
    CHECK(client.Connect());
    dp104::host::Frame frame;
    for(unsigned i = 0; i < frame.size(); ++i)
        frame[i] = dp104::host::Color(static_cast<uint8_t>(i),
                                      static_cast<uint8_t>(255 - i), 0x71);
    CHECK(client.SetFrame(frame));
    CHECK(link.publishes == 1);
    for(unsigned i = 0; i < frame.size(); ++i)
    {
        CHECK(link.visible[i * 3] == frame[i].r);
        CHECK(link.visible[i * 3 + 1] == frame[i].g);
        CHECK(link.visible[i * 3 + 2] == frame[i].b);
    }
    CHECK(client.Disconnect());

    Link missing;
    missing.plcOnline = false;
    dp104::host::Client unsupported(missing);
    CHECK(!unsupported.Connect());
    CHECK(missing.uartWrites == 1 && missing.usbWrites == 0);
    CHECK(!unsupported.SetFrame(frame)); /* No invented local capability. */

    Link busyUsb;
    busyUsb.usbBusy = true;
    Report pending = request(DP104_OP_CAPS, 0x66);
    CHECK(dp104_bridge_on_host(&busyUsb.bridge, 0x1234, pending.data(), pending.size()));
    dp104_bridge_pump(&busyUsb.bridge); /* Real PLC reply generated. */
    dp104_bridge_pump(&busyUsb.bridge); /* USB busy: retain reply. */
    CHECK(busyUsb.bridge.response_ready && busyUsb.usbWrites == 0);
    dp104_bridge_pump(&busyUsb.bridge);
    CHECK(busyUsb.bridge.response_ready && busyUsb.usbWrites == 0);
    busyUsb.usbBusy = false;
    dp104_bridge_pump(&busyUsb.bridge);
    CHECK(!busyUsb.bridge.response_ready && busyUsb.usbWrites == 1 && busyUsb.usb[2] == 0x66);

    Link superseded;
    CHECK(dp104_bridge_on_host(&superseded.bridge, 0x1234, pending.data(), pending.size()));
    dp104_bridge_pump(&superseded.bridge);
    superseded.supersedeDuringUsb = true;
    dp104_bridge_pump(&superseded.bridge);
    CHECK(superseded.bridge.request_ready && !superseded.bridge.response_ready);
    CHECK(superseded.usbWrites == 0); /* Failed old submission must not restore a stale reply. */
    dp104_bridge_pump(&superseded.bridge);
    dp104_bridge_pump(&superseded.bridge);
    CHECK(superseded.usbWrites == 1 && superseded.usb[2] == 0x77);

    Link late;
    late.plcOnline = false;
    Report a = request(DP104_OP_CAPS, 0x41);
    CHECK(dp104_bridge_on_host(&late.bridge, 0x1234, a.data(), a.size()) == 1);
    dp104_bridge_pump(&late.bridge);
    Report b = request(DP104_OP_CAPS, 0x42);
    CHECK(dp104_bridge_on_host(&late.bridge, 0x1234, b.data(), b.size()) == 1);
    dp104_bridge_pump(&late.bridge);
    dp104_bridge_on_plc(&late.bridge, a.data(), a.size());
    dp104_bridge_pump(&late.bridge);
    CHECK(late.usbWrites == 0); /* Stale transaction cannot become new ACK. */
    dp104_bridge_on_plc(&late.bridge, b.data(), b.size());
    dp104_bridge_pump(&late.bridge);
    CHECK(late.usbWrites == 1 && late.usb[2] == 0x42);
    late.uartFailure = true;
    CHECK(dp104_bridge_on_host(&late.bridge, 0x1234, b.data(), b.size()) == 1);
    dp104_bridge_pump(&late.bridge);
    CHECK(!late.bridge.waiting);

    Report legacy{}; legacy[0] = 1;
    CHECK(dp104_bridge_on_host(&late.bridge, 0x1234, legacy.data(), legacy.size()) == 0);
    CHECK(dp104_bridge_on_host(&late.bridge, 0x1234, b.data(), 31) == 1);
    CHECK(link.criticalDepth == 0 && link.criticalEnters == link.criticalLeaves);
    CHECK(late.criticalDepth == 0 && late.criticalEnters == late.criticalLeaves);
    std::puts("master bridge: deferred IO, real PLC acknowledgements, full104RGB and stale reply rejection PASS");
    return 0;
}
