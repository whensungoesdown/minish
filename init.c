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
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
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

/*
 * memtest [rounds]
 *
 * Checks that every word stored is read back unchanged, in the two cases
 * where libc data has been seen corrupted on SoC2:
 *
 *   fresh  each round maps 16 new anonymous pages, writes them and reads
 *          them back. Every first store takes a TLB refill and a page
 *          fault, as when the stack grows.
 *   fork   a 16-page buffer is filled and the process forks. Parent and
 *          child then keep rewriting the buffer with their own patterns and
 *          checking it. The first write of each breaks copy-on-write; after
 *          that each must only ever see its own values.
 *
 * Each word's pattern depends on its address, so a word read from the
 * wrong place is detected as well as a stale value.
 */
#define MT_PAGES	16
#define MT_SIZE		(MT_PAGES * 4096)
#define MT_WORDS	(MT_SIZE / sizeof(unsigned long))

static unsigned long mt_pattern(volatile unsigned long *p, unsigned long seed)
{
	return ((unsigned long)p * 2654435761UL) ^ seed;
}

static void mt_fill(volatile unsigned long *buf, unsigned long seed)
{
	for (size_t i = 0; i < MT_WORDS; i++)
		buf[i] = mt_pattern(&buf[i], seed);
}

static long mt_check(volatile unsigned long *buf, unsigned long seed,
		     const char *what, int round)
{
	long bad = 0;

	for (size_t i = 0; i < MT_WORDS; i++) {
		unsigned long exp = mt_pattern(&buf[i], seed);
		unsigned long got = buf[i];

		if (got != exp) {
			if (bad < 4)
				printf("  %s round %d: %p = %08lx, expected %08lx\n",
				       what, round, (void *)&buf[i], got, exp);
			bad++;
		}
	}
	return bad;
}

static volatile unsigned long *mt_map(void)
{
	void *p = mmap(NULL, MT_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED) {
		perror("memtest: mmap");
		return NULL;
	}
	return p;
}

static int cmd_memtest(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 100;
	volatile unsigned long *buf;
	long bad_fresh = 0, bad_parent = 0;
	int child_bad = 0, st, r;
	pid_t pid;

	if (rounds < 1)
		rounds = 1;
	printf("memtest: %d rounds, %d pages\n", rounds, MT_PAGES);
	fflush(stdout);

	/* fresh pages */
	for (r = 0; r < rounds; r++) {
		buf = mt_map();
		if (!buf)
			return 1;
		mt_fill(buf, 0x11111111UL + r);
		bad_fresh += mt_check(buf, 0x11111111UL + r, "fresh", r);
		munmap((void *)buf, MT_SIZE);
	}
	printf("  fresh pages: %ld bad words\n", bad_fresh);
	fflush(stdout);

	/* copy-on-write after fork */
	buf = mt_map();
	if (!buf)
		return 1;
	mt_fill(buf, 0x22222222UL);

	pid = fork();
	if (pid < 0) {
		perror("memtest: fork");
		munmap((void *)buf, MT_SIZE);
		return 1;
	}
	if (pid == 0) {
		long bad = 0;

		for (r = 0; r < rounds; r++) {
			mt_fill(buf, 0xc0c0c0c0UL ^ r);
			bad += mt_check(buf, 0xc0c0c0c0UL ^ r, "child", r);
		}
		fflush(stdout);
		_exit(bad ? 1 : 0);
	}

	for (r = 0; r < rounds; r++) {
		mt_fill(buf, 0x5a5a5a5aUL ^ r);
		bad_parent += mt_check(buf, 0x5a5a5a5aUL ^ r, "parent", r);
	}
	if (waitpid(pid, &st, 0) < 0)
		perror("memtest: waitpid");
	else if (WIFSIGNALED(st))
		child_bad = -WTERMSIG(st);
	else
		child_bad = WEXITSTATUS(st);
	munmap((void *)buf, MT_SIZE);

	printf("  fork, parent: %ld bad words\n", bad_parent);
	if (child_bad < 0)
		printf("  fork, child: killed by %s\n", strsignal(-child_bad));
	else
		printf("  fork, child: %s\n", child_bad ? "bad words (see above)" : "0 bad words");

	if (bad_fresh || bad_parent || child_bad) {
		printf("memtest: FAIL\n");
		return 1;
	}
	printf("memtest: PASS\n");
	return 0;
}

/*
 * snprintf crash test. The libc crash in vfprintf comes from the FILE
 * that snprintf builds on the stack not being initialized. It shows up
 * mostly when the first command after boot is ps, so test whether it is
 * the first snprintf in a process that fails.
 *
 *   snp         one snprintf in this process (as PID 1, a crash panics)
 *   snptest [n] n fresh processes (fork + exec /init --snptest), each
 *               doing its first snprintf; counts crashes
 */
#define SNP_EXPECT	"/proc/1/stat"

static int snp_once(void)
{
	char buf[64];
	int n = snprintf(buf, sizeof(buf), "/proc/%s/stat", "1");

	return (n == (int)strlen(SNP_EXPECT) && strcmp(buf, SNP_EXPECT) == 0) ? 0 : 3;
}

static int cmd_snp(int argc, char **argv)
{
	int r = snp_once();

	printf("snp: %s\n", r ? "wrong result" : "ok");
	return r;
}

static int cmd_snptest(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 20;
	int ok = 0, wrong = 0, segv = 0, other = 0;

	for (int i = 0; i < n; i++) {
		pid_t pid;
		int st;

		fflush(stdout);
		pid = fork();
		if (pid < 0) {
			perror("snptest: fork");
			break;
		}
		if (pid == 0) {
			char *av[] = { "/init", "--snptest", NULL };

			execv("/init", av);
			_exit(127);
		}
		if (waitpid(pid, &st, 0) < 0) {
			perror("snptest: waitpid");
			break;
		}
		if (WIFSIGNALED(st)) {
			if (WTERMSIG(st) == SIGSEGV)
				segv++;
			else
				other++;
		} else if (WEXITSTATUS(st) == 0) {
			ok++;
		} else {
			wrong++;
		}
	}
	printf("snptest: %d runs: %d ok, %d SIGSEGV, %d other signal, %d wrong result or exec failed\n",
	       n, ok, segv, other, wrong);
	return segv || other || wrong;
}

/*
 * fartest: does a call into a code page that is not mapped yet run the
 * called function?
 *
 * In the snprintf crash, everything _IO_no_init() should store into the
 * FILE is missing, while the stores before and after the call are there,
 * as if the call returned without running the function. It happens on
 * the first use of that code in a process, when the call target is on a
 * page that is not mapped yet and the fetch takes a TLB refill and a
 * page fault.
 *
 * far1() and far2() are each on their own 64 KB aligned block, so the
 * kernel's fault-around does not map them before they are called. Each
 * child calls both once and reports the return values, the stores and
 * the number of page faults the call took (getrusage ru_minflt).
 */
struct far_rec {
	unsigned long a, b, c, d;
};

#define FAR_FN(sec) __attribute__((noinline, noclone, aligned(65536), section(sec)))

FAR_FN(".text.soc2_far1")
static int far1(struct far_rec *r)
{
	r->a = 0x11111111;
	r->b = 0x22222222;
	r->c = 0x33333333;
	r->d = 0x44444444;
	return 0x5a;
}

FAR_FN(".text.soc2_far2")
static int far2(struct far_rec *r)
{
	r->a = 0x55555555;
	r->b = 0x66666666;
	r->c = 0x77777777;
	r->d = 0x88888888;
	return 0xa5;
}

/*
 * far3 runs straight through a 64 KB boundary: a and b are stored in the
 * first block, then 64 KB of nops, then c and d in the next block. The
 * fault on the first block maps only that block (fault-around), so the
 * fetch of the second block faults in the middle of the function, after
 * a plain sequential fetch, not after a branch.
 */
FAR_FN(".text.soc2_far3")
static int far3(struct far_rec *r)
{
	r->a = 0x99999999;
	r->b = 0xaaaaaaaa;
	__asm__ volatile(".rept 16384\n\tnop\n\t.endr" ::: "memory");
	r->c = 0xbbbbbbbb;
	r->d = 0xcccccccc;
	return 0x3c;
}

/*
 * far4 is the libc case: vsnprintf calls _IO_no_init, which is on the
 * other page of the same ITLB pair, mapped by fault-around but not yet
 * valid (PTE present, V=0). far4 starts a 64 KB block (even page), its
 * callee is 4 KB further (odd page of the same pair). Running far4 makes
 * the pair's ITLB entry valid for the even page only, so the call is a
 * branch into an ITLB hit with V=0, which must raise PIF. If the callee
 * is skipped, b and c stay 0 and the return value is wrong.
 */
/*
 * Written in assembly so the layout is exact: the compiler placed a C
 * callee in another 64 KB block. far4 starts a 64 KB block (even page of
 * an ITLB pair), far4_callee is at far4 + 0x1000 (odd page of the same
 * pair). struct far_rec: a at 0, b at 4, c at 8, d at 12.
 */
int far4(struct far_rec *r);
int far4_callee(struct far_rec *r);

__asm__(
	"	.pushsection .text.soc2_far4, \"ax\", @progbits\n"
	"	.balign	65536\n"
	"	.globl	far4\n"
	"	.type	far4, @function\n"
	"far4:\n"
	"	addi.w	$r3, $r3, -16\n"
	"	st.w	$r1, $r3, 12\n"
	"	st.w	$r4, $r3, 8\n"
	"	li.w	$r12, 0xdddddddd\n"
	"	st.w	$r12, $r4, 0\n"		/* r->a */
	"	bl	far4_callee\n"		/* into the odd page of this pair */
	"	ld.w	$r5, $r3, 8\n"
	"	li.w	$r12, 0x12345678\n"
	"	st.w	$r12, $r5, 12\n"	/* r->d */
	"	ld.w	$r1, $r3, 12\n"
	"	addi.w	$r3, $r3, 16\n"
	"	jirl	$r0, $r1, 0\n"
	"	.size	far4, . - far4\n"
	"	.org	0x1000\n"
	"	.globl	far4_callee\n"
	"	.type	far4_callee, @function\n"
	"far4_callee:\n"
	"	li.w	$r12, 0xeeeeeeee\n"
	"	st.w	$r12, $r4, 4\n"		/* r->b */
	"	li.w	$r12, 0xffffffff\n"
	"	st.w	$r12, $r4, 8\n"		/* r->c */
	"	li.w	$r4, 0x77\n"
	"	jirl	$r0, $r1, 0\n"
	"	.size	far4_callee, . - far4_callee\n"
	"	.popsection\n");

/*
 * far5 is the exact libc case. vsnprintf calls _IO_no_init, which is on
 * a page that fault-around already mapped (PTE present, V=0) but whose
 * ITLB pair is not in the TLB. The call then takes two exceptions on the
 * same target: TLB refill (loads the pair with V=0), then PIF, handled by
 * the fast path in tlbex-32.S (sets V, tlbwr), then ertn to the target.
 *
 * far5 starts a 64 KB block; its first fetch faults the block in, so
 * fault-around maps the other pages of the block. It then calls into two
 * other pairs of the block: far5_even at +0x2000 (even page of pair 2/3)
 * and far5_odd at +0x5000 (odd page of pair 4/5). Neither pair is in the
 * ITLB at the time of the call.
 */
int far5(struct far_rec *r);

__asm__(
	"	.pushsection .text.soc2_far5, \"ax\", @progbits\n"
	"	.balign	65536\n"
	"	.globl	far5\n"
	"	.type	far5, @function\n"
	"far5:\n"
	"	addi.w	$r3, $r3, -16\n"
	"	st.w	$r1, $r3, 12\n"
	"	st.w	$r4, $r3, 8\n"
	"	li.w	$r12, 0x13131313\n"
	"	st.w	$r12, $r4, 0\n"		/* r->a */
	"	bl	far5_even\n"		/* new pair, even page */
	"	ld.w	$r4, $r3, 8\n"
	"	bl	far5_odd\n"		/* new pair, odd page, returns 0x55 */
	"	ld.w	$r5, $r3, 8\n"
	"	li.w	$r12, 0x46464646\n"
	"	st.w	$r12, $r5, 12\n"	/* r->d */
	"	ld.w	$r1, $r3, 12\n"
	"	addi.w	$r3, $r3, 16\n"
	"	jirl	$r0, $r1, 0\n"
	"	.size	far5, . - far5\n"
	"	.org	0x2000\n"
	"	.type	far5_even, @function\n"
	"far5_even:\n"
	"	li.w	$r12, 0x24242424\n"
	"	st.w	$r12, $r4, 4\n"		/* r->b */
	"	jirl	$r0, $r1, 0\n"
	"	.size	far5_even, . - far5_even\n"
	"	.org	0x5000\n"
	"	.type	far5_odd, @function\n"
	"far5_odd:\n"
	"	li.w	$r12, 0x35353535\n"
	"	st.w	$r12, $r4, 8\n"		/* r->c */
	"	li.w	$r4, 0x55\n"
	"	jirl	$r0, $r1, 0\n"
	"	.size	far5_odd, . - far5_odd\n"
	"	.popsection\n");

/*
 * far6..far8: is the first instruction of a page skipped when execution
 * runs into that page sequentially and its fetch faults?
 *
 * In libc, "bl _IO_no_init" is at offset 0x70000, the first word of a
 * page; the instruction before it (0x6fffc) is the last word of the
 * previous page, which is the even page of the same ITLB pair. If the
 * bl is lost after the fetch fault, _IO_no_init never runs and vsnprintf
 * continues at 0x70004, which is what the crash dumps show. far3 crossed
 * a page boundary too, but on a nop, where a lost instruction is not
 * visible.
 *
 * far6  the libc layout: store at 0xffc, bl at 0x1000 (odd page of the
 *       pair, mapped by fault-around, V=0)
 * far7  the same, with a store at 0x1000
 * far8  a store at the first word of the next 64 KB block
 *
 * The unused gaps are filled with zeros; they are jumped over, never
 * executed.
 */
int far6(struct far_rec *r);
int far7(struct far_rec *r);
int far8(struct far_rec *r);

__asm__(
	"	.pushsection .text.soc2_far6, \"ax\", @progbits\n"
	"	.balign	65536\n"
	"	.globl	far6\n"
	"	.type	far6, @function\n"
	"far6:\n"
	"	addi.w	$r3, $r3, -16\n"
	"	st.w	$r1, $r3, 12\n"
	"	st.w	$r4, $r3, 8\n"
	"	li.w	$r12, 0x61616161\n"
	"	st.w	$r12, $r4, 0\n"		/* r->a */
	"	b	1f\n"
	"	.org	0xffc\n"
	"1:	st.w	$r4, $r3, 4\n"		/* last word of the even page */
	"	bl	far6_callee\n"		/* first word of the odd page */
	"	ld.w	$r5, $r3, 8\n"
	"	li.w	$r12, 0x64646464\n"
	"	st.w	$r12, $r5, 12\n"	/* r->d */
	"	ld.w	$r1, $r3, 12\n"
	"	addi.w	$r3, $r3, 16\n"
	"	jirl	$r0, $r1, 0\n"
	"	.size	far6, . - far6\n"
	"	.org	0x1100\n"
	"	.type	far6_callee, @function\n"
	"far6_callee:\n"
	"	li.w	$r12, 0x62626262\n"
	"	st.w	$r12, $r4, 4\n"		/* r->b */
	"	li.w	$r12, 0x63636363\n"
	"	st.w	$r12, $r4, 8\n"		/* r->c */
	"	li.w	$r4, 0x66\n"
	"	jirl	$r0, $r1, 0\n"
	"	.size	far6_callee, . - far6_callee\n"
	"	.popsection\n"

	"	.pushsection .text.soc2_far7, \"ax\", @progbits\n"
	"	.balign	65536\n"
	"	.globl	far7\n"
	"	.type	far7, @function\n"
	"far7:\n"
	"	li.w	$r12, 0x71717171\n"
	"	st.w	$r12, $r4, 0\n"		/* r->a */
	"	li.w	$r13, 0x72727272\n"
	"	li.w	$r14, 0x73737373\n"
	"	b	1f\n"
	"	.org	0xffc\n"
	"1:	st.w	$r13, $r4, 4\n"		/* r->b, last word of the even page */
	"	st.w	$r14, $r4, 8\n"		/* r->c, first word of the odd page */
	"	li.w	$r12, 0x74747474\n"
	"	st.w	$r12, $r4, 12\n"	/* r->d */
	"	li.w	$r4, 0x77\n"
	"	jirl	$r0, $r1, 0\n"
	"	.size	far7, . - far7\n"
	"	.popsection\n"

	"	.pushsection .text.soc2_far8, \"ax\", @progbits\n"
	"	.balign	65536\n"
	"	.globl	far8\n"
	"	.type	far8, @function\n"
	"far8:\n"
	"	li.w	$r12, 0x81818181\n"
	"	st.w	$r12, $r4, 0\n"		/* r->a */
	"	li.w	$r13, 0x82828282\n"
	"	li.w	$r14, 0x83838383\n"
	"	b	1f\n"
	"	.org	0xfffc\n"
	"1:	st.w	$r13, $r4, 4\n"		/* r->b, last word of the 64 KB block */
	"	st.w	$r14, $r4, 8\n"		/* r->c, first word of the next block */
	"	li.w	$r12, 0x84848484\n"
	"	st.w	$r12, $r4, 12\n"	/* r->d */
	"	li.w	$r4, 0x88\n"
	"	jirl	$r0, $r1, 0\n"
	"	.size	far8, . - far8\n"
	"	.popsection\n");

static long minflt(void)
{
	struct rusage ru;

	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_minflt;
}

static int far_call(const char *name, int (*fn)(struct far_rec *), int want,
		    const struct far_rec *exp)
{
	struct far_rec r = { 0, 0, 0, 0 };
	long f0, f1;
	int ret, stores_ok;

	f0 = minflt();
	ret = fn(&r);
	f1 = minflt();

	stores_ok = r.a == exp->a && r.b == exp->b &&
		    r.c == exp->c && r.d == exp->d;
	printf("  %s: ret %s, stores %s, page faults in call %ld%s\n", name,
	       ret == want ? "ok" : "WRONG", stores_ok ? "ok" : "WRONG", f1 - f0,
	       f1 == f0 ? " (page was mapped, path not tested)" : "");
	if (!stores_ok)
		printf("    got a=%08lx b=%08lx c=%08lx d=%08lx\n",
		       r.a, r.b, r.c, r.d);
	return (ret == want && stores_ok) ? 0 : 1;
}

static int far_once(void)
{
	static const struct far_rec e1 = { 0x11111111, 0x22222222, 0x33333333, 0x44444444 };
	static const struct far_rec e2 = { 0x55555555, 0x66666666, 0x77777777, 0x88888888 };
	static const struct far_rec e3 = { 0x99999999, 0xaaaaaaaa, 0xbbbbbbbb, 0xcccccccc };
	static const struct far_rec e4 = { 0xdddddddd, 0xeeeeeeee, 0xffffffff, 0x12345678 };
	static const struct far_rec e5 = { 0x13131313, 0x24242424, 0x35353535, 0x46464646 };
	static const struct far_rec e6 = { 0x61616161, 0x62626262, 0x63636363, 0x64646464 };
	static const struct far_rec e7 = { 0x71717171, 0x72727272, 0x73737373, 0x74747474 };
	static const struct far_rec e8 = { 0x81818181, 0x82828282, 0x83838383, 0x84848484 };
	int bad = 0;

	/* page-boundary tests first, before other calls map neighbouring blocks */
	bad |= far_call("far6", far6, 0x66, &e6) << 5;
	bad |= far_call("far7", far7, 0x77, &e7) << 6;
	bad |= far_call("far8", far8, 0x88, &e8) << 7;

	bad |= far_call("far1", far1, 0x5a, &e1);
	bad |= far_call("far2", far2, 0xa5, &e2) << 1;
	bad |= far_call("far3", far3, 0x3c, &e3) << 2;
	/* far4 must be on the even page and its callee on the odd page of one pair */
	if (((unsigned long)far4 & ~0x1fffUL) != ((unsigned long)far4_callee & ~0x1fffUL) ||
	    !((unsigned long)far4_callee & 0x1000))
		printf("  far4: layout not as intended (far4 %p, callee %p), result not meaningful\n",
		       (void *)far4, (void *)far4_callee);
	bad |= far_call("far4", far4, 0x77, &e4) << 3;
	bad |= far_call("far5", far5, 0x55, &e5) << 4;
	fflush(stdout);
	return bad;
}

static int cmd_fartest(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 10;
	int ok = 0, bad = 0, sig = 0;

	printf("fartest: far1 at %p, far2 at %p, far3 at %p, far4 at %p calls %p, far5 at %p,\n"
	       "         far6 at %p, far7 at %p, far8 at %p, %d fresh processes\n",
	       (void *)far1, (void *)far2, (void *)far3, (void *)far4,
	       (void *)far4_callee, (void *)far5, (void *)far6, (void *)far7,
	       (void *)far8, n);
	for (int i = 0; i < n; i++) {
		pid_t pid;
		int st;

		fflush(stdout);
		pid = fork();
		if (pid < 0) {
			perror("fartest: fork");
			break;
		}
		if (pid == 0) {
			char *av[] = { "/init", "--fartest", NULL };

			execv("/init", av);
			_exit(127);
		}
		if (waitpid(pid, &st, 0) < 0) {
			perror("fartest: waitpid");
			break;
		}
		if (WIFSIGNALED(st))
			sig++;
		else if (WEXITSTATUS(st) == 0)
			ok++;
		else
			bad++;
	}
	printf("fartest: %d ok, %d with a wrong result, %d killed by a signal\n",
	       ok, bad, sig);
	return bad || sig;
}

/*
 * Upstream GCC/clang define __loongarch__; older Loongson toolchains may
 * only define one of the other names. Check with:
 *   loongarch32r-linux-gnusf-gcc -dM -E - </dev/null | grep -i loongarch
 */
#if defined(__loongarch__) || defined(__loongarch) || \
    defined(__loongarch32) || defined(__loongarch64)
#define MD_HAVE_ASM 1
#else
#define MD_HAVE_ASM 0
#endif

/* ------------------------------------------------------------------ */
/* Dhrystone 2.1                                                       */
/* ------------------------------------------------------------------ */

/*
 * Dhrystone 2.1 by Reinhold P. Weicker (public domain), following the C
 * version by Rick Richardson. The benchmark code is unchanged, except:
 *   - the procedures are noinline/noclone, so that -O2 cannot inline
 *     them into the loop or specialise them for the constant arguments
 *   - the two records are static instead of malloc'ed
 *   - two locals that the original leaves uninitialised are initialised
 *     (their values are always overwritten before use)
 *
 * dhrystone [runs] [MHz]
 *   Without runs, the number of runs is calibrated to about 2 s. MHz is
 *   the CPU clock used for DMIPS/MHz, 75 by default (SoC2 sys_clk).
 *   DMIPS = Dhrystones per second / 1757 (VAX 11/780).
 *
 * At the end the global and local values are checked against the
 * values the original program prints as "should be", so the run is
 * also a correctness test of the CPU.
 */
typedef enum { Ident_1, Ident_2, Ident_3, Ident_4, Ident_5 } Enumeration;

typedef struct dhry_record {
	struct dhry_record *Ptr_Comp;
	Enumeration Discr;
	union {
		struct {
			Enumeration Enum_Comp;
			int Int_Comp;
			char Str_Comp[31];
		} var_1;
		struct {
			Enumeration E_Comp_2;
			char Str_2_Comp[31];
		} var_2;
		struct {
			char Ch_1_Comp;
			char Ch_2_Comp;
		} var_3;
	} variant;
} Rec_Type, *Rec_Pointer;

static Rec_Type dhry_rec[2];
static Rec_Pointer Ptr_Glob, Next_Ptr_Glob;
static int Int_Glob;
static int Bool_Glob;
static char Ch_1_Glob, Ch_2_Glob;
static int Arr_1_Glob[50];
static int Arr_2_Glob[50][50];

#define DHRY_PROC __attribute__((noinline, noclone))

DHRY_PROC static void Proc_1(Rec_Pointer Ptr_Val_Par);
DHRY_PROC static void Proc_2(int *Int_Par_Ref);
DHRY_PROC static void Proc_3(Rec_Pointer *Ptr_Ref_Par);
DHRY_PROC static void Proc_4(void);
DHRY_PROC static void Proc_5(void);
DHRY_PROC static void Proc_6(Enumeration Enum_Val_Par, Enumeration *Enum_Ref_Par);
DHRY_PROC static void Proc_7(int Int_1_Par_Val, int Int_2_Par_Val, int *Int_Par_Ref);
DHRY_PROC static void Proc_8(int Arr_1_Par_Ref[50], int Arr_2_Par_Ref[50][50],
			     int Int_1_Par_Val, int Int_2_Par_Val);
DHRY_PROC static Enumeration Func_1(char Ch_1_Par_Val, char Ch_2_Par_Val);
DHRY_PROC static int Func_2(char *Str_1_Par_Ref, char *Str_2_Par_Ref);
DHRY_PROC static int Func_3(Enumeration Enum_Par_Val);

static void Proc_1(Rec_Pointer Ptr_Val_Par)
{
	Rec_Pointer Next_Record = Ptr_Val_Par->Ptr_Comp;

	*Ptr_Val_Par->Ptr_Comp = *Ptr_Glob;
	Ptr_Val_Par->variant.var_1.Int_Comp = 5;
	Next_Record->variant.var_1.Int_Comp = Ptr_Val_Par->variant.var_1.Int_Comp;
	Next_Record->Ptr_Comp = Ptr_Val_Par->Ptr_Comp;
	Proc_3(&Next_Record->Ptr_Comp);
	if (Next_Record->Discr == Ident_1) {
		Next_Record->variant.var_1.Int_Comp = 6;
		Proc_6(Ptr_Val_Par->variant.var_1.Enum_Comp,
		       &Next_Record->variant.var_1.Enum_Comp);
		Next_Record->Ptr_Comp = Ptr_Glob->Ptr_Comp;
		Proc_7(Next_Record->variant.var_1.Int_Comp, 10,
		       &Next_Record->variant.var_1.Int_Comp);
	} else {
		*Ptr_Val_Par = *Ptr_Val_Par->Ptr_Comp;
	}
}

static void Proc_2(int *Int_Par_Ref)
{
	int Int_Loc;
	Enumeration Enum_Loc = Ident_2;

	Int_Loc = *Int_Par_Ref + 10;
	do {
		if (Ch_1_Glob == 'A') {
			Int_Loc -= 1;
			*Int_Par_Ref = Int_Loc - Int_Glob;
			Enum_Loc = Ident_1;
		}
	} while (Enum_Loc != Ident_1);
}

static void Proc_3(Rec_Pointer *Ptr_Ref_Par)
{
	if (Ptr_Glob != NULL)
		*Ptr_Ref_Par = Ptr_Glob->Ptr_Comp;
	Proc_7(10, Int_Glob, &Ptr_Glob->variant.var_1.Int_Comp);
}

static void Proc_4(void)
{
	int Bool_Loc;

	Bool_Loc = Ch_1_Glob == 'A';
	Bool_Glob = Bool_Loc | Bool_Glob;
	Ch_2_Glob = 'B';
}

static void Proc_5(void)
{
	Ch_1_Glob = 'A';
	Bool_Glob = 0;
}

static void Proc_6(Enumeration Enum_Val_Par, Enumeration *Enum_Ref_Par)
{
	*Enum_Ref_Par = Enum_Val_Par;
	if (!Func_3(Enum_Val_Par))
		*Enum_Ref_Par = Ident_4;
	switch (Enum_Val_Par) {
	case Ident_1:
		*Enum_Ref_Par = Ident_1;
		break;
	case Ident_2:
		if (Int_Glob > 100)
			*Enum_Ref_Par = Ident_1;
		else
			*Enum_Ref_Par = Ident_4;
		break;
	case Ident_3:
		*Enum_Ref_Par = Ident_2;
		break;
	case Ident_4:
		break;
	case Ident_5:
		*Enum_Ref_Par = Ident_3;
		break;
	}
}

static void Proc_7(int Int_1_Par_Val, int Int_2_Par_Val, int *Int_Par_Ref)
{
	int Int_Loc;

	Int_Loc = Int_1_Par_Val + 2;
	*Int_Par_Ref = Int_2_Par_Val + Int_Loc;
}

static void Proc_8(int Arr_1_Par_Ref[50], int Arr_2_Par_Ref[50][50],
		   int Int_1_Par_Val, int Int_2_Par_Val)
{
	int Int_Index, Int_Loc;

	Int_Loc = Int_1_Par_Val + 5;
	Arr_1_Par_Ref[Int_Loc] = Int_2_Par_Val;
	Arr_1_Par_Ref[Int_Loc + 1] = Arr_1_Par_Ref[Int_Loc];
	Arr_1_Par_Ref[Int_Loc + 30] = Int_Loc;
	for (Int_Index = Int_Loc; Int_Index <= Int_Loc + 1; ++Int_Index)
		Arr_2_Par_Ref[Int_Loc][Int_Index] = Int_Loc;
	Arr_2_Par_Ref[Int_Loc][Int_Loc - 1] += 1;
	Arr_2_Par_Ref[Int_Loc + 20][Int_Loc] = Arr_1_Par_Ref[Int_Loc];
	Int_Glob = 5;
}

static Enumeration Func_1(char Ch_1_Par_Val, char Ch_2_Par_Val)
{
	char Ch_1_Loc, Ch_2_Loc;

	Ch_1_Loc = Ch_1_Par_Val;
	Ch_2_Loc = Ch_1_Loc;
	if (Ch_2_Loc != Ch_2_Par_Val)
		return Ident_1;
	Ch_1_Glob = Ch_1_Loc;
	return Ident_2;
}

static int Func_2(char *Str_1_Par_Ref, char *Str_2_Par_Ref)
{
	int Int_Loc;
	char Ch_Loc = 0;

	Int_Loc = 2;
	while (Int_Loc <= 2) {
		if (Func_1(Str_1_Par_Ref[Int_Loc], Str_2_Par_Ref[Int_Loc + 1]) == Ident_1) {
			Ch_Loc = 'A';
			Int_Loc += 1;
		}
	}
	if (Ch_Loc >= 'W' && Ch_Loc < 'Z')
		Int_Loc = 7;
	if (Ch_Loc == 'R')
		return 1;
	if (strcmp(Str_1_Par_Ref, Str_2_Par_Ref) > 0) {
		Int_Loc += 7;
		Int_Glob = Int_Loc;
		return 1;
	}
	return 0;
}

static int Func_3(Enumeration Enum_Par_Val)
{
	Enumeration Enum_Loc;

	Enum_Loc = Enum_Par_Val;
	return Enum_Loc == Ident_3;
}

/* final values of the main program locals, for the result check */
struct dhry_locals {
	int Int_1_Loc, Int_2_Loc, Int_3_Loc;
	Enumeration Enum_Loc;
	char Str_1_Loc[31], Str_2_Loc[31];
};

static void dhry_run(long Number_Of_Runs, struct dhry_locals *out)
{
	int Int_1_Loc = 0, Int_2_Loc = 0, Int_3_Loc = 0;
	char Ch_Index;
	Enumeration Enum_Loc = Ident_1;
	char Str_1_Loc[31], Str_2_Loc[31];
	long Run_Index;

	memset(dhry_rec, 0, sizeof(dhry_rec));
	memset(Arr_1_Glob, 0, sizeof(Arr_1_Glob));
	memset(Arr_2_Glob, 0, sizeof(Arr_2_Glob));
	Int_Glob = 0;
	Bool_Glob = 0;
	Ch_1_Glob = 0;
	Ch_2_Glob = 0;
	Str_2_Loc[0] = '\0';

	Next_Ptr_Glob = &dhry_rec[1];
	Ptr_Glob = &dhry_rec[0];

	Ptr_Glob->Ptr_Comp = Next_Ptr_Glob;
	Ptr_Glob->Discr = Ident_1;
	Ptr_Glob->variant.var_1.Enum_Comp = Ident_3;
	Ptr_Glob->variant.var_1.Int_Comp = 40;
	strcpy(Ptr_Glob->variant.var_1.Str_Comp, "DHRYSTONE PROGRAM, SOME STRING");
	strcpy(Str_1_Loc, "DHRYSTONE PROGRAM, 1'ST STRING");

	Arr_2_Glob[8][7] = 10;

	for (Run_Index = 1; Run_Index <= Number_Of_Runs; ++Run_Index) {
		Proc_5();
		Proc_4();
		Int_1_Loc = 2;
		Int_2_Loc = 3;
		strcpy(Str_2_Loc, "DHRYSTONE PROGRAM, 2'ND STRING");
		Enum_Loc = Ident_2;
		Bool_Glob = !Func_2(Str_1_Loc, Str_2_Loc);
		while (Int_1_Loc < Int_2_Loc) {
			Int_3_Loc = 5 * Int_1_Loc - Int_2_Loc;
			Proc_7(Int_1_Loc, Int_2_Loc, &Int_3_Loc);
			Int_1_Loc += 1;
		}
		Proc_8(Arr_1_Glob, Arr_2_Glob, Int_1_Loc, Int_3_Loc);
		Proc_1(Ptr_Glob);
		for (Ch_Index = 'A'; Ch_Index <= Ch_2_Glob; ++Ch_Index) {
			if (Enum_Loc == Func_1(Ch_Index, 'C')) {
				Proc_6(Ident_1, &Enum_Loc);
				strcpy(Str_2_Loc, "DHRYSTONE PROGRAM, 3'RD STRING");
				Int_2_Loc = Run_Index;
				Int_Glob = Run_Index;
			}
		}
		Int_2_Loc = Int_2_Loc * Int_1_Loc;
		Int_1_Loc = Int_2_Loc / Int_3_Loc;
		Int_2_Loc = 7 * (Int_2_Loc - Int_3_Loc) - Int_1_Loc;
		Proc_2(&Int_1_Loc);
	}

	out->Int_1_Loc = Int_1_Loc;
	out->Int_2_Loc = Int_2_Loc;
	out->Int_3_Loc = Int_3_Loc;
	out->Enum_Loc = Enum_Loc;
	strcpy(out->Str_1_Loc, Str_1_Loc);
	strcpy(out->Str_2_Loc, Str_2_Loc);
}

/* compare with the "should be" values of the original program */
static int dhry_check(long runs, const struct dhry_locals *l)
{
	static const char some[] = "DHRYSTONE PROGRAM, SOME STRING";
	int ok = 1;

#define DHRY_EXPECT(cond)						\
	do {								\
		if (!(cond)) {						\
			printf("  wrong: %s\n", #cond);			\
			ok = 0;						\
		}							\
	} while (0)

	DHRY_EXPECT(Int_Glob == 5);
	DHRY_EXPECT(Bool_Glob == 1);
	DHRY_EXPECT(Ch_1_Glob == 'A');
	DHRY_EXPECT(Ch_2_Glob == 'B');
	DHRY_EXPECT(Arr_1_Glob[8] == 7);
	DHRY_EXPECT(Arr_2_Glob[8][7] == runs + 10);
	DHRY_EXPECT(Ptr_Glob->Ptr_Comp == Next_Ptr_Glob->Ptr_Comp);
	DHRY_EXPECT(Ptr_Glob->Discr == Ident_1);
	DHRY_EXPECT(Ptr_Glob->variant.var_1.Enum_Comp == Ident_3);
	DHRY_EXPECT(Ptr_Glob->variant.var_1.Int_Comp == 17);
	DHRY_EXPECT(strcmp(Ptr_Glob->variant.var_1.Str_Comp, some) == 0);
	DHRY_EXPECT(Next_Ptr_Glob->Discr == Ident_1);
	DHRY_EXPECT(Next_Ptr_Glob->variant.var_1.Enum_Comp == Ident_2);
	DHRY_EXPECT(Next_Ptr_Glob->variant.var_1.Int_Comp == 18);
	DHRY_EXPECT(strcmp(Next_Ptr_Glob->variant.var_1.Str_Comp, some) == 0);
	DHRY_EXPECT(l->Int_1_Loc == 5);
	DHRY_EXPECT(l->Int_2_Loc == 13);
	DHRY_EXPECT(l->Int_3_Loc == 7);
	DHRY_EXPECT(l->Enum_Loc == Ident_2);
	DHRY_EXPECT(strcmp(l->Str_1_Loc, "DHRYSTONE PROGRAM, 1'ST STRING") == 0);
	DHRY_EXPECT(strcmp(l->Str_2_Loc, "DHRYSTONE PROGRAM, 2'ND STRING") == 0);

#undef DHRY_EXPECT
	return ok;
}

/*
 * Time sources, tried in this order:
 *   clock_gettime(CLOCK_MONOTONIC) through glibc (vDSO or system call)
 *   the clock_gettime system call directly
 *   the gettimeofday system call
 *   the raw stable counter (rdcntvl.w / rdcntvh.w) at the given MHz; on
 *   cpu7b it counts sys_clk cycles
 *
 * glibc 2.27 uses the 32-bit time system calls (clock_gettime,
 * nanosleep, clock_nanosleep). A 32-bit kernel only provides them with
 * CONFIG_COMPAT_32BIT_TIME=y; without it they fail with ENOSYS, so
 * clock_gettime and nanosleep do not work at all. gettimeofday is
 * always provided, so it is used as a fallback for both.
 */
enum { CLK_VDSO, CLK_SYSCALL, CLK_GTOD, CLK_COUNTER, NCLK };
static const char *const clk_name[NCLK] = {
	"clock_gettime (glibc)", "clock_gettime system call",
	"gettimeofday system call", "stable counter",
};
static int clk_src = CLK_VDSO;
static double clk_mhz = 75.0;

static uint64_t clk_counter(void)
{
#if MD_HAVE_ASM
	uint32_t hi, lo, hi2;

	do {
		__asm__ volatile("rdcntvh.w %0" : "=r"(hi));
		__asm__ volatile("rdcntvl.w %0" : "=r"(lo));
		__asm__ volatile("rdcntvh.w %0" : "=r"(hi2));
	} while (hi != hi2);
	return ((uint64_t)hi << 32) | lo;
#else
	return 0;	/* no inline asm: counter not available */
#endif
}

/* seconds from one time source; -1 if it is not available */
static double clk_read(int src)
{
	struct timespec ts = { 0, 0 };
	struct timeval tv = { 0, 0 };
	long r;

	switch (src) {
	case CLK_VDSO:
		r = clock_gettime(CLOCK_MONOTONIC, &ts);
		break;
	case CLK_SYSCALL:
#ifdef SYS_clock_gettime
		r = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
#else
		r = -1;
#endif
		break;
	case CLK_GTOD:
#ifdef SYS_gettimeofday
		r = syscall(SYS_gettimeofday, &tv, NULL);
#else
		r = gettimeofday(&tv, NULL);
#endif
		return r ? -1 : tv.tv_sec + tv.tv_usec / 1e6;
	default:
		return MD_HAVE_ASM ? clk_counter() / (clk_mhz * 1e6) : -1;
	}
	return r ? -1 : ts.tv_sec + ts.tv_nsec / 1e9;
}

static double dhry_now(void)
{
	return clk_read(clk_src);
}

static int sleep_errno;		/* errno of the last failed nanosleep, 0 if it worked */

/*
 * Sleep for ms milliseconds. Without CONFIG_COMPAT_32BIT_TIME nanosleep
 * fails at once with ENOSYS; then wait on gettimeofday, yielding the CPU.
 */
static void dhry_sleep_ms(long ms)
{
	struct timespec req = { ms / 1000, (ms % 1000) * 1000000L };
	double end;

	for (;;) {
		if (nanosleep(&req, &req) == 0) {
			sleep_errno = 0;
			return;
		}
		if (errno != EINTR)
			break;
	}
	sleep_errno = errno;

	end = clk_read(CLK_GTOD);
	if (end < 0) {
		/* no clock at all: a rough delay */
		for (volatile long i = 0; i < ms * 2000L; i++)
			;
		return;
	}
	end += ms / 1000.0;
	while (clk_read(CLK_GTOD) < end)
		sched_yield();
}

/* elapsed time of each source over a sleep of ms milliseconds */
static void clk_measure(long ms, double dt[NCLK])
{
	double t0[NCLK];
	int i;

	for (i = 0; i < NCLK; i++)
		t0[i] = clk_read(i);
	dhry_sleep_ms(ms);
	for (i = 0; i < NCLK; i++)
		dt[i] = (t0[i] < 0) ? -1 : clk_read(i) - t0[i];
}

/* pick the first source that advances over a 50 ms sleep */
static int clk_select(void)
{
	double dt[NCLK];

	clk_measure(50, dt);
	for (int i = 0; i < NCLK; i++) {
		if (dt[i] > 0.025) {
			clk_src = i;
			return 0;
		}
	}
	return -1;
}

/*
 * clocktest [MHz]: what each time source measures over a 1 s sleep.
 * nanosleep is timed by the kernel tick, independent of the sources; if
 * nanosleep fails, the sleep is timed with gettimeofday instead.
 */
static int cmd_clocktest(int argc, char **argv)
{
	double dt[NCLK];
	struct timespec ts = { 0, 0 };
	long r;
	uint64_t c0, c1;

	if (argc > 1 && strtod(argv[1], NULL) > 0)
		clk_mhz = strtod(argv[1], NULL);

	r = clock_gettime(CLOCK_MONOTONIC, &ts);
	printf("clock_gettime(CLOCK_MONOTONIC): ret %ld, %s, %ld.%09ld\n",
	       r, r ? strerror(errno) : "ok", (long)ts.tv_sec, (long)ts.tv_nsec);
#ifdef SYS_clock_gettime
	ts.tv_sec = ts.tv_nsec = 0;
	r = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
	printf("syscall clock_gettime:          ret %ld, %s, %ld.%09ld\n",
	       r, r ? strerror(errno) : "ok", (long)ts.tv_sec, (long)ts.tv_nsec);
#endif

	printf("measuring over a 1 s sleep...\n");
	fflush(stdout);
	c0 = clk_counter();
	clk_measure(1000, dt);
	c1 = clk_counter();
	if (sleep_errno)
		printf("  nanosleep failed: %s; the sleep was timed with gettimeofday\n",
		       strerror(sleep_errno));
	else
		printf("  nanosleep ok\n");
	for (int i = 0; i < NCLK; i++) {
		if (dt[i] < 0)
			printf("  %-27s not available\n", clk_name[i]);
		else
			printf("  %-27s %.6f s\n", clk_name[i], dt[i]);
	}
	if (MD_HAVE_ASM)
		printf("  stable counter: %llu ticks, %.3f MHz if the sleep was 1 s\n",
		       (unsigned long long)(c1 - c0), (c1 - c0) / 1e6);
	if (sleep_errno == ENOSYS)
		printf("  the kernel lacks the 32-bit time system calls: set CONFIG_COMPAT_32BIT_TIME=y\n");
	return 0;
}

#define DHRY_MAX_RUNS	100000000L	/* calibration limit, fits a 32-bit long */

/*
 * Short correctness run in a child process, so that a CPU that loops in
 * Proc_2() or Func_2() (wrong character compare or byte load/store) is
 * reported instead of hanging the shell. alarm()/setitimer() are not
 * available (CONFIG_POSIX_TIMERS is not set), so the parent polls.
 * Returns 0 ok, 1 wrong result, 2 did not finish in limit_ms.
 */
static int dhry_check_child(long runs, long limit_ms)
{
	pid_t pid;
	int st;

	fflush(stdout);
	pid = fork();
	if (pid < 0)
		return 1;
	if (pid == 0) {
		struct dhry_locals loc;

		dhry_run(runs, &loc);
		if (!dhry_check(runs, &loc)) {
			fflush(stdout);
			_exit(1);
		}
		_exit(0);
	}
	for (long waited = 0; waited < limit_ms; waited += 10) {
		if (waitpid(pid, &st, WNOHANG) == pid)
			return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : 1;
		dhry_sleep_ms(10);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
	return 2;
}

static int cmd_dhrystone(int argc, char **argv)
{
	long runs = argc > 1 ? atol(argv[1]) : 0;
	double mhz = argc > 2 ? strtod(argv[2], NULL) : 75.0;
	struct dhry_locals loc;
	double t0, t, dps, dmips;
	int ok;

	if (mhz <= 0)
		mhz = 75.0;
	clk_mhz = mhz;

	/* correctness first: 1 and 10 runs take microseconds */
	{
		static const long quick[] = { 1, 10 };

		for (size_t i = 0; i < sizeof(quick) / sizeof(quick[0]); i++) {
			int r = dhry_check_child(quick[i], 5000);

			if (r == 2) {
				printf("dhrystone: %ld run(s) did not finish in 5 s: the CPU loops in Proc_2 or Func_2 (character compare, byte load/store); run mdtest\n",
				       quick[i]);
				return 1;
			}
			if (r) {
				printf("dhrystone: wrong result after %ld run(s), see the lines above; run mdtest\n",
				       quick[i]);
				return 1;
			}
		}
	}

	if (clk_select() < 0) {
		printf("dhrystone: no time source advances over a 50 ms sleep; run clocktest\n");
		return 1;
	}
	printf("dhrystone: timing with %s%s\n", clk_name[clk_src],
	       clk_src == CLK_COUNTER ? " (assumes it counts at the given MHz)" : "");

	if (runs > 0) {
		t0 = dhry_now();
		dhry_run(runs, &loc);
		t = dhry_now() - t0;
	} else {
		/* calibrate to about 2 s; 3-5 s in total */
		printf("dhrystone: calibrating to about 2 s\n");
		runs = 1000;
		for (;;) {
			double next;

			t0 = dhry_now();
			dhry_run(runs, &loc);
			t = dhry_now() - t0;
			printf("  %ld runs: %.3f s\n", runs, t);
			fflush(stdout);
			if (t >= 2.0 || runs >= DHRY_MAX_RUNS)
				break;
			/* grow by at most 10x per step, so a coarse clock cannot overshoot */
			next = t < 0.2 ? runs * 10.0 : runs * (2.2 / t);
			runs = next > DHRY_MAX_RUNS ? DHRY_MAX_RUNS : (long)next;
		}
	}

	printf("Dhrystone 2.1: %ld runs in %.3f s\n", runs, t);
	ok = dhry_check(runs, &loc);
	if (t <= 0) {
		printf("  time too short to measure\n");
		return 1;
	}
	if (t < 1.0)
		printf("  less than 1 s measured, use more runs for an accurate result\n");

	dps = runs / t;
	dmips = dps / 1757.0;
	printf("  %.0f Dhrystones/s, %.2f us per run\n", dps, 1e6 * t / runs);
	printf("  %.2f DMIPS, %.3f DMIPS/MHz at %.1f MHz\n", dmips, dmips / mhz, mhz);
	printf("  results %s\n", ok ? "ok" : "WRONG");
	fflush(stdout);
	return !ok;
}

/* ------------------------------------------------------------------ */
/* mdtest: multiply, divide and byte/halfword load/store               */
/* ------------------------------------------------------------------ */

/*
 * mdtest [n]
 *
 * Runs each instruction on special values and n random operand pairs
 * (default 100000) and compares with a reference computed in software
 * with shifts and adds only, so the reference does not depend on the
 * hardware multiplier or divider.
 *
 *   mul.w mulh.w mulh.wu      32x32 -> low / high 32 bits
 *   div.w div.wu mod.w mod.wu  divisor 0 and INT_MIN / -1 are skipped
 *                              (undefined in LoongArch)
 *   result used by the next instruction (bypass after mul / div)
 *   ld.b ld.bu ld.h ld.hu st.b st.h at every offset
 *
 * On LoongArch the instructions are forced with inline asm; on other
 * hosts the C operators are used, which only checks the reference code.
 */

#if MD_HAVE_ASM
#define MD_ASM3(insn, a, b) ({						\
	uint32_t __r;							\
	__asm__ volatile(insn " %0, %1, %2" : "=r"(__r) : "r"(a), "r"(b));\
	__r;								\
})
static uint32_t hw_mul_w(uint32_t a, uint32_t b)    { return MD_ASM3("mul.w", a, b); }
static uint32_t hw_mulh_w(uint32_t a, uint32_t b)   { return MD_ASM3("mulh.w", a, b); }
static uint32_t hw_mulh_wu(uint32_t a, uint32_t b)  { return MD_ASM3("mulh.wu", a, b); }
static uint32_t hw_div_w(uint32_t a, uint32_t b)    { return MD_ASM3("div.w", a, b); }
static uint32_t hw_div_wu(uint32_t a, uint32_t b)   { return MD_ASM3("div.wu", a, b); }
static uint32_t hw_mod_w(uint32_t a, uint32_t b)    { return MD_ASM3("mod.w", a, b); }
static uint32_t hw_mod_wu(uint32_t a, uint32_t b)   { return MD_ASM3("mod.wu", a, b); }

/* result consumed by the very next instruction */
static uint32_t hw_mul_use(uint32_t a, uint32_t b)
{
	uint32_t r, s;

	__asm__ volatile("mul.w %0, %2, %3\n\tadd.w %1, %0, %0"
			 : "=&r"(r), "=r"(s) : "r"(a), "r"(b));
	(void)r;
	return s;
}

static uint32_t hw_div_use(uint32_t a, uint32_t b)
{
	uint32_t r, s;

	__asm__ volatile("div.wu %0, %2, %3\n\tadd.w %1, %0, %0"
			 : "=&r"(r), "=r"(s) : "r"(a), "r"(b));
	(void)r;
	return s;
}
#else
static uint32_t hw_mul_w(uint32_t a, uint32_t b)    { return a * b; }
static uint32_t hw_mulh_w(uint32_t a, uint32_t b)   { return (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 32); }
static uint32_t hw_mulh_wu(uint32_t a, uint32_t b)  { return (uint32_t)(((uint64_t)a * b) >> 32); }
static uint32_t hw_div_w(uint32_t a, uint32_t b)    { return (uint32_t)((int32_t)a / (int32_t)b); }
static uint32_t hw_div_wu(uint32_t a, uint32_t b)   { return a / b; }
static uint32_t hw_mod_w(uint32_t a, uint32_t b)    { return (uint32_t)((int32_t)a % (int32_t)b); }
static uint32_t hw_mod_wu(uint32_t a, uint32_t b)   { return a % b; }
static uint32_t hw_mul_use(uint32_t a, uint32_t b)  { return 2 * (a * b); }
static uint32_t hw_div_use(uint32_t a, uint32_t b)  { return 2 * (a / b); }
#endif

/* reference: unsigned 32x32 -> 64 with shifts and adds */
static uint64_t ref_mulu64(uint32_t a, uint32_t b)
{
	uint64_t acc = 0, x = a;

	for (int i = 0; i < 32; i++) {
		if (b & 1)
			acc += x;
		x <<= 1;
		b >>= 1;
	}
	return acc;
}

static uint64_t ref_muls64(uint32_t a, uint32_t b)
{
	uint64_t p = ref_mulu64(a, b);

	/* signed product = unsigned product - (a<0 ? b<<32) - (b<0 ? a<<32) */
	if ((int32_t)a < 0)
		p -= (uint64_t)b << 32;
	if ((int32_t)b < 0)
		p -= (uint64_t)a << 32;
	return p;
}

/* reference: restoring division, b != 0 */
static void ref_divu(uint32_t a, uint32_t b, uint32_t *q, uint32_t *r)
{
	uint32_t quo = 0, rem = 0;

	for (int i = 31; i >= 0; i--) {
		int carry = rem >> 31;

		rem = (rem << 1) | ((a >> i) & 1);
		if (carry || rem >= b) {
			rem -= b;
			quo |= 1u << i;
		}
	}
	*q = quo;
	*r = rem;
}

/* signed: quotient rounded toward zero, remainder has the sign of a */
static void ref_divs(uint32_t a, uint32_t b, uint32_t *q, uint32_t *r)
{
	int neg_a = (int32_t)a < 0, neg_b = (int32_t)b < 0;
	uint32_t ua = neg_a ? 0u - a : a, ub = neg_b ? 0u - b : b, uq, ur;

	ref_divu(ua, ub, &uq, &ur);
	*q = (neg_a != neg_b) ? 0u - uq : uq;
	*r = neg_a ? 0u - ur : ur;
}

static uint32_t md_seed = 0x12345678;

static uint32_t md_rand(void)
{
	static const uint32_t special[] = {
		0, 1, 2, 3, 7, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000,
		0x7fffffff, 0x80000000, 0x80000001, 0xfffffffe, 0xffffffff,
		0x55555555, 0xaaaaaaaa, 0x12345678, 0xedcba987,
	};

	md_seed = md_seed * 1664525u + 1013904223u;
	switch (md_seed >> 29) {
	case 0:
		return special[(md_seed >> 8) % (sizeof(special) / sizeof(special[0]))];
	case 1:
		return (md_seed >> 8) & 0xff;		/* small */
	case 2:
		return 0u - ((md_seed >> 8) & 0xff);	/* small negative */
	default:
		md_seed = md_seed * 1664525u + 1013904223u;
		return md_seed ^ (md_seed >> 13);
	}
}

struct md_count {
	const char *name;
	long bad;
};

static void md_report(struct md_count *c, uint32_t a, uint32_t b,
		      uint32_t got, uint32_t exp)
{
	if (c->bad < 5)
		printf("  %-8s %08x, %08x: got %08x, expected %08x\n",
		       c->name, a, b, got, exp);
	c->bad++;
}

static int md_bytes(void)
{
	volatile union {
		uint8_t b[16];
		uint16_t h[8];
		uint32_t w[4];
	} buf;
	int bad = 0;

	for (int off = 0; off < 8; off++) {
		for (int v = 0; v < 256; v += 0x35) {
			uint8_t val = (uint8_t)(v ^ 0x80);
			int32_t sb;
			uint32_t ub;

			for (int i = 0; i < 4; i++)
				buf.w[i] = 0xa5a5a5a5;
#if MD_HAVE_ASM
			{
				volatile uint8_t *p = &buf.b[off];

				__asm__ volatile("st.b %0, %1, 0" :: "r"((uint32_t)val), "r"(p) : "memory");
				__asm__ volatile("ld.b %0, %1, 0" : "=r"(sb) : "r"(p) : "memory");
				__asm__ volatile("ld.bu %0, %1, 0" : "=r"(ub) : "r"(p) : "memory");
			}
#else
			buf.b[off] = val;
			sb = (int8_t)buf.b[off];
			ub = buf.b[off];
#endif
			if (sb != (int8_t)val || ub != val) {
				if (bad < 5)
					printf("  byte off %d val %02x: ld.b %08x ld.bu %08x\n",
					       off, val, (uint32_t)sb, ub);
				bad++;
			}
			/* neighbours must not change */
			for (int i = 0; i < 16; i++) {
				if (i != off + 0 && buf.b[i] != 0xa5) {
					if (bad < 5)
						printf("  st.b off %d changed byte %d to %02x\n",
						       off, i, buf.b[i]);
					bad++;
				}
			}
		}
	}

	for (int off = 0; off < 8; off += 2) {
		uint16_t val = 0x8a5c;
		int32_t sh;
		uint32_t uh;

		for (int i = 0; i < 4; i++)
			buf.w[i] = 0x5a5a5a5a;
#if MD_HAVE_ASM
		{
			volatile uint8_t *p = &buf.b[off];

			__asm__ volatile("st.h %0, %1, 0" :: "r"((uint32_t)val), "r"(p) : "memory");
			__asm__ volatile("ld.h %0, %1, 0" : "=r"(sh) : "r"(p) : "memory");
			__asm__ volatile("ld.hu %0, %1, 0" : "=r"(uh) : "r"(p) : "memory");
		}
#else
		buf.h[off / 2] = val;
		sh = (int16_t)buf.h[off / 2];
		uh = buf.h[off / 2];
#endif
		if (sh != (int16_t)val || uh != val) {
			if (bad < 10)
				printf("  half off %d: ld.h %08x ld.hu %08x\n",
				       off, (uint32_t)sh, uh);
			bad++;
		}
	}
	return bad;
}

static int cmd_mdtest(int argc, char **argv)
{
	long n = argc > 1 ? atol(argv[1]) : 100000;
	struct md_count c[] = {
		{ "mul.w" }, { "mulh.w" }, { "mulh.wu" },
		{ "div.w" }, { "div.wu" }, { "mod.w" }, { "mod.wu" },
		{ "mul+use" }, { "div+use" },
	};
	long total = 0;
	int bbad;

	printf("mdtest: %ld random operand pairs, %s\n", n,
	       MD_HAVE_ASM ? "instructions forced with inline asm"
			   : "WARNING: no LoongArch macro, testing C operators only");
	fflush(stdout);

	for (long i = 0; i < n; i++) {
		uint32_t a = md_rand(), b = md_rand();
		uint64_t pu = ref_mulu64(a, b), ps = ref_muls64(a, b);
		uint32_t q, r, got;

		if ((got = hw_mul_w(a, b)) != (uint32_t)pu)
			md_report(&c[0], a, b, got, (uint32_t)pu);
		if ((got = hw_mulh_w(a, b)) != (uint32_t)(ps >> 32))
			md_report(&c[1], a, b, got, (uint32_t)(ps >> 32));
		if ((got = hw_mulh_wu(a, b)) != (uint32_t)(pu >> 32))
			md_report(&c[2], a, b, got, (uint32_t)(pu >> 32));
		if ((got = hw_mul_use(a, b)) != (uint32_t)pu * 2)
			md_report(&c[7], a, b, got, (uint32_t)pu * 2);

		if (b == 0)
			continue;
		ref_divu(a, b, &q, &r);
		if ((got = hw_div_wu(a, b)) != q)
			md_report(&c[4], a, b, got, q);
		if ((got = hw_mod_wu(a, b)) != r)
			md_report(&c[6], a, b, got, r);
		if ((got = hw_div_use(a, b)) != q * 2)
			md_report(&c[8], a, b, got, q * 2);

		if (a == 0x80000000u && b == 0xffffffffu)
			continue;
		ref_divs(a, b, &q, &r);
		if ((got = hw_div_w(a, b)) != q)
			md_report(&c[3], a, b, got, q);
		if ((got = hw_mod_w(a, b)) != r)
			md_report(&c[5], a, b, got, r);
	}

	for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
		printf("  %-8s %s", c[i].name, c[i].bad ? "" : "ok\n");
		if (c[i].bad)
			printf("%ld wrong\n", c[i].bad);
		total += c[i].bad;
	}
	bbad = md_bytes();
	printf("  bytes    %s", bbad ? "" : "ok\n");
	if (bbad)
		printf("%d wrong\n", bbad);
	total += bbad;

	printf("mdtest: %s\n", total ? "FAIL" : "PASS");
	return total != 0;
}

/* ------------------------------------------------------------------ */
/* cpiperf: cycles per instruction by instruction type                */
/* ------------------------------------------------------------------ */

/*
 * cpiperf [loops]
 *
 * Runs 32 copies of one instruction in a loop and divides the elapsed
 * stable-counter ticks by the number of instructions. On cpu7b the
 * counter counts sys_clk cycles, so the result is in CPU cycles. The
 * loop branch adds 2 instructions per 32 (about 6%); timer interrupts
 * add a little noise.
 *
 * What the numbers show:
 *   nop / add.w     instruction fetch: about 1-3 with the icache on,
 *                   tens of cycles if every fetch goes to SDRAM
 *   ld.w / st.w     a load or store: there is no data cache, so each
 *                   one is an AXI access to SDRAM
 *   b               a taken branch (redirect at W plus refetch)
 *   mul.w / div.w   multiplier and divider latency
 */
#if MD_HAVE_ASM
#define CPI_LOOP(insn)							\
	__asm__ volatile(						\
		"9:\n"							\
		".rept 32\n"						\
		insn "\n"						\
		".endr\n"						\
		"addi.w %0, %0, -1\n"					\
		"bnez   %0, 9b\n"					\
		: "+r"(n)						\
		: "r"(p), "r"(v1), "r"(v2)				\
		: "$r12", "memory")

static double cpi_one(int kind, long loops)
{
	static volatile uint32_t cpi_buf[16] __attribute__((aligned(64)));
	volatile uint32_t *p = cpi_buf;
	uint32_t v1 = 0x12345, v2 = 7;
	long n = loops;
	uint64_t c0, c1;

	c0 = clk_counter();
	switch (kind) {
	case 0: CPI_LOOP("nop"); break;
	case 1: CPI_LOOP("add.w $r12, %2, %3"); break;
	case 2: CPI_LOOP("ld.w $r12, %1, 0"); break;
	case 3: CPI_LOOP("st.w %2, %1, 0"); break;
	case 4: CPI_LOOP("b 1f\n1:"); break;
	case 5: CPI_LOOP("mul.w $r12, %2, %3"); break;
	case 6: CPI_LOOP("div.w $r12, %2, %3"); break;
	}
	c1 = clk_counter();
	return (double)(c1 - c0) / ((double)loops * 32);
}

static int cmd_cpiperf(int argc, char **argv)
{
	static const char *const name[] = {
		"nop", "add.w", "ld.w", "st.w", "b (taken)", "mul.w", "div.w",
	};
	long loops = argc > 1 ? atol(argv[1]) : 2000;

	if (loops < 1)
		loops = 1;
	printf("cpiperf: %ld loops x 32 instructions, cycles per instruction\n", loops);
	for (int k = 0; k < 7; k++) {
		cpi_one(k, 10);		/* warm up: TLB, icache, page faults */
		printf("  %-10s %8.2f\n", name[k], cpi_one(k, loops));
		fflush(stdout);
	}
	return 0;
}
#else
static int cmd_cpiperf(int argc, char **argv)
{
	printf("cpiperf: needs LoongArch inline asm\n");
	return 1;
}
#endif

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
	{ "memtest",  cmd_memtest,  "memtest [rounds]: store/load check, fresh pages and fork" },
	{ "snp",      cmd_snp,      "one snprintf in this process" },
	{ "snptest",  cmd_snptest,  "snptest [n]: first snprintf in n fresh processes" },
	{ "fartest",  cmd_fartest,  "fartest [n]: call into an unmapped code page in n fresh processes" },
	{ "dhrystone", cmd_dhrystone, "dhrystone [runs] [MHz]: Dhrystone 2.1 benchmark" },
	{ "clocktest", cmd_clocktest, "clocktest [MHz]: compare the time sources over a 1 s sleep" },
	{ "cpiperf",  cmd_cpiperf,  "cpiperf [loops]: cycles per instruction for nop/add/ld/st/b/mul/div" },
	{ "mdtest",   cmd_mdtest,   "mdtest [n]: check mul/div/mod and byte/halfword load/store" },
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

	/* child of snptest: only the first snprintf of a fresh process */
	if (argc > 1 && strcmp(argv[1], "--snptest") == 0)
		return snp_once();
	/* child of fartest: first call into far1/far2 */
	if (argc > 1 && strcmp(argv[1], "--fartest") == 0)
		return far_once();

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
