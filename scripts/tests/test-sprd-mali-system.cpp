#include <assert.h>
#include <string.h>
#include "../../drivers/SprdPlatform/mali/NativeMaliSystemPolicy.h"

int main()
{
    using NativeMaliSystemPolicy::allowsProcess;
    assert(allowsProcess("backboardd"));
    assert(allowsProcess("SpringBoard"));
    assert(allowsProcess("WidgetRenderer_"));
    assert(allowsProcess("WidgetRenderer_D"));
    assert(!allowsProcess("backboard"));
    assert(!allowsProcess("backboardd-helper"));
    assert(!allowsProcess("SpringBoardTest"));
    assert(!allowsProcess("launchd"));
    assert(!allowsProcess(""));
    assert(!allowsProcess(nullptr));
    assert(strcmp(NativeMaliSystemPolicy::GrantProcessList,
        "backboardd,SpringBoard,WidgetRenderer_,WidgetRenderer_D") == 0);
}
