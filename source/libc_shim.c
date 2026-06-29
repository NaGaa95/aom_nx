/* libc_shim.c -- bionic-compatible libc wrappers for libmcfandroid.so
 *
 * Converting wrappers for the spots where bionic and newlib differ (struct
 * layouts, flag values, missing functions), plus the AAsset NDK emulation that
 * serves the game's loose assets/ tree.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "libc_shim.h"

// --- fortify (_chk) wrappers: ignore the object-size argument ----------------

void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen; return memcpy(dst, src, n);
}
void *__memset_chk_fake(void *dst, int c, size_t n, size_t dstlen) {
  (void)dstlen; return memset(dst, c, n);
}
char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen; return strcat(dst, src);
}
char *__strchr_chk_fake(const char *s, int c, size_t slen) {
  (void)slen; return strchr(s, c);
}
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen; return strcpy(dst, src);
}
size_t __strlen_chk_fake(const char *s, size_t slen) {
  (void)slen; return strlen(s);
}
char *__strncat_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) {
  (void)dstlen; return strncat(dst, src, n);
}
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen; return vsnprintf(s, maxlen, fmt, va);
}
int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen; return vsprintf(s, fmt, va);
}

// --- misc bionic ------------------------------------------------------------

int __system_property_get_fake(const char *name, char *value) {
  (void)name; value[0] = '\0'; return 0;
}

unsigned long getauxval_fake(unsigned long type) { (void)type; return 0; }

int gettid_fake(void) {
  u64 tid = 1;
  if (R_SUCCEEDED(svcGetThreadId(&tid, CUR_THREAD_HANDLE)) && tid)
    return (int)(tid & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_GETTID 178

long syscall_fake(long number, ...) {
  if (number == ARM64_SYS_GETTID)
    return gettid_fake();
  errno = ENOSYS;
  return -1;
}

void sincosf_fake(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
void sincos_fake(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }

void android_set_abort_message_fake(const char *msg) {
  debugPrintf("abort message: %s\n", msg ? msg : "(null)");
}

size_t __ctype_get_mb_cur_max_fake(void) { return 1; }
int __register_atfork_fake(void) { return 0; }
int __cxa_thread_atexit_impl_fake(void (*fn)(void *), void *arg, void *dso) {
  (void)fn; (void)arg; (void)dso; return 0; // threads never exit cleanly here
}

#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98

long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE:           return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN:    return 3;
    case BIONIC_SC_PHYS_PAGES:          return (3ll * 1024 * 1024 * 1024) / 0x1000;
    default:                            return -1;
  }
}

long pathconf_fake(const char *path, int name) { (void)path; (void)name; return -1; }

// --- open() flag translation (bionic/linux -> newlib) -----------------------

#define LINUX_O_CREAT  0100
#define LINUX_O_EXCL   0200
#define LINUX_O_TRUNC  01000
#define LINUX_O_APPEND 02000

static int convert_open_flags(int flags) {
  int out = flags & 3;
  if (flags & LINUX_O_CREAT)  out |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out |= O_APPEND;
  return out;
}

int open_fake(const char *path, int flags, ...) {
  int mode = 0666;
  if (flags & LINUX_O_CREAT) {
    va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va);
  }
  return open(path, convert_open_flags(flags), mode);
}

int openat_fake(int dirfd, const char *path, int flags, ...) {
  (void)dirfd;
  int mode = 0666;
  if (flags & LINUX_O_CREAT) {
    va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va);
  }
  return open(path, convert_open_flags(flags), mode);
}

int __open_2_fake(const char *path, int flags) {
  return open(path, convert_open_flags(flags), 0666);
}

int unlinkat_fake(int dirfd, const char *path, int flags) {
  (void)dirfd; (void)flags; return unlink(path);
}

int mkdir_fake(const char *path, int mode) { return mkdir(path, (mode_t)mode); }

// Create a directory and any missing parents (like `mkdir -p`).
static void mkdir_p(const char *path) {
  char tmp[512];
  int len = snprintf(tmp, sizeof(tmp), "%s", path);
  if (len <= 0 || len >= (int)sizeof(tmp)) return;
  while (len > 1 && (tmp[len - 1] == '/' || isspace((unsigned char)tmp[len - 1])))
    tmp[--len] = 0; // trim trailing slash / whitespace
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') { *p = 0; mkdir(tmp, 0777); *p = '/'; }
  }
  mkdir(tmp, 0777);
}

// The engine creates its save dir via system("mkdir <path>"); the Switch has no
// shell, so emulate just that command (else the save fopen fails silently).
int system_fake(const char *cmd) {
  if (!cmd) return -1;
  while (*cmd == ' ') cmd++;
  if (strncmp(cmd, "mkdir ", 6) != 0) return -1;
  const char *p = cmd + 6;
  while (*p == ' ') p++;
  mkdir_p(p);
  return 0;
}
int ftruncate_fake(int fd, long length) { return ftruncate(fd, (off_t)length); }
int truncate_fake(const char *path, long length) { return truncate(path, (off_t)length); }

// fdopendir is not in devkitA64 newlib; libc++ imports it for std::filesystem,
// which the game never exercises.
void *fdopendir_fake(int fd) { (void)fd; return NULL; }

// --- struct stat conversion (bionic aarch64 layout) -------------------------

struct bionic_timespec { int64_t tv_sec; int64_t tv_nsec; };

struct bionic_stat {
  uint64_t st_dev, st_ino;
  uint32_t st_mode, st_nlink, st_uid, st_gid;
  uint64_t st_rdev, __pad1;
  int64_t st_size;
  int32_t st_blksize, __pad2;
  int64_t st_blocks;
  struct bionic_timespec st_atim, st_mtim, st_ctim;
  uint32_t __unused4, __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev; out->st_ino = in->st_ino; out->st_mode = in->st_mode;
  out->st_nlink = in->st_nlink; out->st_uid = in->st_uid; out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev; out->st_size = in->st_size;
  out->st_blksize = in->st_blksize; out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atime;
  out->st_mtim.tv_sec = in->st_mtime;
  out->st_ctim.tv_sec = in->st_ctime;
}

int stat_fake(const char *path, struct bionic_stat *st) {
  struct stat real;
  int ret = stat(path, &real);
  if (ret == 0) convert_stat(&real, st);
  return ret;
}
int fstat_fake(int fd, struct bionic_stat *st) {
  struct stat real;
  int ret = fstat(fd, &real);
  if (ret == 0) convert_stat(&real, st);
  return ret;
}
int lstat_fake(const char *path, struct bionic_stat *st) { return stat_fake(path, st); }

// --- dirent conversion (bionic dirent64 layout) -----------------------------

struct bionic_dirent {
  uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256];
};

void *readdir_fake(void *dirp) {
  static struct bionic_dirent out; // not thread-safe (matches readdir)
  struct dirent *e = readdir((DIR *)dirp);
  if (!e) return NULL;
  memset(&out, 0, sizeof(out));
  out.d_ino = e->d_ino;
  out.d_reclen = sizeof(out);
  out.d_type = e->d_type;
  snprintf(out.d_name, sizeof(out.d_name), "%s", e->d_name);
  return &out;
}

// --- locale: ignore the locale handle, use the C-locale versions ------------

void *newlocale_fake(int mask, const char *locale, void *base) {
  (void)mask; (void)locale; (void)base; return (void *)1;
}
void freelocale_fake(void *loc) { (void)loc; }
void *uselocale_fake(void *loc) { (void)loc; return (void *)1; }

#define WRAP_ISW_L(fn) int fn##_l_fake(int wc, void *loc) { (void)loc; return fn(wc); }
WRAP_ISW_L(iswalpha) WRAP_ISW_L(iswblank) WRAP_ISW_L(iswcntrl) WRAP_ISW_L(iswdigit)
WRAP_ISW_L(iswlower) WRAP_ISW_L(iswprint) WRAP_ISW_L(iswpunct) WRAP_ISW_L(iswspace)
WRAP_ISW_L(iswupper) WRAP_ISW_L(iswxdigit) WRAP_ISW_L(towlower) WRAP_ISW_L(towupper)

int strcoll_l_fake(const char *a, const char *b, void *loc) { (void)loc; return strcoll(a, b); }
size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc) { (void)loc; return strxfrm(dst, src, n); }
size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc) {
  (void)loc; return strftime(s, max, fmt, (const struct tm *)tm);
}
long double strtold_l_fake(const char *s, char **end, void *loc) { (void)loc; return strtold(s, end); }
long long strtoll_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoll(s, end, base); }
unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoull(s, end, base); }
int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc) { (void)loc; return wcscoll(a, b); }
size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc) { (void)loc; return wcsxfrm(dst, src, n); }

size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps) {
  (void)ps;
  size_t i = 0; const char *s = *src;
  while (i < nms && s[i] && (!dst || i < len)) { if (dst) dst[i] = (unsigned char)s[i]; i++; }
  if (dst && i < len) { dst[i] = 0; *src = NULL; }
  return i;
}
size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps) {
  (void)ps;
  size_t i = 0; const wchar_t *s = *src;
  while (i < nwc && s[i] && (!dst || i < len)) { if (dst) dst[i] = (char)s[i]; i++; }
  if (dst && i < len) { dst[i] = 0; *src = NULL; }
  return i;
}

// --- memory / fs odds and ends ----------------------------------------------

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}

char *realpath_fake(const char *path, char *resolved) {
  if (!resolved) resolved = malloc(0x1000);
  strcpy(resolved, path);
  return resolved;
}

int strerror_r_fake(int err, char *buf, size_t len) {
  snprintf(buf, len, "%s", strerror(err));
  return 0;
}

int statvfs_fake(const char *path, void *buf) { (void)path; memset(buf, 0, 0x70); return 0; }

// --- stdio over the fake bionic __sF (stdin/stdout/stderr) ------------------
// libc++ binds std::cout/cerr to &__sF[1]/[2]; these absorb those and forward
// everything else.

uint8_t fake_sF[3][0x100];

static int is_fake_file(const void *f) {
  const uint8_t *p = f, *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) return n;
  return fwrite(ptr, size, n, f);
}
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) return 0;
  return fread(ptr, size, n, f);
}
int fputc_fake(int c, FILE *f) { return is_fake_file(f) ? c : fputc(c, f); }
int fflush_fake(FILE *f) { return (is_fake_file(f) || !f) ? 0 : fflush(f); }
int fclose_fake(FILE *f) { return is_fake_file(f) ? 0 : fclose(f); }
int ferror_fake(FILE *f) { return is_fake_file(f) ? 0 : ferror(f); }
int fileno_fake(FILE *f) {
  if (is_fake_file(f)) return ((const uint8_t *)f - &fake_sF[0][0]) / 0x100;
  return fileno(f);
}
int fprintf_fake(FILE *f, const char *fmt, ...) {
  if (is_fake_file(f)) return 0;
  va_list va; va_start(va, fmt);
  int ret = vfprintf(f, fmt, va);
  va_end(va);
  return ret;
}
int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  return is_fake_file(f) ? 0 : vfprintf(f, fmt, va);
}
int fseek_fake(FILE *f, long off, int whence) { return is_fake_file(f) ? -1 : fseek(f, off, whence); }
int getc_fake(FILE *f) { return is_fake_file(f) ? -1 : getc(f); }
int ungetc_fake(int c, FILE *f) { return is_fake_file(f) ? -1 : ungetc(c, f); }

// --- AAsset emulation: serve assets from the loose assets/ tree next to the NRO

typedef struct { FILE *f; uint8_t *mem; size_t size, pos; } Asset;
typedef struct { DIR *dir; char base[512]; char name[256]; } AssetDir;

void *AAssetManager_fromJava_fake(void *env, void *mgr) { (void)env; (void)mgr; return (void *)1; }

// The engine passes APK-relative names ("sk1/sk1.mpk", "bgm001_*.ogg"), or
// occasionally an absolute path. Try assets/<name>, then the name verbatim.
static FILE *open_asset(const char *name, size_t *out_size) {
  char path[640];
  FILE *f = NULL;
  if (name && name[0] == '/') f = fopen(name, "rb");
  if (!f) { snprintf(path, sizeof(path), "%s/%s", ASSETS_DIR, name ? name : ""); f = fopen(path, "rb"); }
  if (!f && name) f = fopen(name, "rb");
  if (!f) return NULL;
  setvbuf(f, NULL, _IOFBF, 64 * 1024);
  fseek(f, 0, SEEK_END);
  if (out_size) *out_size = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  return f;
}

// buffered fopen for the big game archives
FILE *fopen_fake(const char *path, const char *mode) {
  FILE *f = fopen(path, mode);
  if (f && strchr(mode, 'r')) {
    const char *ext = strrchr(path, '.');
    if (ext && (strcasecmp(ext, ".mpk") == 0 || strcasecmp(ext, ".ogg") == 0))
      setvbuf(f, NULL, _IOFBF, 128 * 1024);
  }
  return f;
}

void *AAssetManager_open_fake(void *mgr, const char *path, int mode) {
  (void)mgr; (void)mode;
  Asset *a = calloc(1, sizeof(*a));
  if (!a) return NULL;
  a->f = open_asset(path, &a->size);
  if (!a->f) { free(a); return NULL; }
  // cache small assets (BGM oggs) in RAM so their decode doesn't contend with
  // map streaming off the SD during loads. sk1.mpk is too big -> file-backed.
  if (a->size > 0 && a->size <= 16 * 1024 * 1024) {
    a->mem = malloc(a->size);
    if (a->mem && fread(a->mem, 1, a->size, a->f) == a->size) {
      fclose(a->f);
      a->f = NULL;
    } else {
      free(a->mem);
      a->mem = NULL;
      fseek(a->f, 0, SEEK_SET);
    }
  }
  return a;
}

void AAsset_close_fake(void *asset) {
  Asset *a = asset;
  if (!a) return;
  if (a->f) fclose(a->f);
  free(a->mem);
  free(a);
}

int AAsset_read_fake(void *asset, void *buf, size_t count) {
  Asset *a = asset;
  if (!a) return -1;
  if (a->mem) {
    size_t avail = a->size - a->pos;
    if (count > avail) count = avail;
    memcpy(buf, a->mem + a->pos, count);
    a->pos += count;
    return (int)count;
  }
  return a->f ? (int)fread(buf, 1, count, a->f) : -1;
}

long AAsset_seek_fake(void *asset, long off, int whence) {
  Asset *a = asset;
  if (!a) return -1;
  if (a->mem) {
    long base = (whence == SEEK_CUR) ? (long)a->pos
              : (whence == SEEK_END) ? (long)a->size : 0;
    long np = base + off;
    if (np < 0 || (size_t)np > a->size) return -1;
    a->pos = (size_t)np;
    return (long)a->pos;
  }
  if (!a->f || fseek(a->f, off, whence) < 0) return -1;
  return ftell(a->f);
}

long AAsset_getLength_fake(void *asset) {
  Asset *a = asset;
  return a ? (long)a->size : 0;
}

void *AAssetManager_openDir_fake(void *mgr, const char *path) {
  (void)mgr;
  AssetDir *d = calloc(1, sizeof(*d));
  if (!d) return NULL;
  if (path && path[0]) snprintf(d->base, sizeof(d->base), "%s/%s", ASSETS_DIR, path);
  else                 snprintf(d->base, sizeof(d->base), "%s", ASSETS_DIR);
  d->dir = opendir(d->base); // NDK: a missing dir still yields a valid empty handle
  return d;
}

const char *AAssetDir_getNextFileName_fake(void *adir) {
  AssetDir *d = adir;
  if (!d || !d->dir) return NULL;
  struct dirent *e;
  while ((e = readdir(d->dir)) != NULL) {
    if (e->d_name[0] == '.') continue;
    char full[768];
    snprintf(full, sizeof(full), "%s/%s", d->base, e->d_name);
    struct stat st;
    if (stat(full, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR) continue; // files only
    snprintf(d->name, sizeof(d->name), "%s", e->d_name);
    return d->name;
  }
  return NULL;
}

void AAssetDir_close_fake(void *adir) {
  AssetDir *d = adir;
  if (!d) return;
  if (d->dir) closedir(d->dir);
  free(d);
}

// --- pthread rwlocks (bionic structs the game allocates; we stash a real
//     object pointer in the first bytes, like the mutex shims) ---------------

typedef struct { RwLock lock; } FakeRwLock;

static FakeRwLock *get_rwlock(void **storage) {
  if (!*storage) {
    FakeRwLock *l = calloc(1, sizeof(*l));
    rwlockInit(&l->lock);
    *storage = l;
  }
  return *storage;
}

int pthread_rwlock_rdlock_fake(void **rw) { rwlockReadLock(&get_rwlock(rw)->lock); return 0; }
int pthread_rwlock_wrlock_fake(void **rw) { rwlockWriteLock(&get_rwlock(rw)->lock); return 0; }
int pthread_rwlock_unlock_fake(void **rw) {
  FakeRwLock *l = get_rwlock(rw);
  if (rwlockIsWriteLockHeldByCurrentThread(&l->lock)) rwlockWriteUnlock(&l->lock);
  else rwlockReadUnlock(&l->lock);
  return 0;
}
