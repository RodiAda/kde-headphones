#pragma once

#include <memory>
#include <span>

#include "Backend.h"

struct KnownService {
    const char *uuid;
    DeviceKind kind;
    const char *nameHint;
    bool ownTransport;
};

std::span<const KnownService> knownServices();
std::unique_ptr<Backend> createBackend(MDRConnection *connection, const DeviceInfo &device);
