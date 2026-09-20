#include <sys/stat.h>
#include <sys/mount.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "launch.h"
#include "launch_priv.h"
#include "core.h"
#include "log.h"

static void
pd_launchd_boot_mkdir_p(const char *path, mode_t mode)
{
	if (mkdir(path, mode) < 0 && errno != EEXIST) {
		perror(path);
	}
}

static void
pd_launchd_boot_try_mount(const char *src, const char *target, int flags, void *data)
{
	pd_launchd_boot_mkdir_p(target, 0755);
	if (mount(src, target, flags, data) < 0 && errno != EBUSY) {
		fprintf(stderr, "mount %s on %s failed: ", src, target);
		perror("");
	}
}

/* iokit project: root is always mounted read-only by vfs_mountroot()
 * (bsd/vfs/vfs_subr.c hardcodes MNT_RDONLY|MNT_ROOTFS) -- userland PID 1
 * is expected to remount it read-write itself, the real-world equivalent
 * of `mount -uw /`. Without this every write anywhere on "/" fails EROFS.
 * __mac_mount() special-cases the root vnode and forces MNT_UPDATE
 * automatically for path "/", so flags=0 is sufficient, but hfs_mount()
 * (bsd/hfs/hfs_vfsops.c) asserts `data` is non-NULL whenever MNT_UPDATE
 * is set -- a zeroed struct is fine for a plain write-upgrade, those
 * fields only matter for a fresh/wrapper mount. Layout matches the
 * KERNEL-side struct hfs_mount_args (bsd/hfs/hfs_mount.h) field-for-field,
 * not the userspace-visible one -- same struct tools/init_binary/init.c's
 * own remount_root_rw() already uses and documents in more detail. */
struct pd_hfs_mount_args_stub {
	unsigned int hfs_uid;
	unsigned int hfs_gid;
	unsigned short hfs_mask;
	unsigned short _pad;
	unsigned int hfs_encoding;
	int tz_minuteswest;
	int tz_dsttime;
	int flags;
	int journal_tbuffer_size;
	int journal_flags;
	int journal_disable;
};

/* iokit project, DAR-219: report through launchd_syslog(... | LOG_CONSOLE),
 * NOT fprintf(stderr)/perror(). As PID 1 this runs before anything has
 * arranged a useful stderr, so a failing remount used to produce NO output
 * at all -- while its downstream consequence (pd_pid1_prepare_legacy_ipc()'s
 * mkdir("/var/tmp/") failing EROFS a few lines below) DID print, via
 * launchd_syslog|LOG_CONSOLE. A real-hardware boot log therefore showed the
 * symptom with its cause invisible, which is exactly the ambiguity DAR-219
 * had to be investigated to resolve. Log the success case too: silence is
 * not distinguishable from "never ran" (this project has been burned by a
 * diagnostic that shared its subject's failure mode before), and one line
 * per boot is cheap next to re-running a real-hardware test to find out. */
static void
pd_launchd_boot_remount_root_rw(void)
{
	struct pd_hfs_mount_args_stub args;
	memset(&args, 0, sizeof(args));
	if (mount("hfs", "/", 0, &args) < 0) {
		launchd_syslog(LOG_ERR | LOG_CONSOLE,
				"pd_launchd_boot: remount / read-write failed: %s "
				"(root stays read-only; expect EROFS from every write below)",
				strerror(errno));
	} else {
		launchd_syslog(LOG_NOTICE | LOG_CONSOLE,
				"pd_launchd_boot: remounted / read-write");
	}
}

/* iokit project, DAR-135 follow-up (2026-09-17): tools/init_binary/
 * inject_into_sd_image.sh populates /Library/LaunchDaemons (and everything
 * else on the image) via a plain, non-privileged host-side `cp` into an
 * hdiutil-attached HFS+ volume -- deliberately, so the whole
 * userland-staging build+inject+boot pipeline never needs an interactive
 * sudo password (see that script's own "no sudo needed for an
 * hdiutil-attached image owned by this user" comment). The real on-disk
 * owner of every file it writes is therefore whatever uid ran the script
 * on the host, not root -- confirmed 2026-09-17 by mounting a real image
 * and running `ls -ln`: /Library/LaunchDaemons/*.plist all showed uid 502
 * (the host build user), not 0.
 *
 * This is invisible to launchd's own pd_launchd_load_daemons_dir() (it
 * reads plists directly with no ownership gate, so boot-time static
 * loading already worked), but it is a real, reproduced blocker for the
 * DYNAMIC `launchctl load|unload <plist>` path: launchctl.c's own real,
 * unmodified path_goodness_check() -- `sb.st_uid != 0 && sb.st_uid !=
 * getuid()` -- rejects every one of these files with "Dubious ownership
 * on file (skipping)" when run as root, seen live in
 * qemu/dar253_boot3.log, qemu/dar253_boot5.log and
 * qemu/dar170_crash_probe.log ("launchctl unload
 * /Library/LaunchDaemons/com.apple.configd.plist" -> "nothing found to
 * unload"). That check is real Apple security policy and correct; the bug
 * is upstream of it, in how this port's own tooling populates the image.
 *
 * Real macOS never hits this because its install tooling runs privileged
 * and lays down system files already owned by root:wheel. This process
 * IS root by the time it reaches here (PID 1, and pd_launchd_boot_
 * remount_root_rw() has just made "/" writable), so it can fix the real
 * on-disk ownership itself with no host-side sudo involved -- that keeps
 * inject_into_sd_image.sh's no-sudo design intact while making the
 * on-disk state match what real Apple's packaging actually produces,
 * rather than changing launchctl.c's real, correct check. */
static void
pd_launchd_boot_chown_root(const char *path)
{
	if (chown(path, 0, 0) < 0 && errno != ENOENT) {
		launchd_syslog(LOG_ERR | LOG_CONSOLE,
				"pd_launchd_boot: chown(%s, root:wheel) failed: %s",
				path, strerror(errno));
	}
}

static void
pd_launchd_boot_fix_launchdaemons_ownership(void)
{
	static const char dir[] = "/Library/LaunchDaemons";
	DIR *d;
	struct dirent *de;
	unsigned int fixed = 0;

	pd_launchd_boot_chown_root("/Library");
	pd_launchd_boot_chown_root(dir);

	d = opendir(dir);
	if (d == NULL) {
		/* Not fatal -- e.g. a minimal image with no LaunchDaemons dir
		 * at all yet. pd_launchd_load_daemons_dir() below handles
		 * that case the same way. */
		return;
	}
	while ((de = readdir(d)) != NULL) {
		char path[1024];
		if (de->d_name[0] == '.') {
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		pd_launchd_boot_chown_root(path);
		fixed++;
	}
	closedir(d);

	launchd_syslog(LOG_NOTICE | LOG_CONSOLE,
			"pd_launchd_boot: normalized %s + %u plist(s) to root:wheel "
			"(DAR-135, dynamic launchctl load/unload ownership check)",
			dir, fixed);
}

/* /var/empty -- sshd's real privilege-separation chroot directory
 * (_PATH_PRIVSEP_CHROOT_DIR). Exactly the same root cause, and exactly
 * the same fix, as the LaunchDaemons ownership normalization above:
 * tools/init_binary/inject_into_sd_image.sh creates the directory
 * without sudo, so its real on-disk owner is the host build uid rather
 * than root, and real, unmodified upstream OpenSSH refuses to start --
 * sshd.c's privsep_preauth() -> misc.c safe_path() prints
 * "/var/empty must be owned by root and not group or world-writable."
 * and exits 255. Reproduced for real in
 * qemu/dar183_sshd_boot_test.log before this was added (DAR-183).
 *
 * That check is real OpenSSH security policy and correct -- the privsep
 * child chroots there, so a directory any non-root user can write to
 * would defeat the whole point. As with the plists, the bug is upstream
 * of it in how this port's tooling populates the image, and PID 1 is
 * already root with "/" writable by the time it gets here. Silent when
 * the directory does not exist (an image built without sshd installed);
 * launchd itself has no other relationship with sshd, which is
 * deliberately NOT a launchd job here (DAR-183). */
static void
pd_launchd_boot_fix_privsep_dir(void)
{
	static const char dir[] = "/var/empty";
	struct stat sb;

	if (stat(dir, &sb) < 0) {
		return;
	}
	pd_launchd_boot_chown_root(dir);
	if ((sb.st_mode & (S_IWGRP | S_IWOTH)) != 0 && chmod(dir, 0755) < 0) {
		launchd_syslog(LOG_ERR | LOG_CONSOLE,
				"pd_launchd_boot: chmod(%s, 0755) failed: %s",
				dir, strerror(errno));
		return;
	}
	launchd_syslog(LOG_NOTICE | LOG_CONSOLE,
			"pd_launchd_boot: normalized %s to root:wheel, mode 0755 "
			"(DAR-183, sshd privilege-separation chroot dir)", dir);
}

void
pd_launchd_boot(void)
{
	pd_launchd_boot_remount_root_rw();
	pd_launchd_boot_fix_launchdaemons_ownership();
	pd_launchd_boot_fix_privsep_dir();
	pd_launchd_boot_mkdir_p("/dev", 0755);
	pd_launchd_boot_try_mount("devfs", "/dev", 0, NULL);
}
