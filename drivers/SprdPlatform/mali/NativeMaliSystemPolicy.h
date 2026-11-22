// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <string.h>

namespace NativeMaliSystemPolicy {
// proc_selfname() exposes at most the kernel task-name width.  The real
// WidgetRenderer_D launch is observed as WidgetRenderer_ (the final D is
// truncated), so both the source name and its measured runtime spelling are
// listed.  Matching remains exact for each token.
static constexpr const char *GrantProcessList =
    "WidgetRenderer_,WidgetRenderer_D";

inline bool allowsProcess(const char *name)
{
    return name && (!strcmp(name, "WidgetRenderer_") ||
        !strcmp(name, "WidgetRenderer_D"));
}
}
