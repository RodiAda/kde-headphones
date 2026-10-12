#include "Registry.h"

#include <mdr-c/Headphones.h>

#include "AirPodsBackend.h"
#include "BoseBackend.h"
#include "GalaxyBackend.h"
#include "PixelBudsBackend.h"
#include "NothingBackend.h"
#include "SonyBackend.h"

namespace {

constexpr KnownService KnownServices[] = {
    {MDR_SERVICE_UUID_XM5, DeviceKind::SonyV2, nullptr, false},
    {MDR_SERVICE_UUID_LEGACY, DeviceKind::SonyV1, nullptr, false},
    {"2E73A4AD-332D-41FC-90E2-16BEF06523F2", DeviceKind::Galaxy, nullptr, false},
    {"00001102-0000-1000-8000-00805F9B34FD", DeviceKind::GalaxyLegacy, nullptr, false},
    {"25E97FF7-24CE-4C4C-8951-F764A708F7B5", DeviceKind::PixelBuds, nullptr, false},
    {"AEAC4A03-DFF5-498F-843A-34487CF133EB", DeviceKind::Nothing, nullptr, false},
    {"00001101-0000-1000-8000-00805F9B34FB", DeviceKind::Bose, "Bose", false},
    {"00000000-DECA-FADE-DECA-DEAFDECACAFF", DeviceKind::Bose, nullptr, false},
    {"00001101-0000-1000-8000-00805F9B34FB", DeviceKind::GalaxyClassic, "Buds", false},
    {"74EC2172-0BAD-4D01-8F77-997B2BE0722A", DeviceKind::AirPods, nullptr, true},
};

}

std::span<const KnownService> knownServices()
{
    return KnownServices;
}

std::unique_ptr<Backend> createBackend(MDRConnection *connection, const DeviceInfo &device)
{
    switch (device.kind) {
    case DeviceKind::SonyV2:
    case DeviceKind::SonyV1:
        return std::make_unique<SonyBackend>(connection, device);
    case DeviceKind::Galaxy:
    case DeviceKind::GalaxyClassic:
    case DeviceKind::GalaxyLegacy:
        return std::make_unique<GalaxyBackend>(connection, device);
    case DeviceKind::AirPods:
        return std::make_unique<AirPodsBackend>(connection, device);
    case DeviceKind::PixelBuds:
        return std::make_unique<PixelBudsBackend>(connection, device);
    case DeviceKind::Nothing:
        return std::make_unique<NothingBackend>(connection, device);
    case DeviceKind::Bose:
        return std::make_unique<BoseBackend>(connection, device);
    default:
        return nullptr;
    }
}
