/*
 * Copyright (C) 2014-2021 Firejail Authors
 *
 * This file is part of firejail project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#define _XOPEN_SOURCE 500
#include "firejail.h"
#include <ftw.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <syslog.h>
#include <errno.h>
#include <dirent.h>
#include <grp.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/wait.h>
#include <limits.h>

#include <fcntl.h>
#ifndef O_PATH
#define O_PATH 010000000
#endif

#include <sys/syscall.h>
#ifdef __NR_openat2
#include <linux/openat2.h>
#endif

#define MAX_GROUPS 1024
#define MAXBUF 4098
#define EMPTY_STRING ("")


// send the error to /var/log/auth.log and exit after a small delay
void errLogExit(char* fmt, ...) {
	va_list args;
	va_start(args,fmt);
	openlog("firejail", LOG_NDELAY | LOG_PID, LOG_AUTH);
	MountData *m = get_last_mount();

	char *msg1;
	char *msg2  = "Access error";
	if (vasprintf(&msg1, fmt, args) != -1 &&
	    asprintf(&msg2, "Access error: uid %d, last mount name:%s dir:%s type:%s - %s", getuid(), m->fsname, m->dir, m->fstype, msg1) != -1)
		syslog(LOG_CRIT, "%s", msg2);
	va_end(args);
	closelog();

	sleep(2);
	fprintf(stderr, "%s\n", msg2);
	exit(1);
}

static void clean_supplementary_groups(gid_t gid) {
	assert(cfg.username);
	gid_t groups[MAX_GROUPS];
	int ngroups = MAX_GROUPS;
	int rv = getgrouplist(cfg.username, gid, groups, &ngroups);
	if (rv == -1)
		goto clean_all;

	// clean supplementary group list
	// allow only firejail, tty, audio, video, games
	gid_t new_groups[MAX_GROUPS];
	int new_ngroups = 0;
	char *allowed[] = {
		"firejail",
		"tty",
		"audio",
		"video",
		"games",
		NULL
	};

	int i = 0;
	while (allowed[i]) {
		gid_t g = get_group_id(allowed[i]);
	 	if (g) {
			int j;
			for (j = 0; j < ngroups; j++) {
				if (g == groups[j]) {
					new_groups[new_ngroups] = g;
					new_ngroups++;
					break;
				}
			}
		}
		i++;
	}

	if (new_ngroups) {
		rv = setgroups(new_ngroups, new_groups);
		if (rv)
			goto clean_all;

		if (arg_debug) {
			printf("Supplementary groups: ");
			for (i = 0; i < new_ngroups; i++)
				printf("%d ", new_groups[i]);
			printf("\n");
		}
	}
	else
		goto clean_all;

	return;

clean_all:
	fwarning("cleaning all supplementary groups\n");
	if (setgroups(0, NULL) < 0)
		errExit("setgroups");
}


// drop privileges
// - for root group or if nogroups is set, supplementary groups are not configured
void drop_privs(int nogroups) {
	gid_t gid = getgid();
	if (arg_debug)
		printf("Drop privileges: pid %d, uid %d, gid %d, nogroups %d\n",  getpid(), getuid(), gid, nogroups);

	// configure supplementary groups
	EUID_ROOT();
	if (gid == 0 || nogroups) {
		if (setgroups(0, NULL) < 0)
			errExit("setgroups");
		if (arg_debug)
			printf("No supplementary groups\n");
	}
	else if (arg_noroot)
		clean_supplementary_groups(gid);

	// set uid/gid
	if (setresgid(-1, getgid(), getgid()) != 0)
		errExit("setresgid");
	if (setresuid(-1, getuid(), getuid()) != 0)
		errExit("setresuid");
}


int mkpath_as_root(const char* path) {
	assert(path && *path);

	// work on a copy of the path
	char *file_path = strdup(path);
	if (!file_path)
		errExit("strdup");

	char* p;
	int done = 0;
	for (p=strchr(file_path+1, '/'); p; p=strchr(p+1, '/')) {
		*p='\0';
		if (mkdir(file_path, 0755)==-1) {
			if (errno != EEXIST) {
				free(file_path);
				return -1;
			}
		}
		else {
			if (set_perms(file_path, 0, 0, 0755))
				errExit("set_perms");
			done = 1;
		}

		*p='/';
	}
	if (done)
		fs_logger2("mkpath", path);

	free(file_path);
	return 0;
}

void fwarning(char* fmt, ...) {
	if (arg_quiet)
		return;

	va_list args;
	va_start(args,fmt);
	fprintf(stderr, "Warning: ");
	vfprintf(stderr, fmt, args);
	va_end(args);
}

void fmessage(char* fmt, ...) { // TODO: this function is duplicated in src/fnet/interface.c
	if (arg_quiet)
		return;

	va_list args;
	va_start(args,fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fflush(0);
}

void logsignal(int s) {
	if (!arg_debug)
		return;

	openlog("firejail", LOG_NDELAY | LOG_PID, LOG_USER);
	syslog(LOG_INFO, "Signal %d caught", s);
	closelog();
}


void logmsg(const char *msg) {
	if (!arg_debug)
		return;

	openlog("firejail", LOG_NDELAY | LOG_PID, LOG_USER);
	syslog(LOG_INFO, "%s\n", msg);
	closelog();
}


void logargs(int argc, char **argv) {
	if (!arg_debug)
		return;

	int i;
	int len = 0;

	// calculate message length
	for (i = 0; i < argc; i++)
		len += strlen(argv[i]) + 1;	  // + ' '

	// build message
	char msg[len + 1];
	char *ptr = msg;
	for (i = 0; i < argc; i++) {
		sprintf(ptr, "%s ", argv[i]);
		ptr += strlen(ptr);
	}

	// log message
	logmsg(msg);
}


void logerr(const char *msg) {
	if (!arg_debug)
		return;

	openlog("firejail", LOG_NDELAY | LOG_PID, LOG_USER);
	syslog(LOG_ERR, "%s\n", msg);
	closelog();
}


void set_nice(int inc) {
	errno = 0;
	int rv = nice(inc);
	(void) rv;
	if (errno)
		fwarning("cannot set nice value\n");
}


static int copy_file_by_fd(int src, int dst) {
	assert(src >= 0);
	assert(dst >= 0);

	ssize_t len;
	static const int BUFLEN = 1024;
	unsigned char buf[BUFLEN];
	while ((len = read(src, buf, BUFLEN)) > 0) {
		int done = 0;
		while (done != len) {
			int rv = write(dst, buf + done, len - done);
			if (rv == -1)
				return -1;
			done += rv;
		}
	}
	if (len == 0)
		return 0;
	return -1;
}

// return -1 if error, 0 if no error; if destname already exists, return error
int copy_file(const char *srcname, const char *destname, uid_t uid, gid_t gid, mode_t mode) {
	assert(srcname);
	assert(destname);

	// open source
	int src = open(srcname, O_RDONLY|O_CLOEXEC);
	if (src < 0) {
		fwarning("cannot open source file %s, file not copied\n", srcname);
		return -1;
	}

	// open destination
	int dst = open(destname, O_CREAT|O_WRONLY|O_TRUNC|O_CLOEXEC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (dst < 0) {
		fwarning("cannot open destination file %s, file not copied\n", destname);
		close(src);
		return -1;
	}

	int errors = copy_file_by_fd(src, dst);
	if (!errors) {
		if (fchown(dst, uid, gid) == -1)
			errExit("fchown");
		if (fchmod(dst, mode) == -1)
			errExit("fchmod");
	}
	close(src);
	close(dst);
	return errors;
}

// return -1 if error, 0 if no error
void copy_file_as_user(const char *srcname, const char *destname, uid_t uid, gid_t gid, mode_t mode) {
	pid_t child = fork();
	if (child < 0)
		errExit("fork");
	if (child == 0) {
		// drop privileges
		drop_privs(0);

		// copy, set permissions and ownership
		int rv = copy_file(srcname, destname, uid, gid, mode); // already a regular user
		if (rv)
			fwarning("cannot copy %s\n", srcname);
#ifdef HAVE_GCOV
		__gcov_flush();
#endif
		_exit(0);
	}
	// wait for the child to finish
	waitpid(child, NULL, 0);
}

void copy_file_from_user_to_root(const char *srcname, const char *destname, uid_t uid, gid_t gid, mode_t mode) {
	// open destination
	int dst = open(destname, O_CREAT|O_WRONLY|O_TRUNC|O_CLOEXEC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (dst < 0) {
		fwarning("cannot open destination file %s, file not copied\n", destname);
		return;
	}

	pid_t child = fork();
	if (child < 0)
		errExit("fork");
	if (child == 0) {
		// drop privileges
		drop_privs(0);

		int src = open(srcname, O_RDONLY|O_CLOEXEC);
		if (src < 0) {
			fwarning("cannot open source file %s, file not copied\n", srcname);
		} else {
			if (copy_file_by_fd(src, dst)) {
				fwarning("cannot copy %s\n", srcname);
			}
			close(src);
		}
		close(dst);
#ifdef HAVE_GCOV
		__gcov_flush();
#endif
		_exit(0);
	}
	// wait for the child to finish
	waitpid(child, NULL, 0);
	if (fchown(dst, uid, gid) == -1)
		errExit("fchown");
	if (fchmod(dst, mode) == -1)
		errExit("fchmod");
	close(dst);
}

// return -1 if error, 0 if no error
void touch_file_as_user(const char *fname, mode_t mode) {
	pid_t child = fork();
	if (child < 0)
		errExit("fork");
	if (child == 0) {
		// drop privileges
		drop_privs(0);

		FILE *fp = fopen(fname, "wx");
		if (fp) {
			fprintf(fp, "\n");
			SET_PERMS_STREAM(fp, -1, -1, mode);
			fclose(fp);
		}
		else
			fwarning("cannot create %s\n", fname);
#ifdef HAVE_GCOV
		__gcov_flush();
#endif
		_exit(0);
	}
	// wait for the child to finish
	waitpid(child, NULL, 0);
}

// return 1 if the file is a directory
int is_dir(const char *fname) {
	assert(fname);
	if (*fname == '\0')
		return 0;

	// if fname doesn't end in '/', add one
	int rv;
	struct stat s;
	if (fname[strlen(fname) - 1] == '/')
		rv = stat(fname, &s);
	else {
		char *tmp;
		if (asprintf(&tmp, "%s/", fname) == -1) {
			fprintf(stderr, "Error: cannot allocate memory, %s:%d\n", __FILE__, __LINE__);
			errExit("asprintf");
		}
		rv = stat(tmp, &s);
		free(tmp);
	}

	if (rv == -1)
		return 0;

	if (S_ISDIR(s.st_mode))
		return 1;

	return 0;
}

// return 1 if the file is a link
int is_link(const char *fname) {
	assert(fname);
	if (*fname == '\0')
		return 0;

	// remove trailing slashes
	char *tmp = clean_pathname(fname);

	char c;
	ssize_t rv = readlink(tmp, &c, 1);
	free(tmp);

	return (rv != -1);
}

// remove all slashes and single dots from the end of a path
// for example /foo/bar///././. -> /foo/bar
void trim_trailing_slash_or_dot(char *path) {
	assert(path);

	char *end = strchr(path, '\0');
	if ((end - path) > 1) {
		end--;
		while (*end == '/' ||
		      (*end == '.' && *(end - 1) == '/')) {
			*end = '\0';
			end--;
			if (end == path)
				break;
		}
	}
}

// remove multiple spaces and return allocated memory
char *line_remove_spaces(const char *buf) {
	EUID_ASSERT();
	assert(buf);
	size_t len = strlen(buf);
	if (len == 0)
		return NULL;

	// allocate memory for the new string
	char *rv = malloc(len + 1);
	if (rv == NULL)
		errExit("malloc");

	// remove space at start of line
	const char *ptr1 = buf;
	while (*ptr1 == ' ' || *ptr1 == '\t')
		ptr1++;

	// copy data and remove additional spaces
	char *ptr2 = rv;
	int state = 0;
	while (*ptr1 != '\0') {
		if (*ptr1 == '\n' || *ptr1 == '\r')
			break;

		if (state == 0) {
			if (*ptr1 != ' ' && *ptr1 != '\t')
				*ptr2++ = *ptr1++;
			else {
				*ptr2++ = ' ';
				ptr1++;
				state = 1;
			}
		}
		else {				  // state == 1
			while (*ptr1 == ' ' || *ptr1 == '\t')
				ptr1++;
			state = 0;
		}
	}

	// strip last blank character if any
	if (ptr2 > rv && *(ptr2 - 1) == ' ')
		--ptr2;
	*ptr2 = '\0';
	//	if (arg_debug)
	//		printf("Processing line #%s#\n", rv);

	return rv;
}


char *split_comma(char *str) {
	EUID_ASSERT();
	if (str == NULL || *str == '\0')
		return NULL;
	char *ptr = strchr(str, ',');
	if (!ptr)
		return NULL;
	*ptr = '\0';
	ptr++;
	if (*ptr == '\0')
		return NULL;
	return ptr;
}


// simplify absolute path by removing
// 1) consecutive and trailing slashes, and
// 2) segments with a single dot
// for example /foo//./bar/ -> /foo/bar
char *clean_pathname(const char *path) {
	assert(path && path[0] == '/');

	size_t len = strlen(path);
	char *rv = malloc(len + 1);
	if (!rv)
		errExit("malloc");

	size_t i = 0;
	size_t j = 0;
	while (path[i]) {
		if (path[i] == '/') {
			while (path[i+1] == '/' ||
			      (path[i+1] == '.' && path[i+2] == '/'))
				i++;
		}

		rv[j++] = path[i++];
	}
	rv[j] = '\0';

	// remove a trailing dot
	if (j > 1 && rv[j - 1] == '.' && rv[j - 2] == '/')
		rv[--j] = '\0';

	// remove a trailing slash
	if (j > 1 && rv[j - 1] == '/')
		rv[--j] = '\0';

	return rv;
}


void check_unsigned(const char *str, const char *msg) {
	EUID_ASSERT();
	const char *ptr = str;
	while (*ptr != ' ' && *ptr != '\t' && *ptr != '\0') {
		if (!isdigit(*ptr)) {
			fprintf(stderr, "%s %s\n", msg, str);
			exit(1);
		}
		ptr++;
	}
}


#define BUFLEN 4096
// find the first child for this parent; return 1 if error
int find_child(pid_t parent, pid_t *child) {
	EUID_ASSERT();
	*child = 0;				  // use it to flag a found child

	DIR *dir;
	EUID_ROOT();				  // grsecurity fix
	if (!(dir = opendir("/proc"))) {
		// sleep 2 seconds and try again
		sleep(2);
		if (!(dir = opendir("/proc"))) {
			fprintf(stderr, "Error: cannot open /proc directory\n");
			exit(1);
		}
	}

	struct dirent *entry;
	char *end;
	while (*child == 0 && (entry = readdir(dir))) {
		pid_t pid = strtol(entry->d_name, &end, 10);
		if (end == entry->d_name || *end)
			continue;
		if (pid == parent)
			continue;

		// open stat file
		char *file;
		if (asprintf(&file, "/proc/%u/status", pid) == -1) {
			perror("asprintf");
			exit(1);
		}
		FILE *fp = fopen(file, "re");
		if (!fp) {
			free(file);
			continue;
		}

		// look for firejail executable name
		char buf[BUFLEN];
		while (fgets(buf, BUFLEN - 1, fp)) {
			if (strncmp(buf, "PPid:", 5) == 0) {
				char *ptr = buf + 5;
				while (*ptr != '\0' && (*ptr == ' ' || *ptr == '\t')) {
					ptr++;
				}
				if (*ptr == '\0') {
					fprintf(stderr, "Error: cannot read /proc file\n");
					exit(1);
				}
				if (parent == atoi(ptr)) {
					// we don't want /usr/bin/xdg-dbus-proxy!
					char *cmdline = pid_proc_cmdline(pid);
					if (cmdline) {
						if (strncmp(cmdline, XDG_DBUS_PROXY_PATH, strlen(XDG_DBUS_PROXY_PATH)) != 0)
							*child = pid;
						free(cmdline);
					}
				}
				break;		  // stop reading the file
			}
		}
		fclose(fp);
		free(file);
	}
	closedir(dir);
	EUID_USER();
	return (*child)? 0:1;			  // 0 = found, 1 = not found
}


void extract_command_name(int index, char **argv) {
	EUID_ASSERT();
	assert(argv);
	assert(argv[index]);

	// configure command index
	cfg.original_program_index = index;

	char *str = strdup(argv[index]);
	if (!str)
		errExit("strdup");

	// if we have a symbolic link, use the real path to extract the name
//	if (is_link(argv[index])) {
//		char*newname = realpath(argv[index], NULL);
//		if (newname) {
//			free(str);
//			str = newname;
//		}
//	}

	// configure command name
	cfg.command_name = str;
	if (!cfg.command_name)
		errExit("strdup");

	// remove the path: /usr/bin/firefox becomes firefox
	char *basename = cfg.command_name;
	char *ptr = strrchr(cfg.command_name, '/');
	if (ptr) {
		basename = ++ptr;
		if (*ptr == '\0') {
			fprintf(stderr, "Error: invalid command name\n");
			exit(1);
		}
	}
	else
		ptr = basename;

	// restrict the command name to the first word
	while (*ptr != ' ' && *ptr != '\t' && *ptr != '\0')
		ptr++;

	// command name is a substring of cfg.command_name
	if (basename != cfg.command_name || *ptr != '\0') {
		*ptr = '\0';

		basename = strdup(basename);
		if (!basename)
			errExit("strdup");

		free(cfg.command_name);
		cfg.command_name = basename;
	}
}


void update_map(char *mapping, char *map_file) {
	int fd;
	size_t j;
	size_t map_len;				  /* Length of 'mapping' */

	/* Replace commas in mapping string with newlines */

	map_len = strlen(mapping);
	for (j = 0; j < map_len; j++)
		if (mapping[j] == ',')
			mapping[j] = '\n';

	fd = open(map_file, O_RDWR|O_CLOEXEC);
	if (fd == -1) {
		fprintf(stderr, "Error: cannot open %s: %s\n", map_file, strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (write(fd, mapping, map_len) != (ssize_t)map_len) {
		fprintf(stderr, "Error: cannot write to %s: %s\n", map_file, strerror(errno));
		exit(EXIT_FAILURE);
	}

	close(fd);
}


void wait_for_other(int fd) {
	//****************************
	// wait for the parent to be initialized
	//****************************
	char childstr[BUFLEN + 1];
	int newfd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (newfd == -1)
		errExit("fcntl");
	FILE* stream;
	stream = fdopen(newfd, "r");
	*childstr = '\0';
	if (fgets(childstr, BUFLEN, stream)) {
		// remove \n)
		char *ptr = childstr;
		while(*ptr !='\0' && *ptr != '\n')
			ptr++;
		if (*ptr == '\0')
			errExit("fgets");
		*ptr = '\0';
	}
	else {
		fprintf(stderr, "Error: proc %d cannot sync with peer: %s\n",
			getpid(), ferror(stream) ? strerror(errno) : "unexpected EOF");

		int status = 0;
		pid_t pid = wait(&status);
		if (pid != -1) {
			if (WIFEXITED(status))
				fprintf(stderr, "Peer %d unexpectedly exited with status %d\n",
					pid, WEXITSTATUS(status));
			else if (WIFSIGNALED(status))
				fprintf(stderr, "Peer %d unexpectedly killed (%s)\n",
					pid, strsignal(WTERMSIG(status)));
			else
				fprintf(stderr, "Peer %d unexpectedly exited "
					"(un-decodable wait status %04x)\n", pid, status);
		}
		exit(1);
	}

	if (strcmp(childstr, "arg_noroot=0") == 0)
		arg_noroot = 0;
	else if (strcmp(childstr, "arg_noroot=1") == 0)
		arg_noroot = 1;
	else {
		fprintf(stderr, "Error: unexpected message from peer: %s\n", childstr);
		exit(1);
	}

	fclose(stream);
}

void notify_other(int fd) {
	FILE* stream;
	int newfd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (newfd == -1)
		errExit("fcntl");
	stream = fdopen(newfd, "w");
	fprintf(stream, "arg_noroot=%d\n", arg_noroot);
	fflush(stream);
	fclose(stream);
}

uid_t pid_get_uid(pid_t pid) {
	EUID_ASSERT();
	uid_t rv = 0;

	// open status file
	char *file;
	if (asprintf(&file, "/proc/%u/status", pid) == -1) {
		perror("asprintf");
		exit(1);
	}
	EUID_ROOT();				  // grsecurity fix
	FILE *fp = fopen(file, "re");
	if (!fp) {
		free(file);
		fprintf(stderr, "Error: cannot open /proc file\n");
		exit(1);
	}

	// extract uid
	static const int PIDS_BUFLEN = 1024;
	char buf[PIDS_BUFLEN];
	while (fgets(buf, PIDS_BUFLEN - 1, fp)) {
		if (strncmp(buf, "Uid:", 4) == 0) {
			char *ptr = buf + 4;
			while (*ptr != '\0' && (*ptr == ' ' || *ptr == '\t')) {
				ptr++;
			}
			if (*ptr == '\0') {
				fprintf(stderr, "Error: cannot read /proc file\n");
				exit(1);
			}

			rv = atoi(ptr);
			break;			  // break regardless!
		}
	}

	fclose(fp);
	free(file);
	EUID_USER();				  // grsecurity fix

	return rv;
}




uid_t get_group_id(const char *group) {
	// find tty group id
	gid_t gid = 0;
	struct group *g = getgrnam(group);
	if (g)
		gid = g->gr_gid;

	return gid;
}


static int remove_callback(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
	(void) sb;
	(void) typeflag;
	(void) ftwbuf;
	assert(fpath);

	if (strcmp(fpath, ".") == 0)
		return 0;

	if (remove(fpath)) {	// removes the link not the actual file
		perror("remove");
		fprintf(stderr, "Error: cannot remove file from user .firejail directory: %s\n", fpath);
		exit(1);
	}

	return 0;
}


int remove_overlay_directory(void) {
	EUID_ASSERT();
	struct stat s;
	sleep(1);

	char *path;
	if (asprintf(&path, "%s/.firejail", cfg.homedir) == -1)
		errExit("asprintf");

	if (lstat(path, &s) == 0) {
		// deal with obvious problems such as symlinks and root ownership
		if (!S_ISDIR(s.st_mode)) {
			if (S_ISLNK(s.st_mode))
				fprintf(stderr, "Error: %s is a symbolic link\n", path);
			else
				fprintf(stderr, "Error: %s is not a directory\n", path);
			exit(1);
		}
		if (s.st_uid != getuid()) {
			fprintf(stderr, "Error: %s is not owned by the current user\n", path);
			exit(1);
		}

		pid_t child = fork();
		if (child < 0)
			errExit("fork");
		if (child == 0) {
			// open ~/.firejail, fails if there is any symlink
			int fd = safer_openat(-1, path, O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
			if (fd == -1)
				errExit("safer_openat");
			// chdir to ~/.firejail
			if (fchdir(fd) == -1)
				errExit("fchdir");
			close(fd);

			EUID_ROOT();
			// FTW_PHYS - do not follow symbolic links
			if (nftw(".", remove_callback, 64, FTW_DEPTH | FTW_PHYS) == -1)
				errExit("nftw");

			EUID_USER();
			// remove ~/.firejail
			if (rmdir(path) == -1)
				errExit("rmdir");
#ifdef HAVE_GCOV
			__gcov_flush();
#endif
			_exit(0);
		}
		// wait for the child to finish
		waitpid(child, NULL, 0);
		// check if ~/.firejail was deleted
		if (stat(path, &s) == 0)
			return 1;
	}
	return 0;
}

// flush stdin if it is connected to a tty and has input
void flush_stdin(void) {
	if (!isatty(STDIN_FILENO))
		return;

	int cnt = 0;
	int rv = ioctl(STDIN_FILENO, FIONREAD, &cnt);
	if (rv != 0 || cnt == 0)
		return;

	fwarning("removing %d bytes from stdin\n", cnt);

	// If this process is backgrounded, below ioctl() will trigger
	// SIGTTOU and stop us. We avoid this by ignoring SIGTTOU for
	// the duration of the ioctl.
	sighandler_t hdlr = signal(SIGTTOU, SIG_IGN);
	rv = ioctl(STDIN_FILENO, TCFLSH, TCIFLUSH);
	signal(SIGTTOU, hdlr);

	if (rv)
		fwarning("Flushing stdin failed: %s\n", strerror(errno));
}

// return 1 if new directory was created, else return 0
int create_empty_dir_as_user(const char *dir, mode_t mode) {
	assert(dir);
	mode &= 07777;
	struct stat s;

	if (stat(dir, &s)) {
		if (arg_debug)
			printf("Creating empty %s directory\n", dir);
		pid_t child = fork();
		if (child < 0)
			errExit("fork");
		if (child == 0) {
			// drop privileges
			drop_privs(0);

			if (mkdir(dir, mode) == 0) {
				if (chmod(dir, mode) == -1)
					{;} // do nothing
			}
			else if (arg_debug)
				printf("Directory %s not created: %s\n", dir, strerror(errno));
#ifdef HAVE_GCOV
			__gcov_flush();
#endif
			_exit(0);
		}
		waitpid(child, NULL, 0);
		if (stat(dir, &s) == 0)
			return 1;
	}
	return 0;
}

void create_empty_dir_as_root(const char *dir, mode_t mode) {
	assert(dir);
	mode &= 07777;
	struct stat s;

	if (stat(dir, &s)) {
		if (arg_debug)
			printf("Creating empty %s directory\n", dir);
		/* coverity[toctou] */
		// don't fail if directory already exists. This can be the case in a race
		// condition, when two jails launch at the same time. See #1013
		if (mkdir(dir, mode) == -1 && errno != EEXIST)
			errExit("mkdir");
		if (set_perms(dir, 0, 0, mode))
			errExit("set_perms");
		ASSERT_PERMS(dir, 0, 0, mode);
	}
}

void create_empty_file_as_root(const char *fname, mode_t mode) {
	assert(fname);
	mode &= 07777;
	struct stat s;

	if (stat(fname, &s)) {
		if (arg_debug)
			printf("Creating empty %s file\n", fname);
		/* coverity[toctou] */
		// don't fail if file already exists. This can be the case in a race
		// condition, when two jails launch at the same time. Compare to #1013
		FILE *fp = fopen(fname, "we");
		if (!fp)
			errExit("fopen");
		SET_PERMS_STREAM(fp, 0, 0, mode);
		fclose(fp);
	}
}

// return 1 if error
int set_perms(const char *fname, uid_t uid, gid_t gid, mode_t mode) {
	assert(fname);
	if (chmod(fname, mode) == -1)
		return 1;
	if (chown(fname, uid, gid) == -1)
		return 1;
	return 0;
}

void mkdir_attr(const char *fname, mode_t mode, uid_t uid, gid_t gid) {
	assert(fname);
	mode &= 07777;
#if 0
	printf("fname %s, uid %d, gid %d, mode %x - ", fname, uid, gid, (unsigned) mode);
	if (S_ISLNK(mode))
		printf("l");
	else if (S_ISDIR(mode))
		printf("d");
	else if (S_ISCHR(mode))
		printf("c");
	else if (S_ISBLK(mode))
		printf("b");
	else if (S_ISSOCK(mode))
		printf("s");
	else
		printf("-");
	printf( (mode & S_IRUSR) ? "r" : "-");
	printf( (mode & S_IWUSR) ? "w" : "-");
	printf( (mode & S_IXUSR) ? "x" : "-");
	printf( (mode & S_IRGRP) ? "r" : "-");
	printf( (mode & S_IWGRP) ? "w" : "-");
	printf( (mode & S_IXGRP) ? "x" : "-");
	printf( (mode & S_IROTH) ? "r" : "-");
	printf( (mode & S_IWOTH) ? "w" : "-");
	printf( (mode & S_IXOTH) ? "x" : "-");
	printf("\n");
#endif
	if (mkdir(fname, mode) == -1 ||
	    chmod(fname, mode) == -1 ||
	    chown(fname, uid, gid)) {
	    	fprintf(stderr, "Error: failed to create %s directory\n", fname);
		errExit("mkdir/chmod");
	}

	ASSERT_PERMS(fname, uid, gid, mode);
}

unsigned extract_timeout(const char *str) {
	unsigned s;
	unsigned m;
	unsigned h;
	int rv = sscanf(str, "%02u:%02u:%02u", &h, &m, &s);
	if (rv != 3) {
		fprintf(stderr, "Error: invalid timeout, please use a hh:mm:ss format\n");
		exit(1);
	}
	unsigned timeout = h * 3600 + m * 60 + s;
	if (timeout == 0) {
		fprintf(stderr, "Error: invalid timeout\n");
		exit(1);
	}

	return timeout;
}

void disable_file_or_dir(const char *fname) {
	struct stat s;
	if (stat(fname, &s) != -1) {
		if (arg_debug)
			printf("blacklist %s\n", fname);
		if (is_dir(fname)) {
			if (mount(RUN_RO_DIR, fname, "none", MS_BIND, "mode=400,gid=0") < 0)
				errExit("disable directory");
		}
		else {
			if (mount(RUN_RO_FILE, fname, "none", MS_BIND, "mode=400,gid=0") < 0)
				errExit("disable file");
		}
		fs_logger2("blacklist", fname);
	}
}

void disable_file_path(const char *path, const char *file) {
	assert(file);
	assert(path);

	char *fname;
	if (asprintf(&fname, "%s/%s", path, file) == -1)
		errExit("asprintf");

	disable_file_or_dir(fname);
	free(fname);
}

// open an existing file without following any symbolic link
// relative paths are interpreted relative to dirfd
// ignore dirfd if path is absolute
// https://web.archive.org/web/20180419120236/https://blogs.gnome.org/jamesh/2018/04/19/secure-mounts
int safer_openat(int dirfd, const char *path, int flags) {
	assert(path && path[0]);
	flags |= O_NOFOLLOW;

	int fd = -1;

#ifdef __NR_openat2 // kernel 5.6 or better
	struct open_how oh;
	memset(&oh, 0, sizeof(oh));
	oh.flags = flags;
	oh.resolve = RESOLVE_NO_SYMLINKS;
	fd = syscall(__NR_openat2, dirfd, path, &oh, sizeof(struct open_how));
	if (fd != -1 || errno != ENOSYS)
		return fd;
#endif

	// openat2 syscall is not available, traverse path and
	// check each component if it is a symbolic link or not
	char *dup = strdup(path);
	if (!dup)
		errExit("strdup");
	char *tok = strtok(dup, "/");
	if (!tok) { // nothing to do, path is the root directory
		free(dup);
		return openat(dirfd, path, flags);
	}
	char *last_tok = EMPTY_STRING;

	int parentfd;
	if (path[0] == '/')
		parentfd = open("/", O_PATH|O_CLOEXEC);
	else
		parentfd = fcntl(dirfd, F_DUPFD_CLOEXEC, 0);
	if (parentfd == -1)
		errExit("open/fcntl");

	while (1) {
		// open path component, assuming it is a directory; this fails with ENOTDIR if it is a symbolic link
		// if token is a single dot, the directory referred to by parentfd is reopened
		fd = openat(parentfd, tok, O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
		if (fd == -1) {
			// if the following token is NULL, the current token is the final path component
			// try again to open it, this time using the passed flags, and return -1 or the descriptor
			last_tok = tok;
			tok = strtok(NULL, "/");
			if (!tok)
				fd = openat(parentfd, last_tok, flags);
			close(parentfd);
			free(dup);
			return fd;
		}
		// move on to next path component
		last_tok = tok;
		tok = strtok(NULL, "/");
		if (!tok)
			break;
		close(parentfd);
		parentfd = fd;
	}
	// getting here when the last path component exists and is of file type directory
	// reopen it using the passed flags
	close(fd);
	fd = openat(parentfd, last_tok, flags);
	close(parentfd);
	free(dup);
	return fd;
}

int has_handler(pid_t pid, int signal) {
	if (signal > 0 && signal <= SIGRTMAX) {
		char *fname;
		if (asprintf(&fname, "/proc/%d/status", pid) == -1)
			errExit("asprintf");
		EUID_ROOT();
		FILE *fp = fopen(fname, "re");
		EUID_USER();
		free(fname);
		if (fp) {
			char buf[BUFLEN];
			while (fgets(buf, BUFLEN, fp)) {
				if (strncmp(buf, "SigCgt:", 7) == 0) {
					unsigned long long val;
					if (sscanf(buf + 7, "%llx", &val) != 1) {
						fprintf(stderr, "Error: cannot read /proc file\n");
						exit(1);
					}
					val >>= (signal - 1);
					val &= 1ULL;
					fclose(fp);
					return val;  // 1 if process has a handler for the signal, else 0
				}
			}
			fclose(fp);
		}
	}
	return 0;
}

void enter_network_namespace(pid_t pid) {
	// in case the pid is that of a firejail process, use the pid of the first child process
	pid_t child = switch_to_child(pid);

	// exit if no permission to join the sandbox
	check_join_permission(child);

	// check network namespace
	char *name;
	if (asprintf(&name, "/run/firejail/network/%d-netmap", pid) == -1)
		errExit("asprintf");
	struct stat s;
	if (stat(name, &s) == -1) {
		fprintf(stderr, "Error: the sandbox doesn't use a new network namespace\n");
		exit(1);
	}

	// join the namespace
	EUID_ROOT();
	if (join_namespace(child, "net")) {
		fprintf(stderr, "Error: cannot join the network namespace\n");
		exit(1);
	}
}

// return 1 if error, 0 if a valid pid was found
static int extract_pid(const char *name, pid_t *pid) {
	int retval = 0;
	EUID_ASSERT();
	if (!name || strlen(name) == 0) {
		fprintf(stderr, "Error: invalid sandbox name\n");
		exit(1);
	}

	EUID_ROOT();
	if (name2pid(name, pid)) {
		retval = 1;
	}
	EUID_USER();
	return retval;
}

// return 1 if error, 0 if a valid pid was found
int read_pid(const char *name, pid_t *pid) {
	char *endptr;
	errno = 0;
	long int pidtmp = strtol(name, &endptr, 10);
	if ((errno == ERANGE && (pidtmp == LONG_MAX || pidtmp == LONG_MIN))
		|| (errno != 0 && pidtmp == 0)) {
		return extract_pid(name,pid);
	}
	// endptr points to '\0' char in name if the entire string is valid
	if (endptr == NULL || endptr[0]!='\0') {
		return extract_pid(name,pid);
	}
	*pid =(pid_t)pidtmp;
	return 0;
}

pid_t require_pid(const char *name) {
	pid_t pid;
	if (read_pid(name,&pid)) {
		fprintf(stderr, "Error: cannot find sandbox %s\n", name);
		exit(1);
	}
	return pid;
}

// return 1 if there is a link somewhere in path of directory
static int has_link(const char *dir) {
	assert(dir);
	int fd = safer_openat(-1, dir, O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
	if (fd != -1)
		close(fd);
	else if (errno == ELOOP || (errno == ENOTDIR && is_dir(dir)))
		return 1;
	return 0;
}

void check_homedir(const char *dir) {
	assert(dir);
	if (dir[0] != '/') {
		fprintf(stderr, "Error: invalid user directory \"%s\"\n", cfg.homedir);
		exit(1);
	}
	// symlinks are rejected in many places
	if (has_link(dir)) {
		fprintf(stderr, "No full support for symbolic links in path of user directory.\n"
			"Please provide resolved path in password database (/etc/passwd).\n\n");
	}
}
