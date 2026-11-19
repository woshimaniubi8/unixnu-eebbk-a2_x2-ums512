#if (DEBUG || DEVELOPMENT) && defined(XNU_OWNER_DATA_MOUNT) && XNU_OWNER_DATA_MOUNT
static const char native_mali_probe_path[] = "@PROBE_PATH@";

static int
native_mali_probe_verified_file(const uint8_t *bytes, size_t size, vfs_context_t ctx)
{
    vnode_t vp = NULLVP;
    int error = vnode_open(native_mali_probe_path, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx);
    if (error) {
        return error;
    }
    struct vnode_attr attr;
    VATTR_INIT(&attr);
    VATTR_WANTED(&attr, va_uid);
    error = vnode_getattr(vp, &attr, ctx);
    if (!error && (!VATTR_IS_SUPPORTED(&attr, va_uid) || attr.va_uid != 0 ||
        vnode_vtype(vp) != VREG || !native_gpu_addon_equal(vp, bytes, size, 0755, ctx))) {
        error = EIO;
    }
    const int closed = vnode_close(vp, FREAD, ctx);
    return error ? error : closed;
}

static int
native_mali_probe_stage(proc_t p, user_addr_t scratch)
{
@PROBE_PACKED@
@PROBE_SHA@
    const size_t bytes = @PROBE_BYTES@;
    uint8_t *buffer = kalloc_data(bytes, Z_WAITOK);
    if (!buffer) {
        return ENOMEM;
    }
    int error = native_gpu_addon_inflate(probe_packed, sizeof(probe_packed), buffer, bytes);
    if (!error) {
        SHA256_CTX hash;
        uint8_t digest[32];
        SHA256_Init(&hash);
        SHA256_Update(&hash, buffer, bytes);
        SHA256_Final(digest, &hash);
        if (memcmp(digest, probe_sha, sizeof(digest))) {
            error = EINVAL;
        }
    }
    if (!error) {
        vfs_context_t ctx = vfs_context_current();
        error = native_gpu_addon_parents(p, scratch, native_mali_probe_path);
        if (!error) {
            error = native_mali_probe_verified_file(buffer, bytes, ctx);
            if (error == ENOENT) {
                error = native_gpu_addon_write(native_mali_probe_path, buffer, bytes, 0755, ctx);
                if (!error || error == -1) {
                    error = native_mali_probe_verified_file(buffer, bytes, ctx);
                }
            }
        }
    }
    kfree_data(buffer, bytes);
    printf("M157 mali-user: staged bytes=%u err=%d\n", (unsigned)bytes, error);
    return error;
}

static int
native_mali_probe_spawn(proc_t p, user_addr_t scratch, mach_vm_size_t scratch_size, user_addr_t envp)
{
    /* Keep this data clear of the owner's existing boot env and mount records. */
    const size_t actions_size = PSF_ACTIONS_SIZE(2);
    if (!actions_size || proc_getpid(p) != 1 || !proc_is64bit(p) || !envp || !scratch ||
        scratch_size < 0x1000 || actions_size > scratch_size - 0x1000) {
        return EINVAL;
    }
    int error = native_mali_probe_stage(p, scratch);
    if (error) {
        return error;
    }

    const user_addr_t path = scratch + 0x100;
    const user_addr_t argv = scratch + 0x180;
    const user_addr_t desc = scratch + 0x1a0;
    const user_addr_t pid_addr = scratch + 0x280;
    const user_addr_t file_actions_addr = scratch + 0x1000;
    const user64_addr_t args[] = {path, USER_ADDR_NULL};
    const struct user__posix_spawn_args_desc description = {
        .file_actions_size = actions_size,
        .file_actions = file_actions_addr,
    };
    struct _posix_spawn_file_actions *actions = kalloc_data(actions_size, Z_WAITOK | Z_ZERO);
    if (!actions) {
        return ENOMEM;
    }
    actions->psfa_act_alloc = actions->psfa_act_count = 2;
    actions->psfa_act_acts[0].psfaa_type = PSFA_OPEN;
    actions->psfa_act_acts[0].psfaa_filedes = 1;
    actions->psfa_act_acts[0].psfaa_openargs.psfao_oflag = O_WRONLY;
    strlcpy(actions->psfa_act_acts[0].psfaa_openargs.psfao_path, "/dev/null", PATH_MAX);
    actions->psfa_act_acts[1].psfaa_type = PSFA_DUP2;
    actions->psfa_act_acts[1].psfaa_filedes = 1;
    actions->psfa_act_acts[1].psfaa_dup2args.psfad_newfiledes = 2;

    if (!(error = copyout(native_mali_probe_path, path, sizeof(native_mali_probe_path))) &&
        !(error = copyout(args, argv, sizeof(args))) &&
        !(error = copyout(&description, desc, sizeof(description)))) {
        error = copyout(actions, file_actions_addr, actions_size);
    }
    kfree_data(actions, actions_size);
    pid_t child = 0;
    if (!error) {
        error = copyout(&child, pid_addr, sizeof(child));
    }
    if (!error) {
        struct posix_spawn_args request = {
            .pid = pid_addr,
            .path = path,
            .adesc = desc,
            .argv = argv,
            .envp = envp,
        };
        int32_t result[2] = {};
#if defined(XNU_OWNER_CACHE_MOUNT) && XNU_OWNER_CACHE_MOUNT
        /* Preserve the one-shot cache premap for the original init exec. */
        const int pending = native_owner_cache_premap_pending;
        native_owner_cache_premap_pending = 0;
#endif
        error = posix_spawn(p, &request, result);
#if defined(XNU_OWNER_CACHE_MOUNT) && XNU_OWNER_CACHE_MOUNT
        native_owner_cache_premap_pending = pending;
#endif
        if (!error) {
            error = copyin(pid_addr, &child, sizeof(child));
            if (!error && child <= 1) {
                error = EIO;
            }
        }
    }
    printf("M157 mali-user: spawn pid=%d err=%d\n", child, error);
    return error;
}
#endif

/* @MALI_PROBE_BOOT_CALL@ */
#if (DEBUG || DEVELOPMENT) && defined(XNU_OWNER_DATA_MOUNT) && XNU_OWNER_DATA_MOUNT
	int mali_probe = 0;
	if (PE_parse_boot_argn("@BOOT_ARG@", &mali_probe, sizeof(mali_probe)) && mali_probe == 1) {
		const int probe_error = native_mali_probe_spawn(p, (user_addr_t)scratch_addr,
		    map_page_size, init_envp);
		/* Normal launchd still starts; it never waits on the diagnostic. */
		printf("M157 mali-user: continue normal init probe_err=%d\n", probe_error);
	}
#endif
