/*
 * pam_usernet.
 * Copyright (C) 2016  Renzo Davoli, Eduard Caizer University of Bologna
 * Copyright (C) 2011-2017 The iproute2 Authors
 * Copyright (C) 2018  Daniel Gröber
 *
 * pam_usernet module
 *    provide each user with their own network
 *   (for users belonging to the "usernet" group)
 *
 * Cado is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <security/pam_appl.h>
#include <security/pam_modules.h>

#include <pam_net_checkgroup.h>

#define NETNS_RUN_DIR "/var/run/netns/"
#define NETNS_ETC_DIR "/etc/netns"

/**
 * init_log: log initialization with the given name
 */
void init_log(const char * log_name)
{
	setlogmask (LOG_UPTO (LOG_NOTICE));
	openlog (log_name, LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
}

/**
 * end_log: closes the log previously initialized
 */
void end_log()
{
	closelog ();
}

/**
 * bind_etc: Mount config files from /etc/netns/<name>/ into current namespace.
 */
int bind_etc(const char *name)
{
	int rv = 0;
	char *etc_netns_path = NULL;

	rv = asprintf(&etc_netns_path, "%s/%s", NETNS_ETC_DIR, name);
	if(rv == -1)
		goto alloc_fail;

	/* Make /etc a mount point, so we can apply a propagation policy to it
	 * below */
	rv = mount("/etc", "/etc", "none", MS_BIND, NULL);
	if (rv == -1) {
		syslog (LOG_ERR, "mount --bind %s %s: %s",
			etc_netns_path, etc_netns_path, strerror(errno));
		return -1;
	}

	/* Don't let bind mounts from /etc/netns/<name>/<file> -> /etc/<file>
	 * propagate back to the parent namespace */
	if (mount("", "/etc", "none", MS_PRIVATE, NULL)) {
		syslog (LOG_ERR, "\"mount --make-private /%s\" failed: %s\n",
			etc_netns_path, strerror(errno));
		return -1;
	}


	DIR *dir = opendir(etc_netns_path);
	if (!dir)
		return -1;

	struct dirent *entry = NULL;
	char *netns_name = NULL;
	char *etc_name = NULL;
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0)
			continue;
		if (strcmp(entry->d_name, "..") == 0)
			continue;

		rv = asprintf(&netns_name, "%s/%s", etc_netns_path, entry->d_name);
		if(rv == -1)
			goto free_dir;

		rv = asprintf(&etc_name, "/etc/%s", entry->d_name);
		if(rv == -1)
			goto free_netns_name;

		if (mount(netns_name, etc_name, "none", MS_BIND, NULL) < 0) {
			syslog (LOG_ERR, "Bind %s -> %s failed: %s\n",
				netns_name, etc_name, strerror(errno));
		}
	}

	rv = 0;

/*free_etc_name:*/
	free(etc_name);
free_netns_name:
	free(netns_name);
free_dir:
	closedir(dir);
/*free_etc_netns_path:*/
	free(etc_netns_path);

alloc_fail:
	if(rv == -1) {
		syslog (LOG_ERR, "allocating memory failed");
		return -1;
	} else {
		return 0;
	}
}

/**
 * remount_sys: Mount a version of /sys that describes the new network namespace
 */
int remount_sys(const char *name)
{
	unsigned long mountflags = 0;

	/* Temporarily make '/' private until we're done re-mounting /sys */
	if (mount("", "/", "none", MS_PRIVATE | MS_REC, NULL)) {
		syslog (LOG_ERR, "\"mount --make-rprivate /\" failed: %s\n",
			strerror(errno));
		return -1;
	}

	if (umount2("/sys", MNT_DETACH) < 0) {
		struct statvfs fsstat;

		/* If this fails, perhaps there wasn't a sysfs instance
		 * mounted. Good. */
		if (statvfs("/sys", &fsstat) == 0) {
			/* We couldn't umount the sysfs, we'll attempt to
			 * overlay it. A read-only instance can't be shadowed
			 * with a read-write one. */
			if (fsstat.f_flag & ST_RDONLY)
				mountflags = MS_RDONLY;
		}
	}

	if (mount(name, "/sys", "sysfs", mountflags, NULL) < 0) {
		syslog (LOG_ERR, "mount of /sys failed: %s\n", strerror(errno));
		return -1;
	}

	/* Make '/' shared again! */
	if (mount("", "/", "none", MS_SHARED | MS_REC, NULL)) {
		syslog (LOG_ERR, "\"mount --make-rshared /\" failed: %s\n",
			strerror(errno));
		return -1;
	}

	return 0;
}

/**
 * create_netns_rundir: Create /var/run/netns mount if it doesn't exist yet.
 */
int create_netns_rundir(void)
{
	int rv = 0;

	rv = mkdir(NETNS_RUN_DIR, S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH);
	if (rv == -1 && errno != EEXIST) {
		syslog (LOG_ERR, "cannot create netns dir %s: %s",
			NETNS_RUN_DIR, strerror(errno));
		return -1;
	}

	rv = mount("", NETNS_RUN_DIR, "none", MS_SHARED | MS_REC, NULL);
	if (rv == 0) {
		return 0;
	}

	if (errno != EINVAL) {
		syslog (LOG_ERR, "mount --make-shared %s: %s",
			NETNS_RUN_DIR, strerror(errno));
		return -1;
	}

	rv = mount(NETNS_RUN_DIR, NETNS_RUN_DIR, "none", MS_BIND, NULL);
	if (rv == -1) {
		syslog (LOG_ERR, "mount --bind %s: %s",
			NETNS_RUN_DIR, strerror(errno));
		return -1;
	}

	rv = mount("", NETNS_RUN_DIR, "none", MS_SHARED | MS_REC, NULL);
	if (rv == -1) {
		syslog (LOG_ERR, "mount --make-shared after bind %s: %s",
			NETNS_RUN_DIR, strerror(errno));
		return -1;
	}

	return 0;
}

/**
 * unshare_netns: Create new netns, including mounting the handle to ns_path.
 */
int unshare_netns(char *ns_path)
{
	int rv;
	int nsfd;

	nsfd = open(ns_path, O_RDONLY|O_CREAT|O_EXCL, 0);
	if (nsfd < 0) {
		syslog (LOG_ERR, "cannot create netns %s: %s",
			ns_path, strerror(errno));
		return -1;
	}

	close(nsfd);

	rv = unshare(CLONE_NEWNET);
	if (rv < 0) {
		syslog (LOG_ERR, "Failed to create a new netns %s: %s",
			ns_path, strerror(errno));
		return -1;
	}

	rv = mount("/proc/self/ns/net", ns_path, "none", MS_BIND, NULL);
	if (rv == -1) {
		syslog (LOG_ERR, "mount /proc/self/ns/net -> %s failed: %s",
			ns_path, strerror(errno));
		return -1;
	}

	return nsfd;
}

/**
 * enter_netns: Ensure we are in the netns referred to by ns_path, either by
 * creating it or entering it if it already exists.
 */
int enter_netns(char *ns_path)
{
	int nsfd;
	nsfd = open(ns_path, O_RDONLY);
	if (nsfd < 0) {
		if (errno == ENOENT) {
			unshare_netns(ns_path);
		} else {
			syslog (LOG_ERR, "netns open failed %s", ns_path);
			return -1;
		}
	} else {
		if (setns(nsfd, CLONE_NEWNET) != 0) {
			syslog (LOG_ERR, "cannot join netns %s: %s",
				ns_path, strerror(errno));
			close(nsfd);
			return -1;
		}
		close(nsfd);
	}

	return 0;
}

/*
 * PAM entry point for session creation
 */
int pam_sm_open_session(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
	const char *user;
	int rv;
	int isusernet;
	char *ns_path = NULL;

	init_log ("pam_usernet");
	if ((rv=pam_get_user(pamh, &user, NULL) != PAM_SUCCESS)) {
		syslog (LOG_ERR, "get user: %s", strerror(errno));
		end_log();
		return PAM_SUCCESS;
	}

	isusernet = checkgroup(user, "usernet");
	if(isusernet <= 0) {
		end_log();
		return PAM_IGNORE;
	}

	if (create_netns_rundir() == -1)
		goto close_log_and_abort;

	rv = asprintf(&ns_path, "%s/%s", NETNS_RUN_DIR, user);
	if(rv == -1)
		goto close_log_and_abort;

	rv = enter_netns(ns_path);
	if(rv == -1)
		goto close_log_and_abort;

	if (unshare(CLONE_NEWNS) < 0) {
		syslog (LOG_ERR, "unshare(mount) failed: %s\n", strerror(errno));
		goto close_log_and_abort;
	}

	if(remount_sys(user) == -1) {
		syslog (LOG_ERR, "remounting /sys failed");
		goto close_log_and_abort;
	}

	/* Setup bind mounts for config files in /etc */
	if(bind_etc(user) == -1) {
		syslog (LOG_ERR, "mounting /etc/netns/%s config files failed",
			user);
		goto close_log_and_abort;
	}

	end_log();
	return PAM_SUCCESS;

close_log_and_abort:
	if(ns_path)
		free(ns_path);

	end_log();
	return PAM_ABORT;
}

/*
 * PAM entry point for session cleanup
 */
int pam_sm_close_session(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
	return(PAM_IGNORE);
}
