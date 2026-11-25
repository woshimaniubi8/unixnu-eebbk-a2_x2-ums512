/* Report which launchd MachServices are actually registered.
 *
 * Runs as a system job and writes straight to the platform serial tty, because
 * /dev/console on this port is not the UART the kernel console uses.
 */
#include <fcntl.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The iOS SDK does not ship the bootstrap header; declare the one call. */
extern kern_return_t bootstrap_look_up(mach_port_t bp, const char *service_name, mach_port_t *sp);

static int g_out = -1;

static void emit(const char *format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length <= 0) {
        return;
    }
    if (length > (int)sizeof(buffer) - 2) {
        length = (int)sizeof(buffer) - 2;
    }
    buffer[length] = '\n';
    write(g_out >= 0 ? g_out : STDOUT_FILENO, buffer, (size_t)length + 1);
}

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
    g_out = open("/dev/ttys0", O_WRONLY | O_NONBLOCK);
    if (g_out < 0) {
        g_out = open("/dev/console", O_WRONLY | O_NONBLOCK);
    }
    emit("svc-probe: start out=%d", g_out);

    mach_port_t bootstrap = MACH_PORT_NULL;
    if (task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &bootstrap) != KERN_SUCCESS) {
        emit("svc-probe: no bootstrap port");
        return 1;
    }
    emit("svc-probe: bootstrap=0x%x", bootstrap);

    for (unsigned i = 0; i < sizeof(kServices) / sizeof(kServices[0]); ++i) {
        emit("svc-probe: try %s", kServices[i]);
        mach_port_t port = MACH_PORT_NULL;
        kern_return_t kr = bootstrap_look_up(bootstrap, kServices[i], &port);
        if (kr == KERN_SUCCESS) {
            emit("svc-probe: %s ok port=0x%x", kServices[i], port);
            mach_port_deallocate(mach_task_self(), port);
        } else {
            emit("svc-probe: %s MISSING kr=0x%x", kServices[i], kr);
        }
    }
    emit("svc-probe: done");
    return 0;
}
