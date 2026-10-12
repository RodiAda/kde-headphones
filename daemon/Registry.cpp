#include "Registry.h"

#include <mdr-c/Headphones.h>

#include "GalaxyBackend.h"
#include "HuaweiBackend.h"
#include "SonyBackend.h"

namespace {

constexpr KnownService KnownServices[] = {
    {MDR_SERVICE_UUID_XM5, DeviceKind::SonyV2, nullptr, false},
    {MDR_SERVICE_UUID_LEGACY, DeviceKind::SonyV1, nullptr, false},
    {"2E73A4AD-332D-41FC-90E2-16BEF06523F2", DeviceKind::Galaxy, nullptr, false},
    {"00001102-0000-1000-8000-00805F9B34FD", DeviceKind::GalaxyLegacy, nullptr, false},
    {"00001101-0000-1000-8000-00805F9B34FB", DeviceKind::Huawei, "FreeBuds", false},
    {"00001101-0000-1000-8000-00805F9B34FB", DeviceKind::Huawei, "FreeLace", false},
    {"00001101-0000-1000-8000-00805F9B34FB", DeviceKind::Huawei, "FreeClip", false},
    {"00001101-0000-1000-8000-00805F9B34FB", DeviceKind::Huawei, "HONOR Earbuds", false},
    {"00001101-0000-1000-8000-00805F9B34FB", DeviceKind::GalaxyClassic, "Buds", false},
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
    case DeviceKind::Huawei:
        return std::make_unique<HuaweiBackend>(connection, device);
    default:
        return nullptr;
    }
}
