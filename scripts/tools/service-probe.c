/* Report which launchd MachServices are actually registered.
 *
 * Runs as a system job from /System/Library/Extensions and prints one line per
 * service, so the serial console shows whether the ports SpringBoard talks to
 * during launch exist in the bootstrap namespace.
 */
#include <mach/mach.h>
#include <bootstrap.h>
#include <stdio.h>
#include <string.h>

static const char *kServices[] = {
    "com.apple.CARenderServer",
    "com.apple.backboard.display.services",
    "com.apple.backboard.hid-services.xpc",
    "com.apple.backboard.hid.services",
    "com.apple.backboard.system-app-server",
    "com.apple.backboardd.virtualframebuffer",
    "com.apple.backlightd",
    "com.apple.iohideventsystem",
    "com.apple.springboard.services",
    "com.apple.springboard.backgroundappservices",
    "com.apple.springboard.statusbarservices",
    "com.apple.frontboard.systemappservices",
    "com.apple.systemstatus",
    "com.apple.systemstatus.publisher",
    "com.apple.mobilegestalt.xpc",
    "com.apple.system.logger",
    "com.apple.cfprefsd.system",
    "com.apple.distributed_notifications.system",
    "com.apple.securityd",
    "com.apple.runningboardd",
    "com.apple.lsd.mapdb",
    "com.apple.mobile.keybagd",
    "com.apple.containermanagerd.system",
    "com.apple.bsd.dirhelper",
};

int main(void) {
    printf("svc-probe: start\n");
    fflush(stdout);
    for (unsigned i = 0; i < sizeof(kServices) / sizeof(kServices[0]); ++i) {
        mach_port_t port = MACH_PORT_NULL;
        kern_return_t kr = bootstrap_look_up(bootstrap_port, kServices[i], &port);
        if (kr == KERN_SUCCESS) {
            printf("svc-probe: %-48s ok port=0x%x\n", kServices[i], port);
            mach_port_deallocate(mach_task_self(), port);
        } else {
            printf("svc-probe: %-48s MISSING kr=0x%x\n", kServices[i], kr);
        }
        fflush(stdout);
    }
    printf("svc-probe: done\n");
    fflush(stdout);
    return 0;
}

