/***
 Copyright (C) 2015 Intel Corporation

 Author: Auke-jan H. Kok <auke-jan.h.kok@intel.com>
 Author: Julio Montes <julio.montes@intel.com>

 This file is part of clr-cloud-init.

 clr-cloud-init is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 clr-cloud-init is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with clr-cloud-init. If not, see <http://www.gnu.org/licenses/>.

 In addition, as a special exception, the copyright holders give
 permission to link the code of portions of this program with the
 OpenSSL library under certain conditions as described in each
 individual source file, and distribute linked combinations
 including the two.
 You must obey the GNU General Public License in all respects
 for all of the code used other than OpenSSL.  If you modify
 file(s) with this exception, you may extend this exception to your
 version of the file(s), but you are not obligated to do so.  If you
 do not wish to do so, delete this exception statement from your
 version.  If you delete this exception statement from all source
 files in the program, then also delete it here.
***/

/*
 * lib.c - collection of misc functions for modules to do work
 */

#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <sys/sendfile.h>
#include <libgen.h>

#include <glib.h>

#include "debug.h"
#include "lib.h"

#define MOD "lib: "

#define SUDOERS_PATH SYSCONFDIR "/sudoers.d/"

void LOG(const char *fmt, ...) {
	va_list args;
	struct timespec now;

	va_start(args, fmt);
	clock_gettime(CLOCK_MONOTONIC, &now);
	fprintf(stderr, "[%f] ", ((double)now.tv_sec + ((double)now.tv_nsec / 1000000000.0)));
	vfprintf(stderr, fmt, args);

	va_end(args);
}

bool exec_task(const gchar* command_line) {
	gchar* standard_output = NULL;
	gchar* standard_error = NULL;
	GError* error = NULL;
	gint exit_status = 0;
	gboolean result;
	GString* command;
	command = g_string_new("");
	g_string_printf(command, SHELL_PATH " -c \"%s\"", (char*)command_line );

	LOG(MOD "Executing: %s\n", command->str);
	result = g_spawn_command_line_sync(command->str,
		&standard_output,
		&standard_error,
		&exit_status,
		&error);

	g_string_free(command, true);

	if (!result || exit_status != 0) {
		result = false;
		LOG(MOD "Command failed\n");
		if (error) {
			LOG(MOD "Error: %s\n", (char*)error->message);
		}
		if (standard_error) {
			LOG(MOD "STD Error: %s\n", (char*)standard_error);
		}
	}

	if (standard_output) {
		LOG(MOD "STD output: %s\n", (char*)standard_output);
		g_free(standard_output);
	}

	if (standard_error) {
		g_free(standard_error);
	}

	if (error) {
		g_error_free(error);
	}

	return result;
}

bool exec_task_async(const gchar* command_line, GChildWatchFunc async_func_watcher, gpointer data) {
	GPid pid = 0;
	GError *error = NULL;
	gchar* command = g_strdup(command_line);
	gchar* argvp[] = {SHELL_PATH, "-c", command, NULL };

	g_spawn_async(NULL, argvp, NULL,
			G_SPAWN_DO_NOT_REAP_CHILD,
			NULL, NULL, &pid, &error);

	if (error) {
		LOG(MOD "Error running async command: %s\n", (char*)error->message);
		g_error_free(error);
	}

	g_child_watch_add(pid, async_func_watcher, data);

	LOG(MOD "Executing [%d]: %s -c \"%s\"\n", pid, SHELL_PATH, command);

	g_free(command);

	return true;
}

int make_dir(const char* pathname, mode_t mode) {
	struct stat stats;
	if (stat(pathname, &stats) != 0) {
		if (g_mkdir_with_parents(pathname, (gint)mode) != 0) {
			LOG(MOD "Cannot create directory %s\n", pathname);
			return -1;
		}
	} else if (!S_ISDIR (stats.st_mode)) {
		LOG(MOD "%s already exists and is not a directory.\n",
			pathname);
		return -1;
	}
	return 0;
}

bool write_file(const GString* data, const gchar* file_path, int oflags, mode_t mode) {
	int fd;
	bool result = true;

	fd = open(file_path, oflags, mode);
	if (-1 == fd) {
		LOG(MOD "Cannot open %s\n", (char*)file_path);
		return false;
	}

	if (write(fd, data->str, data->len) == -1) {
		LOG(MOD "Cannot write in file '%s'", (char*)file_path);
		result = false;
	}

	if (fchmod(fd, mode) == -1) {
		LOG(MOD "Cannot change mode file '%s'", (char*)file_path);
		result = false;
	}

	if (close(fd) == -1) {
		LOG(MOD "Cannot close file '%s'", (char*)file_path);
		result = false;
	}

	return result;
}

int chown_path(const char* pathname, const char* ownername, const char* groupname) {
	uid_t owner_id;
	gid_t group_id;
	struct passwd *pw;
	struct group *grp;

	pw = getpwnam(ownername);
	owner_id = pw ? pw->pw_uid : (uid_t) - 1;
	grp = getgrnam(groupname);
	group_id = grp ? grp->gr_gid : (gid_t) - 1;

	return chown(pathname, owner_id, group_id);
}

bool write_sudo_directives(const GString* data, const gchar* filename) {
	gchar sudoers_file[PATH_MAX] = { 0 };
	g_strlcpy(sudoers_file, SUDOERS_PATH, PATH_MAX);
	if (make_dir(sudoers_file, S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH) != 0) {
		return false;
	}

	g_strlcat(sudoers_file, filename, PATH_MAX);

	return write_file(data, sudoers_file, O_CREAT|O_APPEND|O_WRONLY, S_IRUSR|S_IRGRP);
}

bool write_ssh_keys(const GString* data, const gchar* username) {
	gchar auth_keys_file[PATH_MAX];
	struct passwd *pw;

	pw = getpwnam(username);

	if (pw && pw->pw_dir) {
		g_snprintf(auth_keys_file, PATH_MAX, "%s/.ssh/", pw->pw_dir);

		if (make_dir(auth_keys_file, S_IRWXU) != 0) {
			LOG(MOD "Cannot create %s.\n", auth_keys_file);
			return false;
		}

		if (chown_path(auth_keys_file, username, username) != 0) {
			LOG(MOD "Cannot change the owner and group of %s.\n", auth_keys_file);
			return false;
		}

		g_strlcat(auth_keys_file, "authorized_keys", PATH_MAX);

		if (!write_file(data, auth_keys_file, O_CREAT|O_APPEND|O_WRONLY, S_IRUSR|S_IWUSR)) {
			return false;
		}

		if (chown_path(auth_keys_file, username, username) != 0) {
			LOG(MOD "Cannot change the owner and group of %s.\n", auth_keys_file);
			return false;
		}
	}

	return true;
}

bool copy_file(const gchar* src, const gchar* dest) {
	int fd_src = 0;
	int fd_dest = 0;
	struct stat st = { 0 };
	ssize_t send_result = 0;
	bool result = false;
	off_t bytes_copied = 0;
	gchar dest_dir[PATH_MAX] = { 0 };

	fd_src = open(src, O_RDONLY);
	if (-1 == fd_src) {
		LOG(MOD "Unable to open source file '%s'\n", src);
		return false;
	}

	if (fstat(fd_src, &st) == -1) {
		LOG(MOD "Unable to get info from file '%s'\n", src);
		goto fail1;
	}

	g_strlcpy(dest_dir, dest, PATH_MAX);
	if (make_dir(dirname(dest_dir), st.st_mode) != 0) {
		LOG(MOD "Unable to create directory '%s'\n", dest_dir);
		goto fail1;
	}

	fd_dest = open(dest, O_WRONLY|O_CREAT|O_TRUNC, st.st_mode);
	if (-1 == fd_dest) {
		LOG(MOD "Unable to open destination file '%s'\n", dest);
		goto fail1;
	}

	send_result = sendfile(fd_dest, fd_src, &bytes_copied, (size_t)st.st_size);
	if (-1 == send_result) {
		LOG(MOD "Unable to copy file from '%s' to '%s'\n", src, dest);
		goto fail2;
	}

	result = true;

fail2:
	close(fd_dest);
fail1:
	close(fd_src);
	return result;
}

bool gnode_free(GNode* node, __unused__ gpointer data) {
	if (node->data) {
		g_free(node->data);
	}
	return false;
}
