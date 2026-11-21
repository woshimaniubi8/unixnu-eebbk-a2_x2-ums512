// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <string.h>

namespace NativeMaliSystemPolicy {
static constexpr const char *GrantProcessList = "backboardd,SpringBoard,WidgetRenderer_D";

inline bool allowsProcess(const char *name)
{
    return name && (!strcmp(name, "backboardd") || !strcmp(name, "SpringBoard") ||
        !strcmp(name, "WidgetRenderer_D"));
}
}
