// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Service::MM {

enum class Module : u32 {
    CPU = 0,
    GPU = 1,
    EMC = 2,
    SYS_BUS = 3,
    M_SELECT = 4,
    NVDEC = 5,
    NVENC = 6,
    NVJPG = 7,
    TEST = 8,
};

using Priority = u32;
using Setting = u32;

enum class EventClearMode : u32 {
    // TODO: Add specific clear mode values when documented
};

// Consolidate settings into a struct for better organization
struct Settings {
    Setting min{0};
    Setting max{0};
    Setting current{0};
    u32 id{1};  // Used by newer API versions
};

void LoopProcess(Core::System& system);

} // namespace Service::MM
