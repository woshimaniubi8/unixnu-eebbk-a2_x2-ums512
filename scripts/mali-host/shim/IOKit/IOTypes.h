/* SPDX-License-Identifier: GPL-2.0-only */
/* Host stand-in for <IOKit/IOTypes.h>. It provides only the return type and the
 * return codes the driver code names; values are distinct small integers rather
 * than IOKit's real errno family, because host tests compare identities, not
 * numeric values. The kernel build uses the real header. The memory doubles stay
 * in tests/XnuMemoryFixture.h. */
#ifndef NATIVE_MALI_TEST_IOKIT_IOTYPES_H
#define NATIVE_MALI_TEST_IOKIT_IOTYPES_H
using IOReturn = int;
enum {
    kIOReturnSuccess = 0,
    kIOReturnBadArgument = 1,
    kIOReturnBusy = 2,
    kIOReturnNoResources = 3,
    kIOReturnNoMemory = 4,
    kIOReturnUnsupported = 5,
    kIOReturnNotReady = 6,
    kIOReturnIOError = 7,
    kIOReturnTimeout = 8
};
#endif
