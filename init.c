/* SPDX-License-Identifier: GPL-2.0 */
/*
 * init.c - a small shell that runs as /init on SoC2 (LA32R Linux, glibc)
 *
 * As PID 1 it mounts /proc, /sys and /dev, opens the serial console as the
 * controlling terminal and then runs an interactive shell. It reaps every
 * child, including orphans re-parented to init.
 *
 * Shell features:
 *   - built-in commands (type "help")
 *   - external programs, searched in $PATH
 *   - pipelines:      cmd1 | cmd2 | cmd3
 *   - redirection:    cmd < in > out,  cmd >> out
 *   - background:     cmd &
 *   - quoting:        '...', "...", backslash
 *   - variables:      $NAME, $? (exit status of the last command)
 *
 * The console is in canonical mode, so line editing (echo, backspace,
 * Ctrl-U) is done by the kernel TTY layer. Ctrl-C stops the foreground
 * program; the shell itself ignores it.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LINE_MAX_LEN	1024
#define WORD_BUF_LEN	4096
#define MAX_TOKENS	64
#define MAX_ARGS	32
#define MAX_CMDS	8

#define CONSOLE_TTY	"/dev/ttyS0"
#define DEFAULT_PATH	"/bin:/sbin:/usr/bin:/usr/sbin"

extern char **environ;

static int is_init;
static int last_status;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int write_all(int fd, const char *buf, size_t len)
{
	while (len) {
		ssize_t n = write(fd, buf, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		buf += n;
		len -= n;
	}
	return 0;
}

static int copy_fd(int in, int out)
{
	char buf[1024];
	ssize_t n;

	while ((n = read(in, buf, sizeof(buf))) != 0) {
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (write_all(out, buf, n) < 0)
			return -1;
	}
	return 0;
}

static int cat_file(const char *path)
{
	int fd = open(path, O_RDONLY);
	int ret;

	if (fd < 0) {
		perror(path);
		return 1;
	}
	fflush(stdout);
	ret = copy_fd(fd, 1);
	if (ret < 0)
		perror(path);
	close(fd);
	return ret < 0;
}

/* ------------------------------------------------------------------ */
/* Built-in commands                                                   */
/* ------------------------------------------------------------------ */

static int cmd_help(int argc, char **argv);

static int cmd_echo(int argc, char **argv)
{
	int i = 1, newline = 1;

	if (i < argc && strcmp(argv[i], "-n") == 0) {
		newline = 0;
		i++;
	}
	for (; i < argc; i++)
		printf("%s%s", argv[i], i < argc - 1 ? " " : "");
	if (newline)
		putchar('\n');
	return 0;
}

static int cmd_cd(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : getenv("HOME");

	if (!dir)
		dir = "/";
	if (chdir(dir) < 0) {
		perror(dir);
		return 1;
	}
	return 0;
}

static int cmd_pwd(int argc, char **argv)
{
	char buf[PATH_MAX];

	if (!getcwd(buf, sizeof(buf))) {
		perror("pwd");
		return 1;
	}
	printf("%s\n", buf);
	return 0;
}

static void mode_str(mode_t m, char *s)
{
	static const char rwx[] = "rwxrwxrwx";

	s[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : S_ISCHR(m) ? 'c' :
	       S_ISBLK(m) ? 'b' : S_ISFIFO(m) ? 'p' : S_ISSOCK(m) ? 's' : '-';
	for (int i = 0; i < 9; i++)
		s[i + 1] = (m & (0400 >> i)) ? rwx[i] : '-';
	s[10] = '\0';
}

static void ls_long(const char *path, const char *name)
{
	struct stat st;
	char mode[11];

	if (lstat(path, &st) < 0) {
		perror(path);
		return;
	}
	mode_str(st.st_mode, mode);
	if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode))
		printf("%s %3lu %4u, %4u %s", mode, (unsigned long)st.st_nlink,
		       major(st.st_rdev), minor(st.st_rdev), name);
	else
		printf("%s %3lu %10lld %s", mode, (unsigned long)st.st_nlink,
		       (long long)st.st_size, name);

	if (S_ISLNK(st.st_mode)) {
		char target[PATH_MAX];
		ssize_t n = readlink(path, target, sizeof(target) - 1);

		if (n >= 0) {
			target[n] = '\0';
			printf(" -> %s", target);
		}
	}
	putchar('\n');
}

static int not_dot(const struct dirent *d)
{
	return strcmp(d->d_name, ".") && strcmp(d->d_name, "..");
}

static int ls_path(const char *path, int lng)
{
	struct dirent **ents;
	struct stat st;
	size_t width = 0;
	int n, cols;

	if (lstat(path, &st) < 0) {
		perror(path);
		return 1;
	}
	if (!S_ISDIR(st.st_mode)) {
		if (lng)
			ls_long(path, path);
		else
			printf("%s\n", path);
		return 0;
	}

	n = scandir(path, &ents, not_dot, alphasort);
	if (n < 0) {
		perror(path);
		return 1;
	}

	for (int i = 0; i < n; i++) {
		size_t len = strlen(ents[i]->d_name) + (ents[i]->d_type == DT_DIR);

		if (len > width)
			width = len;
	}
	cols = 80 / (int)(width + 2);
	if (cols < 1)
		cols = 1;

	for (int i = 0; i < n; i++) {
		const char *name = ents[i]->d_name;

		if (lng) {
			char full[PATH_MAX];

			snprintf(full, sizeof(full), "%s/%s", path, name);
			ls_long(full, name);
		} else {
			int is_dir = ents[i]->d_type == DT_DIR;
			int len = printf("%s%s", name, is_dir ? "/" : "");

			if ((i + 1) % cols == 0 || i == n - 1)
				putchar('\n');
			else
				printf("%*s", (int)(width + 2) - len, "");
		}
		free(ents[i]);
	}
	free(ents);
	return 0;
}

static int cmd_ls(int argc, char **argv)
{
	int i = 1, lng = 0, ret = 0;

	if (i < argc && strcmp(argv[i], "-l") == 0) {
		lng = 1;
		i++;
	}
	if (i == argc)
		return ls_path(".", lng);
	for (; i < argc; i++) {
		if (argc - i > 1 || i > (lng ? 2 : 1))
			printf("%s:\n", argv[i]);
		ret |= ls_path(argv[i], lng);
	}
	return ret;
}

static int cmd_cat(int argc, char **argv)
{
	int ret = 0;

	if (argc == 1) {
		fflush(stdout);
		return copy_fd(0, 1) < 0;
	}
	for (int i = 1; i < argc; i++)
		ret |= cat_file(argv[i]);
	return ret;
}

static int cmd_cp(int argc, char **argv)
{
	struct stat st;
	int in, out, ret;

	if (argc != 3) {
		fprintf(stderr, "usage: cp <src> <dst>\n");
		return 1;
	}
	in = open(argv[1], O_RDONLY);
	if (in < 0 || fstat(in, &st) < 0) {
		perror(argv[1]);
		return 1;
	}
	out = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
	if (out < 0) {
		perror(argv[2]);
		close(in);
		return 1;
	}
	ret = copy_fd(in, out);
	if (ret < 0)
		perror("cp");
	close(in);
	close(out);
	return ret < 0;
}

static int cmd_mv(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: mv <src> <dst>\n");
		return 1;
	}
	if (rename(argv[1], argv[2]) < 0) {
		perror("mv");
		return 1;
	}
	return 0;
}

static int cmd_touch(int argc, char **argv)
{
	int ret = 0;

	for (int i = 1; i < argc; i++) {
		int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);

		if (fd < 0) {
			perror(argv[i]);
			ret = 1;
			continue;
		}
		futimens(fd, NULL);
		close(fd);
	}
	return ret;
}

static int cmd_mkdir(int argc, char **argv)
{
	int ret = 0;

	for (int i = 1; i < argc; i++) {
		if (mkdir(argv[i], 0755) < 0) {
			perror(argv[i]);
			ret = 1;
		}
	}
	return ret;
}

static int cmd_rm(int argc, char **argv)
{
	int ret = 0;

	for (int i = 1; i < argc; i++) {
		if (unlink(argv[i]) < 0) {
			perror(argv[i]);
			ret = 1;
		}
	}
	return ret;
}

static int cmd_rmdir(int argc, char **argv)
{
	int ret = 0;

	for (int i = 1; i < argc; i++) {
		if (rmdir(argv[i]) < 0) {
			perror(argv[i]);
			ret = 1;
		}
	}
	return ret;
}

/*
 * mount                          list mounts
 * mount [-t type] [-o opts] <source> <dir>
 * Without -t the source is used as the type: "mount tmpfs /tmp".
 */
static int cmd_mount(int argc, char **argv)
{
	const char *type = NULL, *opts = NULL, *src = NULL, *dir = NULL;

	if (argc == 1)
		return cat_file("/proc/mounts");

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
			type = argv[++i];
		else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
			opts = argv[++i];
		else if (!src)
			src = argv[i];
		else if (!dir)
			dir = argv[i];
		else
			src = NULL;
	}
	if (!src || !dir) {
		fprintf(stderr, "usage: mount [-t type] [-o opts] <source> <dir>\n");
		return 1;
	}
	if (mount(src, dir, type ? type : src, 0, opts) < 0) {
		perror("mount");
		return 1;
	}
	return 0;
}

static int cmd_umount(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: umount <dir>\n");
		return 1;
	}
	if (umount(argv[1]) < 0) {
		perror("umount");
		return 1;
	}
	return 0;
}

static int cmd_uname(int argc, char **argv)
{
	struct utsname u;

	if (uname(&u) < 0) {
		perror("uname");
		return 1;
	}
	printf("%s %s %s %s %s\n", u.sysname, u.nodename, u.release,
	       u.version, u.machine);
	return 0;
}

static int cmd_ps(int argc, char **argv)
{
	struct dirent *d;
	DIR *proc = opendir("/proc");

	if (!proc) {
		perror("/proc");
		return 1;
	}
	printf("  PID  PPID S CMD\n");
	while ((d = readdir(proc))) {
		char path[64], stat[512], cmd[256];
		char *l, *r, state = '?';
		int ppid = 0, fd;
		ssize_t n;

		if (!isdigit((unsigned char)d->d_name[0]))
			continue;

		snprintf(path, sizeof(path), "/proc/%s/stat", d->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		n = read(fd, stat, sizeof(stat) - 1);
		close(fd);
		if (n <= 0)
			continue;
		stat[n] = '\0';

		/* "pid (comm) S ppid ..."; comm may contain spaces or ')' */
		l = strchr(stat, '(');
		r = strrchr(stat, ')');
		if (!l || !r || r < l)
			continue;
		*r = '\0';
		sscanf(r + 2, "%c %d", &state, &ppid);

		snprintf(path, sizeof(path), "/proc/%s/cmdline", d->d_name);
		cmd[0] = '\0';
		fd = open(path, O_RDONLY);
		if (fd >= 0) {
			n = read(fd, cmd, sizeof(cmd) - 1);
			close(fd);
			if (n > 0) {
				for (ssize_t i = 0; i < n - 1; i++)
					if (cmd[i] == '\0')
						cmd[i] = ' ';
				cmd[n] = '\0';
			}
		}
		if (cmd[0])
			printf("%5s %5d %c %s\n", d->d_name, ppid, state, cmd);
		else
			printf("%5s %5d %c [%s]\n", d->d_name, ppid, state, l + 1);
	}
	closedir(proc);
	return 0;
}

static int cmd_free(int argc, char **argv)
{
	struct sysinfo si;
	unsigned long long unit;

	if (sysinfo(&si) < 0) {
		perror("sysinfo");
		return 1;
	}
	unit = si.mem_unit ? si.mem_unit : 1;
	printf("%-6s %10s %10s %10s %10s\n", "KiB", "total", "used", "free",
	       "buffers");
	printf("%-6s %10llu %10llu %10llu %10llu\n", "Mem:",
	       si.totalram * unit / 1024,
	       (si.totalram - si.freeram) * unit / 1024,
	       si.freeram * unit / 1024,
	       si.bufferram * unit / 1024);
	return 0;
}

static int cmd_uptime(int argc, char **argv)
{
	struct sysinfo si;

	if (sysinfo(&si) < 0) {
		perror("sysinfo");
		return 1;
	}
	printf("up %ld:%02ld:%02ld, %u processes, load %.2f %.2f %.2f\n",
	       si.uptime / 3600, si.uptime / 60 % 60, si.uptime % 60,
	       si.procs, si.loads[0] / 65536.0, si.loads[1] / 65536.0,
	       si.loads[2] / 65536.0);
	return 0;
}

static int cmd_dmesg(int argc, char **argv)
{
	int size = klogctl(10, NULL, 0);	/* SYSLOG_ACTION_SIZE_BUFFER */
	char *buf;
	int n;

	if (size <= 0)
		size = 16384;
	buf = malloc(size);
	if (!buf) {
		perror("dmesg");
		return 1;
	}
	n = klogctl(3, buf, size);		/* SYSLOG_ACTION_READ_ALL */
	if (n < 0) {
		perror("dmesg");
		free(buf);
		return 1;
	}
	fflush(stdout);
	write_all(1, buf, n);
	free(buf);
	return 0;
}

static int cmd_sleep(int argc, char **argv)
{
	struct timespec ts;
	double sec;

	if (argc != 2) {
		fprintf(stderr, "usage: sleep <seconds>\n");
		return 1;
	}
	sec = strtod(argv[1], NULL);
	ts.tv_sec = (time_t)sec;
	ts.tv_nsec = (long)((sec - ts.tv_sec) * 1e9);
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
	return 0;
}

static const struct {
	const char *name;
	int sig;
} sig_names[] = {
	{ "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT },
	{ "KILL", SIGKILL }, { "SEGV", SIGSEGV }, { "TERM", SIGTERM },
	{ "CONT", SIGCONT }, { "STOP", SIGSTOP }, { "USR1", SIGUSR1 },
	{ "USR2", SIGUSR2 },
};

static int cmd_kill(int argc, char **argv)
{
	int sig = SIGTERM, i = 1, ret = 0;

	if (i < argc && argv[i][0] == '-') {
		const char *s = argv[i] + 1;

		if (isdigit((unsigned char)*s)) {
			sig = atoi(s);
		} else {
			size_t k;

			if (strncmp(s, "SIG", 3) == 0)
				s += 3;
			for (k = 0; k < sizeof(sig_names) / sizeof(sig_names[0]); k++)
				if (strcmp(s, sig_names[k].name) == 0)
					break;
			if (k == sizeof(sig_names) / sizeof(sig_names[0])) {
				fprintf(stderr, "kill: unknown signal %s\n", argv[i]);
				return 1;
			}
			sig = sig_names[k].sig;
		}
		i++;
	}
	if (i == argc) {
		fprintf(stderr, "usage: kill [-SIG] <pid...>\n");
		return 1;
	}
	for (; i < argc; i++) {
		if (kill(atoi(argv[i]), sig) < 0) {
			perror(argv[i]);
			ret = 1;
		}
	}
	return ret;
}

static int cmd_env(int argc, char **argv)
{
	for (char **e = environ; *e; e++)
		printf("%s\n", *e);
	return 0;
}

static int cmd_export(int argc, char **argv)
{
	int ret = 0;

	if (argc == 1)
		return cmd_env(argc, argv);
	for (int i = 1; i < argc; i++) {
		char *eq = strchr(argv[i], '=');

		if (!eq || eq == argv[i]) {
			fprintf(stderr, "usage: export NAME=VALUE\n");
			ret = 1;
			continue;
		}
		*eq = '\0';
		if (setenv(argv[i], eq + 1, 1) < 0) {
			perror("export");
			ret = 1;
		}
		*eq = '=';
	}
	return ret;
}

static int cmd_unset(int argc, char **argv)
{
	for (int i = 1; i < argc; i++)
		unsetenv(argv[i]);
	return 0;
}

static int cmd_sync(int argc, char **argv)
{
	sync();
	return 0;
}

static int cmd_reboot(int argc, char **argv)
{
	sync();
	if (reboot(RB_AUTOBOOT) < 0) {
		perror("reboot");
		return 1;
	}
	return 0;
}

static int cmd_poweroff(int argc, char **argv)
{
	sync();
	if (reboot(RB_POWER_OFF) < 0) {
		perror("poweroff");
		return 1;
	}
	return 0;
}

static int cmd_exit(int argc, char **argv)
{
	if (is_init) {
		fprintf(stderr, "exit: this shell is init, exiting would panic the kernel\n");
		return 1;
	}
	exit(argc > 1 ? atoi(argv[1]) : last_status);
}

struct builtin {
	const char *name;
	int (*fn)(int argc, char **argv);
	const char *usage;
};

static const struct builtin builtins[] = {
	{ "help",     cmd_help,     "list commands" },
	{ "echo",     cmd_echo,     "echo [-n] [text...]" },
	{ "cd",       cmd_cd,       "cd [dir]" },
	{ "pwd",      cmd_pwd,      "print the current directory" },
	{ "ls",       cmd_ls,       "ls [-l] [path...]" },
	{ "cat",      cmd_cat,      "cat [file...]" },
	{ "cp",       cmd_cp,       "cp <src> <dst>" },
	{ "mv",       cmd_mv,       "mv <src> <dst>" },
	{ "touch",    cmd_touch,    "touch <file...>" },
	{ "mkdir",    cmd_mkdir,    "mkdir <dir...>" },
	{ "rm",       cmd_rm,       "rm <file...>" },
	{ "rmdir",    cmd_rmdir,    "rmdir <dir...>" },
	{ "mount",    cmd_mount,    "mount [-t type] [-o opts] <src> <dir>" },
	{ "umount",   cmd_umount,   "umount <dir>" },
	{ "uname",    cmd_uname,    "print kernel information" },
	{ "ps",       cmd_ps,       "list processes" },
	{ "free",     cmd_free,     "show memory usage" },
	{ "uptime",   cmd_uptime,   "show uptime and load" },
	{ "dmesg",    cmd_dmesg,    "print the kernel log" },
	{ "sleep",    cmd_sleep,    "sleep <seconds>" },
	{ "kill",     cmd_kill,     "kill [-SIG] <pid...>" },
	{ "env",      cmd_env,      "print the environment" },
	{ "export",   cmd_export,   "export NAME=VALUE" },
	{ "unset",    cmd_unset,    "unset NAME..." },
	{ "sync",     cmd_sync,     "flush file system buffers" },
	{ "reboot",   cmd_reboot,   "restart the system" },
	{ "poweroff", cmd_poweroff, "power off the system" },
	{ "exit",     cmd_exit,     "exit [status] (not allowed as init)" },
};

#define NR_BUILTINS (sizeof(builtins) / sizeof(builtins[0]))

static const struct builtin *find_builtin(const char *name)
{
	for (size_t i = 0; i < NR_BUILTINS; i++)
		if (strcmp(name, builtins[i].name) == 0)
			return &builtins[i];
	return NULL;
}

static int cmd_help(int argc, char **argv)
{
	for (size_t i = 0; i < NR_BUILTINS; i++)
		printf("  %-9s %s\n", builtins[i].name, builtins[i].usage);
	printf("\nOther commands are searched in $PATH. Supported syntax:\n"
	       "  cmd | cmd    cmd < in    cmd > out    cmd >> out    cmd &\n"
	       "  '...'  \"...\"  \\x  $NAME  $?\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

enum { T_WORD, T_PIPE, T_IN, T_OUT, T_APPEND, T_BG };

struct token {
	int type;
	char *s;
};

struct cmd {
	char *argv[MAX_ARGS + 1];
	int argc;
	char *in;
	char *out;
	int append;
};

struct pipeline {
	struct cmd cmds[MAX_CMDS];
	int ncmds;
	int background;
};

struct wordbuf {
	char *buf;
	size_t len;
	size_t cap;
	int overflow;
};

static void wb_put(struct wordbuf *w, char c)
{
	if (w->len + 1 < w->cap)	/* keep room for the terminator */
		w->buf[w->len++] = c;
	else
		w->overflow = 1;
}

static void wb_puts(struct wordbuf *w, const char *s)
{
	while (*s)
		wb_put(w, *s++);
}

static int is_var_start(char c)
{
	return c == '?' || c == '_' || isalpha((unsigned char)c);
}

/* p points after '$'; returns the position after the variable name */
static const char *expand_var(const char *p, struct wordbuf *w)
{
	char name[64];
	size_t n = 0;
	const char *v;

	if (*p == '?') {
		char num[16];

		snprintf(num, sizeof(num), "%d", last_status);
		wb_puts(w, num);
		return p + 1;
	}
	while ((isalnum((unsigned char)*p) || *p == '_') && n < sizeof(name) - 1)
		name[n++] = *p++;
	name[n] = '\0';
	v = getenv(name);
	if (v)
		wb_puts(w, v);
	return p;
}

static int tokenize(const char *p, struct token *tok, char *buf, size_t cap)
{
	struct wordbuf w = { buf, 0, cap, 0 };
	int n = 0;

	for (;;) {
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		if (!*p || *p == '#')
			break;
		if (n == MAX_TOKENS) {
			fprintf(stderr, "sh: too many words\n");
			return -1;
		}

		switch (*p) {
		case '|':
			tok[n++].type = T_PIPE;
			p++;
			continue;
		case '&':
			tok[n++].type = T_BG;
			p++;
			continue;
		case '<':
			tok[n++].type = T_IN;
			p++;
			continue;
		case '>':
			if (p[1] == '>') {
				tok[n++].type = T_APPEND;
				p += 2;
			} else {
				tok[n++].type = T_OUT;
				p++;
			}
			continue;
		}

		tok[n].type = T_WORD;
		tok[n].s = w.buf + w.len;
		while (*p && !strchr(" \t\r\n|&<>", *p)) {
			if (*p == '\'') {
				for (p++; *p && *p != '\''; p++)
					wb_put(&w, *p);
				if (!*p)
					goto unterminated;
				p++;
			} else if (*p == '"') {
				for (p++; *p && *p != '"';) {
					if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) {
						wb_put(&w, p[1]);
						p += 2;
					} else if (*p == '$' && is_var_start(p[1])) {
						p = expand_var(p + 1, &w);
					} else {
						wb_put(&w, *p++);
					}
				}
				if (!*p)
					goto unterminated;
				p++;
			} else if (*p == '\\' && p[1]) {
				wb_put(&w, p[1]);
				p += 2;
			} else if (*p == '$' && is_var_start(p[1])) {
				p = expand_var(p + 1, &w);
			} else {
				wb_put(&w, *p++);
			}
		}
		if (w.len < w.cap)
			w.buf[w.len++] = '\0';
		else
			w.overflow = 1;
		n++;
	}

	if (w.overflow) {
		fprintf(stderr, "sh: line too long\n");
		return -1;
	}
	return n;

unterminated:
	fprintf(stderr, "sh: unterminated quote\n");
	return -1;
}

/* returns 1 for a command, 0 for an empty line, -1 on a syntax error */
static int parse(struct token *tok, int n, struct pipeline *pl)
{
	struct cmd *c;

	memset(pl, 0, sizeof(*pl));
	pl->ncmds = 1;
	c = &pl->cmds[0];

	for (int i = 0; i < n; i++) {
		switch (tok[i].type) {
		case T_WORD:
			if (c->argc == MAX_ARGS) {
				fprintf(stderr, "sh: too many arguments\n");
				return -1;
			}
			c->argv[c->argc++] = tok[i].s;
			break;
		case T_IN:
		case T_OUT:
		case T_APPEND:
			if (i + 1 >= n || tok[i + 1].type != T_WORD)
				goto syntax;
			if (tok[i].type == T_IN) {
				c->in = tok[++i].s;
			} else {
				c->append = tok[i].type == T_APPEND;
				c->out = tok[++i].s;
			}
			break;
		case T_PIPE:
			if (c->argc == 0)
				goto syntax;
			if (pl->ncmds == MAX_CMDS) {
				fprintf(stderr, "sh: pipeline too long\n");
				return -1;
			}
			c = &pl->cmds[pl->ncmds++];
			break;
		case T_BG:
			if (i != n - 1)
				goto syntax;
			pl->background = 1;
			break;
		}
	}

	if (c->argc == 0) {
		if (pl->ncmds == 1 && !c->in && !c->out && !pl->background)
			return 0;
		goto syntax;
	}
	return 1;

syntax:
	fprintf(stderr, "sh: syntax error\n");
	return -1;
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

static const int job_signals[] = { SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU };

static void set_job_signals(void (*handler)(int))
{
	for (size_t i = 0; i < sizeof(job_signals) / sizeof(job_signals[0]); i++)
		signal(job_signals[i], handler);
}

static int apply_redirects(struct cmd *c)
{
	int fd;

	if (c->in) {
		fd = open(c->in, O_RDONLY);
		if (fd < 0) {
			perror(c->in);
			return -1;
		}
		dup2(fd, 0);
		close(fd);
	}
	if (c->out) {
		fd = open(c->out, O_WRONLY | O_CREAT |
			  (c->append ? O_APPEND : O_TRUNC), 0644);
		if (fd < 0) {
			perror(c->out);
			return -1;
		}
		dup2(fd, 1);
		close(fd);
	}
	return 0;
}

/* run a builtin in the shell process, with its redirections */
static int run_builtin_here(const struct builtin *b, struct cmd *c)
{
	int save_in = -1, save_out = -1, ret = 1;

	fflush(stdout);
	if (c->in)
		save_in = dup(0);
	if (c->out)
		save_out = dup(1);

	if (apply_redirects(c) == 0)
		ret = b->fn(c->argc, c->argv);

	fflush(stdout);
	if (save_in >= 0) {
		dup2(save_in, 0);
		close(save_in);
	}
	if (save_out >= 0) {
		dup2(save_out, 1);
		close(save_out);
	}
	return ret;
}

static void child_exec(struct cmd *c, int background)
{
	const struct builtin *b;

	/* background jobs keep ignoring Ctrl-C, like sh without job control */
	set_job_signals(SIG_DFL);
	if (background) {
		signal(SIGINT, SIG_IGN);
		signal(SIGQUIT, SIG_IGN);
	}

	if (apply_redirects(c) < 0)
		_exit(1);

	b = find_builtin(c->argv[0]);
	if (b) {
		int ret = b->fn(c->argc, c->argv);

		fflush(stdout);
		_exit(ret);
	}

	execvp(c->argv[0], c->argv);
	if (errno == ENOENT) {
		fprintf(stderr, "%s: command not found\n", c->argv[0]);
		_exit(127);
	}
	fprintf(stderr, "%s: %s\n", c->argv[0], strerror(errno));
	_exit(126);
}

static int wait_status(pid_t pid, const char *name)
{
	int st;

	while (waitpid(pid, &st, 0) < 0) {
		if (errno != EINTR) {
			perror("waitpid");
			return 1;
		}
	}
	if (WIFSIGNALED(st)) {
		int sig = WTERMSIG(st);

		if (sig == SIGINT)
			putchar('\n');
		else
			fprintf(stderr, "%s: %s%s\n", name, strsignal(sig),
				WCOREDUMP(st) ? " (core dumped)" : "");
		return 128 + sig;
	}
	return WEXITSTATUS(st);
}

static int run_pipeline(struct pipeline *pl)
{
	pid_t pids[MAX_CMDS];
	int prev_rd = -1, started = 0, ret = 0;

	if (pl->ncmds == 1 && !pl->background) {
		const struct builtin *b = find_builtin(pl->cmds[0].argv[0]);

		if (b)
			return run_builtin_here(b, &pl->cmds[0]);
	}

	for (int i = 0; i < pl->ncmds; i++) {
		int pfd[2] = { -1, -1 };
		pid_t pid;

		if (i < pl->ncmds - 1 && pipe(pfd) < 0) {
			perror("pipe");
			ret = 1;
			break;
		}

		fflush(stdout);
		pid = fork();
		if (pid < 0) {
			perror("fork");
			if (pfd[0] >= 0) {
				close(pfd[0]);
				close(pfd[1]);
			}
			ret = 1;
			break;
		}

		if (pid == 0) {
			if (prev_rd >= 0) {
				dup2(prev_rd, 0);
				close(prev_rd);
			} else if (pl->background && !pl->cmds[i].in) {
				int null = open("/dev/null", O_RDONLY);

				if (null >= 0) {
					dup2(null, 0);
					close(null);
				}
			}
			if (pfd[1] >= 0) {
				dup2(pfd[1], 1);
				close(pfd[1]);
				close(pfd[0]);
			}
			child_exec(&pl->cmds[i], pl->background);
		}

		if (prev_rd >= 0)
			close(prev_rd);
		if (pfd[1] >= 0)
			close(pfd[1]);
		prev_rd = pfd[0];
		pids[started++] = pid;
	}
	if (prev_rd >= 0)
		close(prev_rd);

	if (pl->background) {
		if (started)
			printf("[%d]\n", pids[started - 1]);
		return ret;
	}

	for (int i = 0; i < started; i++) {
		int st = wait_status(pids[i], pl->cmds[i].argv[0]);

		if (i == started - 1 && !ret)
			ret = st;
	}
	return ret;
}

/* collect finished background jobs and orphans re-parented to init */
static void reap_children(void)
{
	pid_t pid;
	int st;

	while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
		if (WIFSIGNALED(st))
			printf("[%d] killed: %s\n", pid, strsignal(WTERMSIG(st)));
		else
			printf("[%d] done, status %d\n", pid, WEXITSTATUS(st));
	}
}

/* ------------------------------------------------------------------ */
/* System setup (PID 1 only)                                           */
/* ------------------------------------------------------------------ */

static void mount_fs(const char *type, const char *dir)
{
	if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
		fprintf(stderr, "init: mkdir %s: %s\n", dir, strerror(errno));
		return;
	}
	if (mount(type, dir, type, 0, NULL) < 0 && errno != EBUSY)
		fprintf(stderr, "init: mount %s on %s: %s\n", type, dir,
			strerror(errno));
}

static void setup_system(void)
{
	mount_fs("proc", "/proc");
	mount_fs("sysfs", "/sys");
	mount_fs("devtmpfs", "/dev");	/* needs CONFIG_DEVTMPFS */
	mkdir("/tmp", 01777);

	setenv("PATH", DEFAULT_PATH, 0);
	setenv("HOME", "/", 0);
	setenv("TERM", "linux", 0);
	setenv("SHELL", "/init", 1);
	chdir("/");
}

/*
 * Use the serial port as stdin/stdout/stderr and as the controlling
 * terminal, so Ctrl-C reaches the foreground program. /dev/console cannot
 * always be a controlling terminal, so try /dev/ttyS0 first.
 */
static void setup_console(void)
{
	int fd = open(CONSOLE_TTY, O_RDWR | O_NOCTTY);

	if (fd < 0)
		fd = open("/dev/console", O_RDWR | O_NOCTTY);
	if (fd < 0)
		return;		/* keep the descriptors from the kernel */

	setsid();
	if (ioctl(fd, TIOCSCTTY, 1) < 0)
		fprintf(stderr, "init: no controlling terminal, Ctrl-C disabled\n");

	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2)
		close(fd);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static void prompt(void)
{
	char cwd[PATH_MAX];

	if (!getcwd(cwd, sizeof(cwd)))
		strcpy(cwd, "?");
	printf("soc2:%s%c ", cwd, geteuid() == 0 ? '#' : '$');
	fflush(stdout);
}

int main(int argc, char **argv)
{
	static char line[LINE_MAX_LEN];
	static char words[WORD_BUF_LEN];
	struct token tok[MAX_TOKENS];
	struct pipeline pl;
	struct utsname u;

	is_init = getpid() == 1;
	if (is_init) {
		setup_system();
		setup_console();
	}
	set_job_signals(SIG_IGN);

	if (uname(&u) == 0)
		printf("\n%s %s on %s\n", u.sysname, u.release, u.machine);
	printf("SoC2 init shell, pid %d. Type 'help' for commands.\n\n",
	       (int)getpid());

	for (;;) {
		int n;

		prompt();
		if (!fgets(line, sizeof(line), stdin)) {
			clearerr(stdin);
			if (!is_init)
				break;
			putchar('\n');	/* Ctrl-D: init must not exit */
			continue;
		}
		if (!strchr(line, '\n') && strlen(line) == sizeof(line) - 1) {
			int c;

			while ((c = getchar()) != EOF && c != '\n')
				;
			fprintf(stderr, "sh: line too long\n");
			continue;
		}

		n = tokenize(line, tok, words, sizeof(words));
		if (n > 0) {
			int r = parse(tok, n, &pl);

			if (r > 0)
				last_status = run_pipeline(&pl);
			else if (r < 0)
				last_status = 2;
		} else if (n < 0) {
			last_status = 2;
		}

		reap_children();
	}
	return last_status;
}
