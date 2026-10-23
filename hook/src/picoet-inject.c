// picoet-inject -- minimal arm64 ptrace injector (P0, dry-run).
//
// Contract (design HOOK-DUAL-DESIGN-2026-10-02 section 4.2):
//   * runs as root on the device; never loads or copies any PICO byte
//   * seizes/interrupts every target thread, saves executor GPR and FPSIMD state
//   * resolves dlopen()/mmap() in the target by module-base + local offset,
//     and refuses if the module full paths differ or the address is not in an
//     executable mapping
//   * plants a temporary `brk #0` sentinel in an executable page
//   * remote-calls mmap(RW) to obtain a scratch buffer for the .so path
//   * remote-calls dlopen(path, RTLD_NOW); our payload constructor runs
//   * restores the sentinel bytes and the registers, detaches every thread
//
// Failure safety (黄金律: "not injected" is fine, "service broken" is not):
//   * attach is all-or-nothing: any non-ESRCH failure is fatal
//   * only a verified sentinel return permits restoration/resumption. An interrupted
//     remote call terminates the target: rewinding PC cannot unwind linker locks
//   * the sentinel is restored through any still-attached thread, so losing
//     the executor does not leave code patched
//   * SIGINT/SIGTERM/SIGHUP/SIGQUIT are blocked across the critical window
//   * PTRACE_O_EXITKILL prevents an abruptly lost injector from resuming an
//     executor in a half-finished remote call
//
// Usage: picoet-inject <pid> <absolute-path-to-libpicoet_hook.so> [--any]
//   --any   skip the "target looks like pxreyetrackingservice" sanity check
//
// Exit: 0 = dlopen returned a non-null handle (or an executable payload mapping
//           already exists); hook readiness is reported separately by the payload;
//       2 = injection failed;
//       3 = temporary signal/stop or no safe user-instruction executor: retry later;
//       4 = stale partial mapping of the payload present: restart the target and retry.
//       5 = unsafe remote/restoration state: target terminated; recover once on a fresh PID.
//       6 = --run-locked control command is busy (no injection attempted).

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <dlfcn.h>
#include <time.h>
#include <asm/ptrace.h>
#include <sys/file.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/xattr.h>
#include <elf.h>
#include "root_file.h"

#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif

#define MAX_TIDS 4096
#define REMOTE_TIMEOUT_MS 5000
#define ATTACH_TIMEOUT_MS 2000

// Small root-only file helpers keep shell redirections away from replaceable
// /data/local/tmp paths. Public paths and file formats remain unchanged.
static int read_control_file(const char *path, int log_tail)
{
    int fd;
    const char *prefix = "/data/local/tmp/picoet-runtime/";
    if (!strncmp(path, prefix, strlen(prefix))) {
        const char *name = path + strlen(prefix);
        if (!*name || strchr(name, '/')) return 2;
        int dir = picoet_open_runtime();
        if (dir < 0) return 2;
        fd = picoet_root_file(openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        close(dir);
    } else {
        fd = picoet_open_root_file(path, O_RDONLY, 0);
    }
    if (fd < 0) return 2;
    char buf[4096];
    size_t cap = log_tail ? sizeof(buf) : 256;
    if (log_tail) {
        off_t end = lseek(fd, 0, SEEK_END);
        if (end < 0 || lseek(fd, end > (off_t)cap ? end - cap : 0, SEEK_SET) < 0) {
            close(fd);
            return 2;
        }
    }
    ssize_t n = read(fd, buf, cap);
    close(fd);
    if (n < 0 || (!log_tail && n == (ssize_t)cap)) return 2;
    return write(STDOUT_FILENO, buf, (size_t)n) == n ? 0 : 2;
}

static int save_setting(const char *path, const char *value)
{
    static const char context[] = "u:object_r:shell_data_file:s0";
    int existing = picoet_open_root_file(path, O_RDONLY, 0);
    if (existing >= 0) {
        char current[16], label[64];
        ssize_t n = read(existing, current, sizeof(current));
        ssize_t label_len = fgetxattr(existing, "security.selinux", label, sizeof(label));
        close(existing);
        if (n == (ssize_t)strlen(value) && !memcmp(current, value, (size_t)n) &&
            label_len == (ssize_t)sizeof(context) && !memcmp(label, context, sizeof(context))) return 0;
    }
    char tmp[] = "/data/adb/picoet-enhance/setting.XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) return 2;
    size_t len = strlen(value);
    // rename preserves the source inode's label. A temp created under /data/adb
    // otherwise keeps adb_data_file and becomes unreadable to the enforcing
    // payload at its legacy /data/local/tmp destination.
    int rc = fsetxattr(fd, "security.selinux", context, sizeof(context), 0) == 0 &&
             fchmod(fd, 0644) == 0 && write(fd, value, len) == (ssize_t)len ? 0 : 2;
    if (close(fd) != 0) rc = 2;
    if (!rc && rename(tmp, path) != 0) rc = 2;
    if (rc) unlink(tmp);
    return rc;
}

static int prepare_state(void)
{
    const char *state = "/data/adb/picoet-enhance";
    if (mkdir(state, 0700) != 0 && errno != EEXIST) return 2;
    struct stat st;
    if (lstat(state, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0022))
        return 2;
    return 0;
}

static int save_mode(const char *mode)
{
    if (strcmp(mode, "off") && strcmp(mode, "dual")) return 2;
    if (prepare_state()) return 2;
    if (save_setting("/data/local/tmp/picoet-mode", !strcmp(mode, "off") ? "off\n" : "dual\n"))
        return 2;
    return save_setting("/data/local/tmp/picoet-gate", !strcmp(mode, "off") ? "off\n" : "on\n");
}

static int prepare_runtime(int reset)
{
    if (mkdir("/data/local/tmp/picoet-runtime", 0700) != 0 && errno != EEXIST) return 2;
    int fd = picoet_open_runtime();
    if (fd < 0) return 2;
    if (!reset) { close(fd); return 0; }
    DIR *dir = fdopendir(fd);
    if (!dir) { close(fd); return 2; }
    int rc = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            if (errno) rc = 2;
            break;
        }
        const char *p = entry->d_name;
        while (*p >= '0' && *p <= '9') ++p;
        if (strcmp(entry->d_name, "unsafe") &&
            (p == entry->d_name || (strcmp(p, ".state") && strcmp(p, ".tmp")))) continue;
        if (unlinkat(fd, entry->d_name, 0) != 0 && errno != ENOENT) rc = 2;
    }
    if (closedir(dir) != 0) rc = 2;
    return rc;
}

// The same lock covers watchdog/manual operations and installer normalization.
// Do not use O_CLOEXEC: --run-locked deliberately passes this lease to the shell.
static int control_lock(const char *path, int wait_for_lock)
{
    int fd = picoet_root_file(open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0600));
    if (fd < 0) {
        fprintf(stderr, "[-] cannot open control lock: %s\n", strerror(errno));
        return -2;
    }
    for (int n = 0; flock(fd, LOCK_EX | LOCK_NB) != 0; n++) {
        if (errno == EWOULDBLOCK && wait_for_lock && n < 300) {
            usleep(100000);
            continue;
        }
        int rc = errno == EWOULDBLOCK ? 6 : 2;
        if (wait_for_lock || rc != 6)
            fprintf(stderr, "[-] control lock unavailable: %s\n", strerror(errno));
        close(fd);
        return -rc;
    }
    return fd;
}

static int normalize_mode(void)
{
    if (prepare_state()) return 2;
    int lock = control_lock("/data/adb/picoet-enhance/control.lock", 1);
    if (lock < 0) return -lock;
    char value[256];
    // Published releases wrote root-owned 0666 settings. Import only their
    // disabled state here, then save_mode replaces them with protected files.
    // Runtime reads remain strict; legacy bytes can only select off, never a
    // new active capability. Still reject links, foreign owners and devices.
    int fd = open("/data/local/tmp/picoet-mode", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    ssize_t n = -1;
    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 && st.st_nlink == 1)
        n = read(fd, value, sizeof(value));
    if (fd >= 0) close(fd);
    if (n == (ssize_t)sizeof(value)) n = -1;
    while (n > 0 && value[n - 1] == '\n') --n;
    const char *mode = n == 3 && !memcmp(value, "off", 3) ? "off" : "dual";
    int rc = save_mode(mode);
    if (!rc && puts(mode) < 0) rc = 2;
    close(lock);
    return rc;
}

struct arm64_regs {
    unsigned long long regs[31]; /* x0..x30 */
    unsigned long long sp;
    unsigned long long pc;
    unsigned long long pstate;
};

struct maps_entry {
    unsigned long long start;
    unsigned long long end;
    char perms[8];
    unsigned long long off;
    char path[768];
};

static struct maps_entry g_entries[8192];
static int g_nentries;

static char *read_file(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(fd);
        return NULL;
    }
    for (;;) {
        if (len + 4096 > cap) {
            size_t ncap = cap * 2;
            char *nb = realloc(buf, ncap);
            if (!nb) {
                free(buf);
                close(fd);
                return NULL;
            }
            buf = nb;
            cap = ncap;
        }
        ssize_t r = read(fd, buf + len, cap - len - 1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return NULL;
        }
        if (r == 0)
            break;
        len += (size_t)r;
    }
    buf[len] = '\0';
    close(fd);
    return buf;
}

static int parse_maps(const char *maps)
{
    g_nentries = 0;
    const char *p = maps;
    while (p && *p && g_nentries < (int)(sizeof(g_entries) / sizeof(g_entries[0]))) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        char line[1200];
        if (linelen < sizeof(line)) {
            memcpy(line, p, linelen);
            line[linelen] = '\0';
            unsigned long long s = 0, e = 0, off = 0;
            char perms[8] = {0};
            char path[768] = {0};
            int n = sscanf(line, "%llx-%llx %7s %llx %*s %*s %767s",
                           &s, &e, perms, &off, path);
            if (n >= 3) {
                struct maps_entry *me = &g_entries[g_nentries++];
                me->start = s;
                me->end = e;
                memset(me->perms, 0, sizeof(me->perms));
                memcpy(me->perms, perms, sizeof(me->perms) - 1);
                me->perms[sizeof(me->perms) - 1] = '\0';
                me->off = off;
                snprintf(me->path, sizeof(me->path), "%s", path);
            }
        }
        p = eol ? eol + 1 : NULL;
    }
    return g_nentries;
}

static int basename_eq(const char *path, const char *name)
{
    const char *bn = strrchr(path, '/');
    bn = bn ? bn + 1 : path;
    return strcmp(bn, name) == 0;
}

static unsigned long long find_module_base(const char *modname, char *out_path, size_t out_sz)
{
    unsigned long long base = 0;
    for (int i = 0; i < g_nentries; i++) {
        struct maps_entry *me = &g_entries[i];
        if (me->off != 0 || !me->path[0])
            continue;
        if (!basename_eq(me->path, modname))
            continue;
        if (base == 0 || me->start < base) {
            base = me->start;
            if (out_path && out_sz)
                snprintf(out_path, out_sz, "%s", me->path);
        }
    }
    return base;
}

static int in_exec_map(unsigned long long addr)
{
    for (int i = 0; i < g_nentries; i++) {
        struct maps_entry *me = &g_entries[i];
        if (strchr(me->perms, 'x') && addr >= me->start && addr < me->end)
            return 1;
    }
    return 0;
}

static int pc_in_linker(unsigned long long pc)
{
    for (int i = 0; i < g_nentries; i++) {
        struct maps_entry *me = &g_entries[i];
        if (pc < me->start || pc >= me->end)
            continue;
        if (strstr(me->path, "linker") || basename_eq(me->path, "libdl.so"))
            return 1;
    }
    return 0;
}

// Pick an executable address to hold a temporary `brk #0`.
// Prefer a readable+executable mapping (execute-only pages may refuse PEEK),
// and among those prefer the phoenix plugin / the target executable.
// NOTE: on arm64 the kernel's copy_to_user_page() flushes the I-cache for
// VM_EXEC pages, so a ptrace-poked instruction is coherent.
static int find_sentinel(unsigned long long *out, char *desc, size_t dsz)
{
    unsigned long long best_rx = 0, best_x = 0;
    char best_rx_desc[256] = {0}, best_x_desc[256] = {0};
    for (int i = 0; i < g_nentries; i++) {
        struct maps_entry *me = &g_entries[i];
        if (!strchr(me->perms, 'x'))
            continue;
        unsigned long long addr = (me->end - 8) & ~7ULL;
        if (strchr(me->perms, 'r')) {
            if (best_rx == 0) {
                best_rx = addr;
                snprintf(best_rx_desc, sizeof(best_rx_desc), "%s", me->path[0] ? me->path : "[anon-x]");
            }
            if (strstr(me->path, "phoenix") || strstr(me->path, "pxreyetrackingservice")) {
                *out = addr;
                snprintf(desc, dsz, "%s", me->path);
                return 0;
            }
        } else if (best_x == 0) {
            best_x = addr;
            snprintf(best_x_desc, sizeof(best_x_desc), "%s", me->path[0] ? me->path : "[anon-x]");
        }
    }
    if (best_rx) {
        *out = best_rx;
        snprintf(desc, dsz, "%s", best_rx_desc);
        return 0;
    }
    if (best_x) {
        *out = best_x;
        snprintf(desc, dsz, "%s", best_x_desc);
        return 0;
    }
    return -1;
}

// Resolve a symbol's module basename + full path + offset within our own
// process, so the same module in the target can be computed as base + offset.
static int resolve_local(const char *sym, char *modname, size_t mn,
                         char *modpath, size_t mp, unsigned long long *off)
{
    void *s = dlsym(RTLD_DEFAULT, sym);
    if (!s)
        return -1;
    Dl_info di;
    if (!dladdr(s, &di) || !di.dli_fname)
        return -1;
    const char *bn = strrchr(di.dli_fname, '/');
    bn = bn ? bn + 1 : di.dli_fname;
    snprintf(modname, mn, "%s", bn);
    snprintf(modpath, mp, "%s", di.dli_fname);
    unsigned long long a = (unsigned long long)s;
    unsigned long long b = (unsigned long long)di.dli_fbase;
    if (a < b)
        return -1;
    *off = a - b;
    return 0;
}

static int get_regs(pid_t tid, struct arm64_regs *r)
{
    struct iovec iov = { r, sizeof(*r) };
    if (ptrace(PTRACE_GETREGSET, tid, (void *)(unsigned long)NT_PRSTATUS, &iov) < 0)
        return -1;
    if (iov.iov_len != sizeof(*r)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int set_regs(pid_t tid, const struct arm64_regs *r)
{
    struct iovec iov = { (void *)r, sizeof(*r) };
    return ptrace(PTRACE_SETREGSET, tid, (void *)(unsigned long)NT_PRSTATUS, &iov) < 0 ? -1 : 0;
}

static int get_fpsimd(pid_t tid, struct user_fpsimd_state *r)
{
    struct iovec iov = { r, sizeof(*r) };
    if (ptrace(PTRACE_GETREGSET, tid, (void *)(unsigned long)NT_FPREGSET, &iov) < 0)
        return -1;
    if (iov.iov_len != sizeof(*r)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int set_fpsimd(pid_t tid, const struct user_fpsimd_state *r)
{
    struct iovec iov = { (void *)r, sizeof(*r) };
    return ptrace(PTRACE_SETREGSET, tid, (void *)(unsigned long)NT_FPREGSET, &iov) < 0 ? -1 : 0;
}

static int peek_mem(pid_t tid, unsigned long long addr, unsigned long long *out)
{
    errno = 0;
    long v = ptrace(PTRACE_PEEKDATA, tid, (void *)(unsigned long)addr, NULL);
    if (v == -1 && errno != 0)
        return -1;
    *out = (unsigned long long)v;
    return 0;
}

static int poke_mem(pid_t tid, unsigned long long addr, unsigned long long val)
{
    return ptrace(PTRACE_POKEDATA, tid, (void *)(unsigned long)addr,
                  (void *)(unsigned long)val) < 0 ? -1 : 0;
}

// Wait for `tid` to stop, bounded by `timeout_ms`.
// returns 0 = stopped (*status_out valid), 1 = timeout, -1 = error.
static int wait_stop(pid_t tid, int *status_out, int timeout_ms)
{
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
        return -1;
    for (;;) {
        int status = 0;
        pid_t r = waitpid(tid, &status, WNOHANG | __WALL);
        if (r == tid) {
            *status_out = status;
            return 0;
        }
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            return -1;
        long long elapsed = (now.tv_sec - start.tv_sec) * 1000LL +
                            (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= timeout_ms)
            return 1;
        usleep(1000);
    }
}

static long long monotonic_ms(void)
{
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) == 0 ? t.tv_sec * 1000LL + t.tv_nsec / 1000000 : -1;
}

static unsigned long long start_ticks(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    char *stat = read_file(path);
    if (!stat) return 0;
    char *p = strrchr(stat, ')');
    if (p && p[1] == ' ') {
        p += 2;
        for (int f = 3; f < 22 && p; ++f) {
            p = strchr(p, ' ');
            if (p) ++p;
        }
    } else p = NULL;
    unsigned long long value = p ? strtoull(p, NULL, 10) : 0;
    free(stat);
    return value;
}

static int target_executable(pid_t pid)
{
    char path[64], exe[256];
    snprintf(path, sizeof(path), "/proc/%d/exe", pid);
    ssize_t n = readlink(path, exe, sizeof(exe) - 1);
    if (n < 0) return 0;
    exe[n] = '\0';
    return !strcmp(exe, "/system/bin/pxreyetrackingservice");
}

static int belongs_to_target(pid_t tid, pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", tid);
    char *status = read_file(path);
    if (!status) return 0;
    const char *field = strstr(status, "Tgid:");
    int same = field && strtol(field + 5, NULL, 10) == pid;
    free(status);
    return same;
}

struct inj {
    pid_t pid;
    unsigned long long start;
    int allow_any;
    pid_t tids[MAX_TIDS];
    int detach_signals[MAX_TIDS];
    int stopped[MAX_TIDS];
    int ntids;
    pid_t exec_tid;
    struct arm64_regs save;
    struct user_fpsimd_state save_fpsimd;
    int have_save;
    int have_fpsimd;
    int regs_dirty;
    int remote_active;
    int exec_stopped;
    unsigned long long sentinel;
    unsigned long long sentinel_orig;
    int sentinel_written;
};

// Copy up to one page into the target using 8-byte PEEK/POKE read-modify-write.
static int write_mem_checked(struct inj *j, unsigned long long addr, const void *buf, size_t len)
{
    if (len == 0 || len > 0x1000)
        return -1;
    const unsigned char *p = buf;
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done;
        if (chunk > 8)
            chunk = 8;
        unsigned long long word = 0;
        if (peek_mem(j->exec_tid, addr + done, &word) != 0)
            return -1;
        memcpy(&word, p + done, chunk);
        if (poke_mem(j->exec_tid, addr + done, word) != 0)
            return -1;
        done += chunk;
    }
    return 0;
}

// Restore the sentinel through whichever attached thread still works.
static int poke_any(struct inj *j, unsigned long long addr, unsigned long long val)
{
    for (int i = 0; i < j->ntids; i++) {
        if (j->tids[i] > 0 && poke_mem(j->tids[i], addr, val) == 0)
            return 0;
    }
    return -1;
}

// Do not resume a half-finished dlopen/constructor. SIGKILL is sent through an
// attached TID in this thread group, rather than killing a possibly recycled PID.
static void terminate_traced(struct inj *j)
{
    fprintf(stderr, "[!] unsafe target state; terminating thread group %d (exit 5)\n", j->pid);
    for (int i = 0; i < j->ntids; i++) {
        if (j->tids[i] <= 0)
            continue;
        // A failed/finished trace must never signal a recycled process ID.
        if (start_ticks(j->pid) != j->start || !belongs_to_target(j->tids[i], j->pid))
            continue;
        if (tgkill(j->pid, j->tids[i], SIGKILL) == 0)
            break;
        if (errno != ESRCH)
            fprintf(stderr, "[!] SIGKILL tid %d: %s\n", j->tids[i], strerror(errno));
    }
    for (int i = 0; i < j->ntids; i++) {
        if (j->tids[i] <= 0)
            continue;
        // Keep EXITKILL armed until tracer exit. In particular, a denied tgkill
        // must not lead to implicit signal-zero detach of an unsafe executor.
        int status;
        waitpid(j->tids[i], &status, WNOHANG | __WALL);
    }
    j->ntids = 0;
}

static int remote_call(struct inj *j, unsigned long long fn, const unsigned long long *args,
                       int nargs, long *ret)
{
    if (!j->have_save || !j->have_fpsimd || !j->exec_stopped || j->remote_active) {
        fprintf(stderr, "[-] remote call refused: complete saved context required\n");
        return -1;
    }
    struct arm64_regs r = j->save;
    for (int i = 0; i < 8; i++)
        r.regs[i] = (i < nargs) ? args[i] : 0ULL;
    r.regs[30] = j->sentinel; /* x30 = LR -> sentinel */
    r.pc = fn;
    r.sp = j->save.sp & ~0xFULL;              /* keep AAPCS64 alignment */
    r.pstate = j->save.pstate & ~0x200200ULL; /* clear SS(D) and D debug bits */
    j->regs_dirty = 1;
    if (set_regs(j->exec_tid, &r) != 0) {
        fprintf(stderr, "[-] SETREGSET failed: %s\n", strerror(errno));
        return -1;
    }
    if (ptrace(PTRACE_CONT, j->exec_tid, NULL, NULL) != 0) {
        fprintf(stderr, "[-] CONT failed: %s\n", strerror(errno));
        return -1;
    }
    j->exec_stopped = 0;
    j->remote_active = 1;
    int status = 0;
    int w = wait_stop(j->exec_tid, &status, REMOTE_TIMEOUT_MS);
    if (w == 1) {
        fprintf(stderr, "[-] remote call timed out after %d ms\n", REMOTE_TIMEOUT_MS);
        return -4;
    }
    if (w != 0) {
        fprintf(stderr, "[-] waitpid failed: %s\n", strerror(errno));
        return -1;
    }
    if (!WIFSTOPPED(status)) {
        fprintf(stderr, "[-] executor died during remote call\n");
        return -2;
    }
    j->exec_stopped = 1;
    if (WSTOPSIG(status) != SIGTRAP) {
        fprintf(stderr, "[-] executor stopped on signal %d (expected SIGTRAP)\n", WSTOPSIG(status));
        return -3;
    }
    struct arm64_regs a;
    if (get_regs(j->exec_tid, &a) != 0) {
        fprintf(stderr, "[-] GETREGSET failed after trap: %s\n", strerror(errno));
        return -1;
    }
    siginfo_t si;
    if (a.pc != j->sentinel || ptrace(PTRACE_GETSIGINFO, j->exec_tid, NULL, &si) != 0 ||
        si.si_signo != SIGTRAP || si.si_code != TRAP_BRKPT) {
        fprintf(stderr, "[-] trap is not the remote-call sentinel\n");
        return -1;
    }
    *ret = (long)a.regs[0];
    j->remote_active = 0;
    return 0;
}

static int cleanup(struct inj *j)
{
    if (j->remote_active) {
        terminate_traced(j);
        return 5;
    }
    // Restore code first (works through any surviving thread).
    if (j->sentinel_written) {
        unsigned long long restored = 0;
        if (poke_any(j, j->sentinel, j->sentinel_orig) != 0 ||
            peek_mem(j->exec_tid, j->sentinel, &restored) != 0 || restored != j->sentinel_orig) {
            fprintf(stderr, "[!] FAILED to restore sentinel bytes\n");
            terminate_traced(j);
            return 5;
        }
        j->sentinel_written = 0;
    }
    if (j->regs_dirty) {
        if (!j->exec_stopped || !j->have_save || !j->have_fpsimd ||
            set_fpsimd(j->exec_tid, &j->save_fpsimd) != 0 ||
            set_regs(j->exec_tid, &j->save) != 0) {
            fprintf(stderr, "[!] FAILED to restore full executor context\n");
            terminate_traced(j);
            return 5;
        }
        struct arm64_regs regs;
        struct user_fpsimd_state fpsimd;
        if (get_regs(j->exec_tid, &regs) != 0 || memcmp(&regs, &j->save, sizeof(regs)) ||
            get_fpsimd(j->exec_tid, &fpsimd) != 0 ||
            memcmp(fpsimd.vregs, j->save_fpsimd.vregs, sizeof(fpsimd.vregs)) ||
            fpsimd.fpsr != j->save_fpsimd.fpsr || fpsimd.fpcr != j->save_fpsimd.fpcr) {
            fprintf(stderr, "[!] restored executor context did not read back\n");
            struct arm64_regs actual;
            if (get_regs(j->exec_tid, &actual) == 0) {
                fprintf(stderr, "[!] pc=%llx/%llx sp=%llx/%llx pstate=%llx/%llx (actual/saved)\n",
                        actual.pc, j->save.pc, actual.sp, j->save.sp, actual.pstate, j->save.pstate);
                for (int r = 0; r < 31; ++r)
                    if (actual.regs[r] != j->save.regs[r])
                        fprintf(stderr, "[!] x%d=%llx/%llx\n", r, actual.regs[r], j->save.regs[r]);
            }
            terminate_traced(j);
            return 5;
        }
        j->regs_dirty = 0;
    }
    for (int i = 0; i < j->ntids; i++) {
        if (j->tids[i] > 0) {
            // DETACH requires a ptrace-stop. An unconfirmed SEIZE has options=0,
            // no injected SIGSTOP and no remote writes. On this attach-failure
            // path finish() exits the tracer, implicitly releasing that lease.
            if (!j->stopped[i]) {
                fprintf(stderr, "[-] unconfirmed SEIZE tid %d; releasing on tracer exit\n", j->tids[i]);
                j->tids[i] = 0;
                continue;
            }
            if (ptrace(PTRACE_DETACH, j->tids[i], NULL,
                       (void *)(unsigned long)j->detach_signals[i]) != 0) {
                fprintf(stderr, "[!] cannot detach tid %d: %s\n", j->tids[i], strerror(errno));
                terminate_traced(j);
                return 5;
            }
        }
        j->tids[i] = 0;
    }
    j->ntids = 0;
    return 0;
}

static int finish(struct inj *j, int result)
{
    int rc = cleanup(j);
    return rc ? rc : result;
}

static int attach_all(struct inj *j)
{
    char dir[64];
    snprintf(dir, sizeof(dir), "/proc/%d/task", j->pid);
    long long start = monotonic_ms();
    if (start < 0) return -1;
    j->ntids = 0;
    // Re-scan after stopping newly discovered threads. Only an unchanged scan
    // closes the clone-during-enumeration race; all scans share one 2s budget.
    for (;;) {
        DIR *d = opendir(dir);
        if (!d) {
            fprintf(stderr, "[-] cannot open %s: %s\n", dir, strerror(errno));
            return -1;
        }
        int added = 0;
        for (;;) {
            errno = 0;
            struct dirent *de = readdir(d);
            if (!de) {
                int failed = errno != 0;
                if (closedir(d) != 0) failed = 1;
                if (failed) return -1;
                break;
            }
            if (de->d_name[0] < '0' || de->d_name[0] > '9')
                continue;
            pid_t tid = (pid_t)atoi(de->d_name);
            if (tid <= 0)
                continue;
            int known = 0;
            for (int i = 0; i < j->ntids; ++i) if (j->tids[i] == tid) { known = 1; break; }
            if (known) continue;
            long long now = monotonic_ms();
            int remaining = (int)(ATTACH_TIMEOUT_MS - (now - start));
            if (now < 0 || remaining <= 0) { closedir(d); return -1; }
            if (j->ntids >= MAX_TIDS) {
                fprintf(stderr, "[-] more than %d threads; aborting (all-or-nothing)\n", MAX_TIDS);
                closedir(d);
                return -1;
            }
            // SEIZE injects no SIGSTOP. A real signal arriving before our
            // interrupt can be handed back without leaving an attach-stop pending.
            // EXITKILL is armed only after this stopped task's identity is checked.
            if (ptrace(PTRACE_SEIZE, tid, NULL, NULL) != 0) {
                if (errno == ESRCH)
                    continue; /* thread exited before we got to it */
                fprintf(stderr, "[-] SEIZE tid %d failed: %s (aborting)\n", tid, strerror(errno));
                closedir(d);
                return -1;
            }
            int slot = j->ntids++;
            j->tids[slot] = tid;
            j->stopped[slot] = j->detach_signals[slot] = 0;
            ++added;
            int interrupted = ptrace(PTRACE_INTERRUPT, tid, NULL, NULL);
            if (interrupted != 0 && errno != ESRCH) {
                fprintf(stderr, "[-] INTERRUPT tid %d failed: %s\n", tid, strerror(errno));
                closedir(d);
                return -1;
            }
            int status = 0;
            if (wait_stop(tid, &status, remaining) != 0) {
                fprintf(stderr, "[-] tid %d did not stop cleanly (aborting)\n", tid);
                closedir(d);
                return -1;
            }
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                // This terminal status has been reaped, not inferred from ESRCH.
                // The vanished worker no longer owns a trace lease to clean up.
                j->tids[--j->ntids] = 0;
                continue;
            }
            if (!WIFSTOPPED(status)) { closedir(d); return -1; }
            j->stopped[slot] = 1;
            unsigned int event = (unsigned int)status >> 16;
            // Only signal-delivery stops accept a detach signal. Event stops
            // preserve the kernel's existing group-stop state, not a new signal.
            if (!event) j->detach_signals[slot] = WSTOPSIG(status);
            if (!belongs_to_target(tid, j->pid) || start_ticks(j->pid) != j->start ||
                (!j->allow_any && !target_executable(j->pid))) {
                fprintf(stderr, "[-] tid %d no longer belongs to target; no writes attempted\n", tid);
                closedir(d);
                return -1;
            }
            if (interrupted != 0) { closedir(d); return -1; }
            if (event && event != PTRACE_EVENT_STOP) { closedir(d); return -1; }
            if (event != PTRACE_EVENT_STOP || WSTOPSIG(status) != SIGTRAP) {
                fprintf(stderr, "[-] real signal/group stop on tid %d; no remote calls attempted\n", tid);
                closedir(d);
                return 3;
            }
            if (ptrace(PTRACE_SETOPTIONS, tid, NULL, (void *)(unsigned long)PTRACE_O_EXITKILL) != 0) {
                fprintf(stderr, "[-] cannot arm EXITKILL on tid %d: %s\n", tid, strerror(errno));
                closedir(d);
                return -1;
            }
        }
        long long now = monotonic_ms();
        if (now < 0 || now - start >= ATTACH_TIMEOUT_MS) return -1;
        if (!added) break;
    }
    if (j->ntids == 0) {
        fprintf(stderr, "[-] attached no threads\n");
        return -1;
    }
    return 0;
}

static int trap_available(pid_t tid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", tid);
    char *status = read_file(path);
    if (!status) return 0;
    const char *blocked = strstr(status, "\nSigBlk:");
    const char *ignored = strstr(status, "\nSigIgn:");
    unsigned long long trap = 1ULL << (SIGTRAP - 1);
    int available = blocked && ignored && !(strtoull(blocked + 8, NULL, 16) & trap) &&
                    !(strtoull(ignored + 8, NULL, 16) & trap);
    free(status);
    return available;
}

static int executor_usable(pid_t tid, const struct arm64_regs *regs)
{
    if (pc_in_linker(regs->pc) || regs->pc < 4 || !in_exec_map(regs->pc) ||
        !in_exec_map(regs->pc - 4)) return 0;
    unsigned long long at, before;
    if (peek_mem(tid, regs->pc & ~7ULL, &at) || peek_mem(tid, (regs->pc - 4) & ~7ULL, &before))
        return 0;
    uint32_t instruction = (uint32_t)(at >> ((regs->pc & 7) * 8));
    uint32_t previous = (uint32_t)(before >> (((regs->pc - 4) & 7) * 8));
    // Do not hijack the SVC/restart boundary. This is a conservative filter,
    // not a reconstruction of the arm64 kernel's syscall-restart state.
    if ((instruction & 0xffe0001fU) == 0xd4000001U ||
        (previous & 0xffe0001fU) == 0xd4000001U) return 0;
    return trap_available(tid);
}

// Bionic's syscall result comparison is one ordinary register-only instruction.
// Let it execute, rather than rewriting PC or trying to restore kernel restart
// metadata. Never single-step SVC itself (it could block with peers frozen).
static int advance_svc_return(struct inj *j)
{
    for (int i = 0; i < j->ntids; ++i) {
        pid_t tid = j->tids[i];
        struct arm64_regs before, after;
        if (get_regs(tid, &before) || (before.pstate & 0x200000ULL) || before.pc < 4 || pc_in_linker(before.pc) ||
            !in_exec_map(before.pc) || !in_exec_map(before.pc - 4)) continue;
        unsigned long long at, previous;
        if (peek_mem(tid, before.pc & ~7ULL, &at) ||
            peek_mem(tid, (before.pc - 4) & ~7ULL, &previous)) continue;
        uint32_t instruction = (uint32_t)(at >> ((before.pc & 7) * 8));
        uint32_t prior = (uint32_t)(previous >> (((before.pc - 4) & 7) * 8));
        // CMN X0, #1, LSL #12. It only changes NZCV, not X0 or memory.
        if (instruction != 0xb140041fU || (prior & 0xffe0001fU) != 0xd4000001U ||
            !trap_available(tid)) continue;
        if (ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL)) return 2;
        j->stopped[i] = 0;
        int status = 0;
        int waited = wait_stop(tid, &status, 100);
        if (waited != 0) {
            int interrupted = ptrace(PTRACE_INTERRUPT, tid, NULL, NULL);
            int interrupt_error = errno;
            // ESRCH can race with an already queued terminal status. Reap and
            // classify that status even when the interrupt request failed.
            int recovered = wait_stop(tid, &status, ATTACH_TIMEOUT_MS);
            if (interrupted != 0)
                fprintf(stderr, "[-] single-step interrupt tid %d: %s\n", tid, strerror(interrupt_error));
            if (recovered != 0) {
                terminate_traced(j);
                return 5;
            }
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            j->tids[i] = 0;
            return 2;
        }
        if (!WIFSTOPPED(status)) {
            terminate_traced(j);
            return 5;
        }
        j->stopped[i] = 1;
        unsigned int event = (unsigned int)status >> 16;
        if (waited != 0 && event == PTRACE_EVENT_STOP && WSTOPSIG(status) == SIGTRAP) return 3;
        siginfo_t si;
        int trace_trap = !event && WSTOPSIG(status) == SIGTRAP &&
            !ptrace(PTRACE_GETSIGINFO, tid, NULL, &si) && si.si_code == TRAP_TRACE;
        if (!trace_trap) {
            if (!event) j->detach_signals[i] = WSTOPSIG(status);
            // A real signal/group stop before remote writes is temporary. Keep
            // unexpected traps or missing siginfo as ordinary checked failures.
            return event == PTRACE_EVENT_STOP || (!event && WSTOPSIG(status) != SIGTRAP) ? 3 : 2;
        }
        // A timed-out step is an observation-only attempt, even if its trace
        // trap arrived while recovering the stop. Do not risk a queued interrupt
        // being mistaken for completion of a subsequent remote call.
        if (waited != 0) return 3;
        if (get_regs(tid, &after) || after.pc != before.pc + 4 || after.sp != before.sp ||
            memcmp(after.regs, before.regs, sizeof(after.regs)) || !executor_usable(tid, &after)) return 2;
        printf("[*] stepped syscall result comparison tid=%d pc=0x%llx -> 0x%llx\n", tid, before.pc, after.pc);
        j->exec_tid = tid;
        j->save = after;
        // PSTATE.SS at TRAP_TRACE belongs to our one-step request, not to the
        // target context. PTRACE_CONT clears it; keep the real post-CMN NZCV.
        j->save.pstate &= ~0x200000ULL;
        j->have_save = 1;
        return 0;
    }
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s <pid> <abs-path-to-libpicoet_hook.so> [--any]\n", argv0);
    fprintf(stderr, "       %s --run-locked <lock-file> <shell-script> [args...]\n", argv0);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && (!strcmp(argv[1], "--read-file") || !strcmp(argv[1], "--read-log") ||
                      !strcmp(argv[1], "--normalize-mode") ||
                      !strcmp(argv[1], "--save-mode") || !strcmp(argv[1], "--prepare-runtime"))) {
        if (geteuid() != 0) return 2;
        if (!strcmp(argv[1], "--read-file") && argc == 3) return read_control_file(argv[2], 0);
        if (!strcmp(argv[1], "--read-log") && argc == 2)
            return read_control_file("/data/local/tmp/picoet-hook.log", 1);
        if (!strcmp(argv[1], "--save-mode") && argc == 3) return save_mode(argv[2]);
        if (!strcmp(argv[1], "--normalize-mode") && argc == 2) return normalize_mode();
        if (!strcmp(argv[1], "--prepare-runtime") && argc == 2) return prepare_runtime(0);
        if (!strcmp(argv[1], "--prepare-runtime") && argc == 3 && !strcmp(argv[2], "--reset"))
            return prepare_runtime(1);
        return 2;
    }
    // Serialize watchdog and manual commands without relying on an Android
    // shell flock applet. The lock FD survives exec and closes with the script.
    if (argc >= 4 && strcmp(argv[1], "--run-locked") == 0) {
        int wait_for_lock = argc > 4 && strcmp(argv[4], "ensure") != 0;
        int fd = control_lock(argv[2], wait_for_lock);
        if (fd < 0) return -fd;
        if (setenv("PICOET_CONTROL_LOCKED", "1", 1) != 0)
            return 2;
        argv[2] = "/system/bin/sh";
        execv(argv[2], &argv[2]);
        return 2;
    }
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    // Block termination signals across the critical window. SIGKILL is
    // unblockable; this shrinks (not eliminates) the interrupted-inject risk.
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGHUP);
    sigaddset(&block, SIGQUIT);
    sigprocmask(SIG_BLOCK, &block, NULL);

    struct inj j;
    memset(&j, 0, sizeof(j));
    j.pid = (pid_t)atoi(argv[1]);
    j.start = start_ticks(j.pid);
    const char *sopath = argv[2];
    int allow_any = (argc > 3 && strcmp(argv[3], "--any") == 0);
    j.allow_any = allow_any;

    if (j.pid <= 0 || sopath[0] != '/') {
        usage(argv[0]);
        return 1;
    }
    if (!j.start) {
        fprintf(stderr, "[-] cannot identify target instance\n");
        return 2;
    }
    size_t slen = strlen(sopath) + 1;
    if (slen > 0x1000) {
        fprintf(stderr, "[-] payload path too long (%zu)\n", slen);
        return 2;
    }
    if (access(sopath, R_OK) != 0) {
        fprintf(stderr, "[-] payload not readable: %s (%s)\n", sopath, strerror(errno));
        return 2;
    }

    const char *sbase = strrchr(sopath, '/');
    sbase = sbase ? sbase + 1 : sopath;

    // 1. sanity: process exists, not already traced, right flavour
    char pathbuf[128];
    snprintf(pathbuf, sizeof(pathbuf), "/proc/%d/comm", j.pid);
    char *comm = read_file(pathbuf);
    if (!comm) {
        fprintf(stderr, "[-] target pid %d not found\n", j.pid);
        return 2;
    }
    char comm_trim[64];
    snprintf(comm_trim, sizeof(comm_trim), "%s", comm);
    free(comm);
    char *nl = strchr(comm_trim, '\n');
    if (nl)
        *nl = '\0';
    // Some services rename their main thread (e.g. pxreyetrackingservice shows
    // comm "Binder:NNN_M"), so also consult cmdline for the sanity check.
    snprintf(pathbuf, sizeof(pathbuf), "/proc/%d/cmdline", j.pid);
    char *cmdline = read_file(pathbuf);
    int looks_right = strstr(comm_trim, "pxreyetracking") != NULL ||
                      (cmdline && strstr(cmdline, "pxreyetracking") != NULL);
    printf("[*] target pid=%d comm=%s cmdline=%s\n",
           j.pid, comm_trim, (cmdline && cmdline[0]) ? cmdline : "(empty)");
    free(cmdline);
    if (!allow_any && (!looks_right || !target_executable(j.pid))) {
        fprintf(stderr, "[-] target does not look like pxreyetrackingservice (use --any to override)\n");
        return 2;
    }

    snprintf(pathbuf, sizeof(pathbuf), "/proc/%d/status", j.pid);
    char *status = read_file(pathbuf);
    if (!status) {
        fprintf(stderr, "[-] cannot read target status\n");
        return 2;
    }
    int tracer = -1;
    char *tp = strstr(status, "TracerPid:");
    if (tp)
        tracer = atoi(tp + strlen("TracerPid:"));
    const char *state_field = strstr(status, "State:");
    if (state_field) {
        state_field += strlen("State:");
        while (*state_field == ' ' || *state_field == '\t') ++state_field;
    }
    int stopped = state_field && (*state_field == 'T' || *state_field == 't');
    free(status);
    if (tracer != 0) {
        fprintf(stderr, "[-] target TracerPid=%d (already traced; refusing)\n", tracer);
        return 2;
    }
    if (stopped) {
        fprintf(stderr, "[-] target is already stopped; refusing to consume its stop signal\n");
        return 3;
    }

    // 2. maps snapshot
    snprintf(pathbuf, sizeof(pathbuf), "/proc/%d/maps", j.pid);
    char *maps = read_file(pathbuf);
    if (!maps) {
        fprintf(stderr, "[-] cannot read target maps (need root)\n");
        return 2;
    }
    parse_maps(maps);
    // "Already mapped" only counts an executable mapping. Actual installed hooks
    // are reported by the payload's PID/starttime-bound runtime record.
    // Interrupted attempts leave read-only fragment mappings behind; a new dlopen
    // against that half-initialised state can abort the A10 linker, so report it
    // as a distinct retryable state instead.
    int mapped_exec = 0, mapped_stale = 0;
    for (int i = 0; i < g_nentries; i++) {
        if (!basename_eq(g_entries[i].path, sbase))
            continue;
        if (strchr(g_entries[i].perms, 'x')) {
            mapped_exec = 1;
            break;
        }
        mapped_stale = 1;
    }
    if (mapped_exec) {
        printf("[=] payload already mapped and executable; nothing to do\n");
        free(maps);
        return 0;
    }
    if (mapped_stale) {
        fprintf(stderr, "[-] stale partial mapping of %s (no executable segment);"
                        " restart the target and retry\n", sbase);
        free(maps);
        return 4;
    }

    // 3. resolve local symbols -> module name/path + offset
    char dlmod[128], mmapmod[128], dlpath_local[768], mmappath_local[768];
    unsigned long long dloff = 0, mmapoff = 0;
    if (resolve_local("dlopen", dlmod, sizeof(dlmod), dlpath_local, sizeof(dlpath_local), &dloff) != 0) {
        fprintf(stderr, "[-] cannot resolve local dlopen\n");
        free(maps);
        return 2;
    }
    if (resolve_local("mmap", mmapmod, sizeof(mmapmod), mmappath_local, sizeof(mmappath_local), &mmapoff) != 0) {
        fprintf(stderr, "[-] cannot resolve local mmap\n");
        free(maps);
        return 2;
    }
    printf("[*] local dlopen %s +0x%llx ; mmap %s +0x%llx\n",
           dlpath_local, dloff, mmappath_local, mmapoff);

    // 4. find the matching modules in the target and verify the full path
    char dlpath[768] = {0}, mmpath[768] = {0};
    unsigned long long dlbase = find_module_base(dlmod, dlpath, sizeof(dlpath));
    unsigned long long mmbase = find_module_base(mmapmod, mmpath, sizeof(mmpath));
    if (!dlbase || !mmbase) {
        fprintf(stderr, "[-] target lacks %s (base=%llx) or %s (base=%llx)\n",
                dlmod, dlbase, mmapmod, mmbase);
        free(maps);
        return 2;
    }
    if (strcmp(dlpath, dlpath_local) != 0 || strcmp(mmpath, mmappath_local) != 0) {
        fprintf(stderr, "[-] module path mismatch (same name, different file); refusing\n"
                        "    local dlopen=%s target=%s\n"
                        "    local mmap  =%s target=%s\n",
                dlpath_local, dlpath, mmappath_local, mmpath);
        free(maps);
        return 2;
    }
    unsigned long long dlopen_addr = dlbase + dloff;
    unsigned long long mmap_addr = mmbase + mmapoff;
    if (!in_exec_map(dlopen_addr) || !in_exec_map(mmap_addr)) {
        fprintf(stderr, "[-] resolved addresses are not in executable mappings; refusing\n");
        free(maps);
        return 2;
    }
    printf("[*] target dlopen=0x%llx (%s) ; mmap=0x%llx (%s)\n",
           dlopen_addr, dlpath, mmap_addr, mmpath);

    // 5. sentinel
    char sdesc[256] = {0};
    if (find_sentinel(&j.sentinel, sdesc, sizeof(sdesc)) != 0) {
        fprintf(stderr, "[-] no executable page for sentinel\n");
        free(maps);
        return 2;
    }
    printf("[*] sentinel @0x%llx (%s)\n", j.sentinel, sdesc);

    // 6. attach everything (all-or-nothing)
    int attach_rc = attach_all(&j);
    if (attach_rc != 0) {
        free(maps);
        return finish(&j, attach_rc == 3 ? 3 : 2);
    }
    printf("[*] attached %d threads\n", j.ntids);
    if (start_ticks(j.pid) != j.start || (!allow_any && !target_executable(j.pid))) {
        fprintf(stderr, "[-] target instance changed during attach\n");
        free(maps);
        return finish(&j, 2);
    }

    // 7. Prefer the group leader at a usable user instruction, outside the
    //    linker and syscall boundary, with SIGTRAP unblocked and not ignored.
    j.exec_tid = 0;
    for (int i = 0; i < j.ntids && j.exec_tid == 0; i++) {
        if (j.tids[i] != j.pid)
            continue;
        struct arm64_regs t;
        if (get_regs(j.tids[i], &t) == 0 && executor_usable(j.tids[i], &t))
            j.exec_tid = j.tids[i];
    }
    for (int i = 0; i < j.ntids && j.exec_tid == 0; i++) {
        struct arm64_regs t;
        if (get_regs(j.tids[i], &t) == 0 && executor_usable(j.tids[i], &t))
            j.exec_tid = j.tids[i];
    }
    if (j.exec_tid == 0) {
        int step_rc = advance_svc_return(&j);
        if (step_rc != 0) {
            free(maps);
            return finish(&j, step_rc);
        }
    }
    if (j.exec_tid == 0) {
        fprintf(stderr, "[-] no safe user-instruction executor; retry later\n");
        free(maps);
        return finish(&j, 3);
    }
    if (!j.have_save && get_regs(j.exec_tid, &j.save) != 0) {
        fprintf(stderr, "[-] GETREGSET on executor %d failed: %s\n", j.exec_tid, strerror(errno));
        free(maps);
        return finish(&j, 2);
    }
    j.have_save = 1;
    j.exec_stopped = 1;
    if (get_fpsimd(j.exec_tid, &j.save_fpsimd) != 0) {
        fprintf(stderr, "[-] GETREGSET FPSIMD failed: %s; no remote call attempted\n", strerror(errno));
        free(maps);
        return finish(&j, 2);
    }
    j.have_fpsimd = 1;
    printf("[*] executor tid=%d sp=0x%llx pc=0x%llx\n", j.exec_tid, j.save.sp, j.save.pc);

    // 8. refresh maps while stopped, then plant the sentinel
    free(maps);
    snprintf(pathbuf, sizeof(pathbuf), "/proc/%d/maps", j.pid);
    maps = read_file(pathbuf);
    if (!maps) {
        fprintf(stderr, "[-] cannot re-read maps after attach\n");
        return finish(&j, 2);
    }
    parse_maps(maps);
    // Addresses resolved before attach must still refer to the same modules.
    char current_path[768];
    if (find_module_base(dlmod, current_path, sizeof(current_path)) != dlbase || strcmp(current_path, dlpath) ||
        find_module_base(mmapmod, current_path, sizeof(current_path)) != mmbase || strcmp(current_path, mmpath) ||
        !in_exec_map(dlopen_addr) || !in_exec_map(mmap_addr)) {
        fprintf(stderr, "[-] loader mappings changed during attach\n");
        free(maps);
        return finish(&j, 2);
    }
    for (int i = 0; i < j.ntids; ++i) {
        struct arm64_regs regs;
        if (get_regs(j.tids[i], &regs) != 0 || pc_in_linker(regs.pc)) {
            fprintf(stderr, "[-] thread unavailable or inside linker; retry later\n");
            free(maps);
            return finish(&j, 3);
        }
    }
    if (find_sentinel(&j.sentinel, sdesc, sizeof(sdesc)) != 0) {
        fprintf(stderr, "[-] sentinel disappeared after attach\n");
        free(maps);
        return finish(&j, 2);
    }
    unsigned long long orig = 0;
    if (peek_mem(j.exec_tid, j.sentinel, &orig) != 0) {
        fprintf(stderr, "[-] PEEKDATA sentinel failed: %s\n", strerror(errno));
        free(maps);
        return finish(&j, 2);
    }
    j.sentinel_orig = orig;
    if (poke_mem(j.exec_tid, j.sentinel, (orig & ~0xffffffffULL) | 0xD4200000ULL) != 0) {
        fprintf(stderr, "[-] POKEDATA sentinel (brk #0) failed: %s\n", strerror(errno));
        free(maps);
        return finish(&j, 2);
    }
    j.sentinel_written = 1;
    printf("[*] sentinel planted (brk #0)\n");
    free(maps);

    // 9. remote mmap(NULL, 0x1000, RW, PRIVATE|ANON, -1, 0)
    unsigned long long margs[6] = { 0, 0x1000, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, (unsigned long long)-1, 0 };
    long buf = 0;
    printf("[*] remote mmap...\n");
    if (remote_call(&j, mmap_addr, margs, 6, &buf) != 0) {
        return finish(&j, 2);
    }
    if (buf == 0 || (unsigned long long)buf == (unsigned long long)-1) {
        fprintf(stderr, "[-] remote mmap returned 0x%lx\n", buf);
        return finish(&j, 2);
    }
    printf("[*] remote mmap -> 0x%lx\n", buf);

    // 10. copy the path string into the target buffer (RMW, stays in page)
    if (write_mem_checked(&j, (unsigned long long)buf, sopath, slen) != 0) {
        fprintf(stderr, "[-] writing payload path failed\n");
        return finish(&j, 2);
    }
    printf("[*] payload path written (%zu bytes)\n", slen);

    // 11. remote dlopen(path, RTLD_NOW)
    unsigned long long dargs[2] = { (unsigned long long)buf, (unsigned long long)RTLD_NOW };
    long handle = 0;
    printf("[*] remote dlopen...\n");
    if (remote_call(&j, dlopen_addr, dargs, 2, &handle) != 0) {
        return finish(&j, 2);
    }
    printf("[*] dlopen returned handle=0x%lx\n", handle);

    // 12. teardown
    int cleanup_rc = cleanup(&j);
    if (cleanup_rc != 0)
        return cleanup_rc;
    if (handle == 0) {
        fprintf(stderr, "[-] injection failed: dlopen returned NULL\n");
        return 2;
    }
    if (kill(j.pid, 0) != 0) {
        fprintf(stderr, "[!] warning: target pid %d no longer alive\n", j.pid);
        return 2;
    }
    printf("[+] injected %s into pid %d; verify /proc/%d/maps contains %s\n",
           sopath, j.pid, j.pid, sbase);
    return 0;
}
