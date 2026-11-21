// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <string.h>

namespace NativeMaliSystemPolicy {
static constexpr const char *GrantProcessList = "backboardd,SpringBoard";

inline bool allowsProcess(const char *name)
{
    return name && (!strcmp(name, "backboardd") || !strcmp(name, "SpringBoard"));
}
}
