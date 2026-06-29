/* imports.c -- .so import resolution for libmcfandroid.so (Adventure of Mana)
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Provides the libc subset, GLES2 (mesa libGLESv2), OpenSLES and AAsset NDK API
 * the engine imports; std::string / RTTI / operator-new resolve from the
 * separately-loaded libc++_shared.so via the so_util module list.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <wchar.h>
#include <errno.h>
#include <locale.h>
#include <setjmp.h>
#include <dirent.h>
#include <zlib.h>
#include <sys/time.h>
#include <GLES2/gl2.h>
#include <switch.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "opensles.h"

extern uintptr_t __cxa_atexit;
extern uintptr_t __stack_chk_fail;

static uint64_t __stack_chk_guard_fake = 0x4242424242424242;

void __assert2(const char *file, int line, const char *func, const char *expr) {
  debugPrintf("assertion failed:\n%s:%d (%s): %s\n", file, line, func, expr);
  abort();
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
#if DEBUG_LOG
  va_list list;
  static char string[0x1000];
  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);
  debugPrintf("%s: %s\n", tag, string);
#else
  (void)tag; (void)fmt;
#endif
  return 0;
}

// bionic's pthread_mutex_t/cond_t are large structs the engine zero-inits
// inline; we lazily back them with newlib objects via the first pointer slot.

int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *mutexattr) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return -1;
  const int recursive = (mutexattr && *mutexattr == 1);
  int ret;
  if (recursive) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    ret = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
  } else {
    ret = pthread_mutex_init(m, NULL);
  }
  if (ret != 0) { free(m); return -1; }
  *uid = m;
  return 0;
}

int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x8000) {
    pthread_mutex_destroy(*uid);
    free(*uid);
    *uid = NULL;
  }
  return 0;
}

static int ensure_mutex(pthread_mutex_t **uid) {
  if (!*uid) return pthread_mutex_init_fake(uid, NULL);
  if ((uintptr_t)*uid == 0x4000) { int attr = 1; return pthread_mutex_init_fake(uid, &attr); }
  return 0;
}

int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  int ret = ensure_mutex(uid);
  if (ret < 0) return ret;
  return pthread_mutex_lock(*uid);
}

int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
  int ret = ensure_mutex(uid);
  if (ret < 0) return ret;
  return pthread_mutex_trylock(*uid);
}

int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  int ret = ensure_mutex(uid);
  if (ret < 0) return ret;
  return pthread_mutex_unlock(*uid);
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr) {
  (void)condattr;
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;
  if (pthread_cond_init(c, NULL) != 0) { free(c); return -1; }
  *cnd = c;
  return 0;
}

int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  return pthread_cond_broadcast(*cnd);
}

int pthread_cond_signal_fake(pthread_cond_t **cnd) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  return pthread_cond_signal(*cnd);
}

int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (cnd && *cnd) {
    pthread_cond_destroy(*cnd);
    free(*cnd);
    *cnd = NULL;
  }
  return 0;
}

int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  if (ensure_mutex(mtx) < 0) return -1;
  return pthread_cond_wait(*cnd, *mtx);
}

int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  if (ensure_mutex(mtx) < 0) return -1;
  return pthread_cond_timedwait(*cnd, *mtx, t);
}

int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine) return -1;
  if (__sync_lock_test_and_set(once_control, 1) == 0)
    (*init_routine)();
  return 0;
}

int pthread_mutexattr_init_fake(int *attr) { if (attr) *attr = 0; return 0; }
int pthread_mutexattr_settype_fake(int *attr, int type) { if (attr) *attr = type; return 0; }

// engine worker threads must point tpidr_el0 at a stack-guard block before
// running any guarded engine code (see tls_setup_guard).
typedef struct { void *(*entry)(void *); void *arg; } ThreadStart;

static void *thread_trampoline(void *p) {
  ThreadStart ts = *(ThreadStart *)p;
  free(p);
  tls_setup_guard();
  return ts.entry(ts.arg);
}

int pthread_create_fake(pthread_t *thread, const void *unused, void *entry, void *arg) {
  (void)unused;
  ThreadStart *ts = malloc(sizeof(*ts));
  if (!ts) return -1;
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;
  return pthread_create(thread, NULL, thread_trampoline, ts);
}

// ---------------------------------------------------------------------------
// small misc shims
// ---------------------------------------------------------------------------

static int getpid_fake(void) { return 1; }
static int sched_yield_fake(void) { svcSleepThread(0); return 0; }

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

DynLibFunction dynlib_functions[] = {
  // --- bionic / fortify -----------------------------------------------------
  { "__sF", (uintptr_t)&fake_sF },
  { "stdin", (uintptr_t)&fake_sF[0] },
  { "stdout", (uintptr_t)&fake_sF[1] },
  { "stderr", (uintptr_t)&fake_sF[2] },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__cxa_thread_atexit_impl", (uintptr_t)&__cxa_thread_atexit_impl_fake },
  { "__errno", (uintptr_t)&__errno },
  { "__assert2", (uintptr_t)&__assert2 },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
  { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__memset_chk", (uintptr_t)&__memset_chk_fake },
  { "__strcat_chk", (uintptr_t)&__strcat_chk_fake },
  { "__strchr_chk", (uintptr_t)&__strchr_chk_fake },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__strncat_chk", (uintptr_t)&__strncat_chk_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_fake },
  { "__android_log_print", (uintptr_t)&__android_log_print },
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "__register_atfork", (uintptr_t)&__register_atfork_fake },
  { "__open_2", (uintptr_t)&__open_2_fake },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "syscall", (uintptr_t)&syscall_fake },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "openlog", (uintptr_t)&ret0 },
  { "closelog", (uintptr_t)&ret0 },
  { "syslog", (uintptr_t)&ret0 },
  { "abort", (uintptr_t)&abort },
  { "exit", (uintptr_t)&ret0 }, // the engine never asks libc to exit cleanly

  // --- AAsset NDK API (served from the loose assets/ tree, see libc_shim.c) --
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake },
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open_fake },
  { "AAssetManager_openDir", (uintptr_t)&AAssetManager_openDir_fake },
  { "AAssetDir_getNextFileName", (uintptr_t)&AAssetDir_getNextFileName_fake },
  { "AAssetDir_close", (uintptr_t)&AAssetDir_close_fake },
  { "AAsset_close", (uintptr_t)&AAsset_close_fake },
  { "AAsset_getLength", (uintptr_t)&AAsset_getLength_fake },
  { "AAsset_read", (uintptr_t)&AAsset_read_fake },
  { "AAsset_seek", (uintptr_t)&AAsset_seek_fake },

  // --- memory ---------------------------------------------------------------
  { "malloc", (uintptr_t)&malloc },
  { "calloc", (uintptr_t)&calloc },
  { "realloc", (uintptr_t)&realloc },
  { "free", (uintptr_t)&free },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },

  // --- mem/str --------------------------------------------------------------
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "strcat", (uintptr_t)&strcat },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcoll", (uintptr_t)&strcoll },
  { "strcpy", (uintptr_t)&strcpy },
  { "strdup", (uintptr_t)&strdup },
  { "strerror", (uintptr_t)&strerror },
  { "strerror_r", (uintptr_t)&strerror_r_fake },
  { "strlcpy", (uintptr_t)&strlcpy },
  { "strlen", (uintptr_t)&strlen },
  { "strncmp", (uintptr_t)&strncmp },
  { "strpbrk", (uintptr_t)&strpbrk },
  { "strspn", (uintptr_t)&strspn },
  { "strstr", (uintptr_t)&strstr },
  { "strtod", (uintptr_t)&strtod },
  { "strtof", (uintptr_t)&strtof },
  { "strtol", (uintptr_t)&strtol },
  { "strtold", (uintptr_t)&strtold },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull },
  { "atoi", (uintptr_t)&atoi },
  { "qsort", (uintptr_t)&qsort },
  { "rand", (uintptr_t)&rand },
  { "srand", (uintptr_t)&srand },
  { "getenv", (uintptr_t)&getenv },
  { "setlocale", (uintptr_t)&setlocale },
  { "localeconv", (uintptr_t)&localeconv },
  { "system", (uintptr_t)&system_fake },

  // --- locale (_l variants: ignore the locale handle) -----------------------
  { "newlocale", (uintptr_t)&newlocale_fake },
  { "freelocale", (uintptr_t)&freelocale_fake },
  { "uselocale", (uintptr_t)&uselocale_fake },
  { "iswalpha_l", (uintptr_t)&iswalpha_l_fake },
  { "iswblank_l", (uintptr_t)&iswblank_l_fake },
  { "iswcntrl_l", (uintptr_t)&iswcntrl_l_fake },
  { "iswdigit_l", (uintptr_t)&iswdigit_l_fake },
  { "iswlower_l", (uintptr_t)&iswlower_l_fake },
  { "iswprint_l", (uintptr_t)&iswprint_l_fake },
  { "iswpunct_l", (uintptr_t)&iswpunct_l_fake },
  { "iswspace_l", (uintptr_t)&iswspace_l_fake },
  { "iswupper_l", (uintptr_t)&iswupper_l_fake },
  { "iswxdigit_l", (uintptr_t)&iswxdigit_l_fake },
  { "towlower_l", (uintptr_t)&towlower_l_fake },
  { "towupper_l", (uintptr_t)&towupper_l_fake },
  { "strcoll_l", (uintptr_t)&strcoll_l_fake },
  { "strxfrm_l", (uintptr_t)&strxfrm_l_fake },
  { "strftime_l", (uintptr_t)&strftime_l_fake },
  { "strtold_l", (uintptr_t)&strtold_l_fake },
  { "strtoll_l", (uintptr_t)&strtoll_l_fake },
  { "strtoull_l", (uintptr_t)&strtoull_l_fake },
  { "wcscoll_l", (uintptr_t)&wcscoll_l_fake },
  { "wcsxfrm_l", (uintptr_t)&wcsxfrm_l_fake },

  // --- wide char ------------------------------------------------------------
  { "wcslen", (uintptr_t)&wcslen },
  { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof },
  { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold },
  { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul },
  { "wcstoull", (uintptr_t)&wcstoull },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wctob", (uintptr_t)&wctob },
  { "btowc", (uintptr_t)&btowc },
  { "mbrlen", (uintptr_t)&mbrlen },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "mbtowc", (uintptr_t)&mbtowc },
  { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs_fake },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs_fake },

  // --- printf / scanf family ------------------------------------------------
  { "snprintf", (uintptr_t)&snprintf },
  { "swprintf", (uintptr_t)&swprintf },
  { "sscanf", (uintptr_t)&sscanf },
  { "vsscanf", (uintptr_t)&vsscanf },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf },
  { "vasprintf", (uintptr_t)&vasprintf },

  // --- math -----------------------------------------------------------------
  { "acos", (uintptr_t)&acos }, { "acosf", (uintptr_t)&acosf },
  { "asin", (uintptr_t)&asin }, { "asinf", (uintptr_t)&asinf },
  { "atan", (uintptr_t)&atan }, { "atan2", (uintptr_t)&atan2 },
  { "atan2f", (uintptr_t)&atan2f },
  { "cos", (uintptr_t)&cos }, { "cosf", (uintptr_t)&cosf },
  { "sin", (uintptr_t)&sin }, { "sinf", (uintptr_t)&sinf },
  { "sincos", (uintptr_t)&sincos_fake }, { "sincosf", (uintptr_t)&sincosf_fake },
  { "tan", (uintptr_t)&tan }, { "tanf", (uintptr_t)&tanf },
  { "exp", (uintptr_t)&exp }, { "expf", (uintptr_t)&expf },
  { "log", (uintptr_t)&log }, { "logf", (uintptr_t)&logf },
  { "log10", (uintptr_t)&log10 }, { "log10f", (uintptr_t)&log10f },
  { "pow", (uintptr_t)&pow }, { "fmod", (uintptr_t)&fmod },
  { "ldexp", (uintptr_t)&ldexp }, { "frexp", (uintptr_t)&frexp },

  // --- time -----------------------------------------------------------------
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "clock_gettime", (uintptr_t)&clock_gettime },
  { "clock", (uintptr_t)&clock },
  { "time", (uintptr_t)&time },
  { "difftime", (uintptr_t)&difftime },
  { "mktime", (uintptr_t)&mktime },
  { "gmtime", (uintptr_t)&gmtime },
  { "localtime", (uintptr_t)&localtime },
  { "strftime", (uintptr_t)&strftime },
  { "nanosleep", (uintptr_t)&nanosleep },
  { "usleep", (uintptr_t)&usleep },

  // --- stdio (over the fake bionic __sF + buffered fopen) -------------------
  { "fopen", (uintptr_t)&fopen_fake },
  { "freopen", (uintptr_t)&freopen },
  { "fclose", (uintptr_t)&fclose_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "fseeko", (uintptr_t)&fseeko },
  { "ftell", (uintptr_t)&ftell },
  { "ftello", (uintptr_t)&ftello },
  { "fflush", (uintptr_t)&fflush_fake },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "fputc", (uintptr_t)&fputc_fake },
  { "fputwc", (uintptr_t)&fputwc },
  { "fgets", (uintptr_t)&fgets },
  { "getc", (uintptr_t)&getc_fake },
  { "getwc", (uintptr_t)&getwc },
  { "ungetc", (uintptr_t)&ungetc_fake },
  { "ungetwc", (uintptr_t)&ungetwc },
  { "feof", (uintptr_t)&feof },
  { "ferror", (uintptr_t)&ferror_fake },
  { "fileno", (uintptr_t)&fileno_fake },
  { "clearerr", (uintptr_t)&clearerr },
  { "setvbuf", (uintptr_t)&setvbuf },
  { "vfprintf", (uintptr_t)&vfprintf_fake },
  { "tmpfile", (uintptr_t)&tmpfile },
  { "tmpnam", (uintptr_t)&tmpnam },
  { "remove", (uintptr_t)&remove },
  { "rename", (uintptr_t)&rename },

  // --- low-level fs (raw fds) -----------------------------------------------
  { "open", (uintptr_t)&open_fake },
  { "openat", (uintptr_t)&openat_fake },
  { "close", (uintptr_t)&close },
  { "read", (uintptr_t)&read },
  { "write", (uintptr_t)&write },
  { "unlinkat", (uintptr_t)&unlinkat_fake },
  { "stat", (uintptr_t)&stat_fake },
  { "fstat", (uintptr_t)&fstat_fake },
  { "lstat", (uintptr_t)&lstat_fake },
  { "mkdir", (uintptr_t)&mkdir_fake },
  { "opendir", (uintptr_t)&opendir },
  { "fdopendir", (uintptr_t)&fdopendir_fake },
  { "readdir", (uintptr_t)&readdir_fake },
  { "closedir", (uintptr_t)&closedir },
  { "getcwd", (uintptr_t)&getcwd },
  { "chdir", (uintptr_t)&ret0 },
  { "realpath", (uintptr_t)&realpath_fake },
  { "statvfs", (uintptr_t)&statvfs_fake },
  { "ftruncate", (uintptr_t)&ftruncate_fake },
  { "truncate", (uintptr_t)&truncate_fake },
  { "ioctl", (uintptr_t)&retm1 },
  { "isatty", (uintptr_t)&ret0 },
  { "fchmod", (uintptr_t)&ret0 },
  { "fchmodat", (uintptr_t)&ret0 },
  { "link", (uintptr_t)&retm1 },
  { "symlink", (uintptr_t)&retm1 },
  { "readlink", (uintptr_t)&retm1 },
  { "sendfile", (uintptr_t)&retm1 },
  { "utimensat", (uintptr_t)&ret0 },
  { "pathconf", (uintptr_t)&pathconf_fake },
  { "sysconf", (uintptr_t)&sysconf_fake },

  // --- zlib (devkitPro -lz) -------------------------------------------------
  { "crc32", (uintptr_t)&crc32 },
  { "deflate", (uintptr_t)&deflate },
  { "deflateEnd", (uintptr_t)&deflateEnd },
  { "deflateInit2_", (uintptr_t)&deflateInit2_ },
  { "deflateReset", (uintptr_t)&deflateReset },
  { "inflate", (uintptr_t)&inflate },
  { "inflateEnd", (uintptr_t)&inflateEnd },
  { "inflateInit_", (uintptr_t)&inflateInit_ },
  { "inflateReset", (uintptr_t)&inflateReset },

  // --- setjmp / misc --------------------------------------------------------
  { "setjmp", (uintptr_t)&setjmp },
  { "longjmp", (uintptr_t)&longjmp },
  { "getpid", (uintptr_t)&getpid_fake },
  { "sched_yield", (uintptr_t)&sched_yield_fake },

  // --- pthread --------------------------------------------------------------
  { "pthread_create", (uintptr_t)&pthread_create_fake },
  { "pthread_join", (uintptr_t)&pthread_join },
  { "pthread_detach", (uintptr_t)&pthread_detach },
  { "pthread_exit", (uintptr_t)&pthread_exit },
  { "pthread_self", (uintptr_t)&pthread_self },
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake },
  { "pthread_mutexattr_destroy", (uintptr_t)&ret0 },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },

  // --- GLES2 (mesa libGLESv2) ----------------------------------------------
  { "glActiveTexture", (uintptr_t)&glActiveTexture },
  { "glAttachShader", (uintptr_t)&glAttachShader },
  { "glBindBuffer", (uintptr_t)&glBindBuffer },
  { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
  { "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer },
  { "glBindTexture", (uintptr_t)&glBindTexture },
  { "glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate },
  { "glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate },
  { "glBufferData", (uintptr_t)&glBufferData },
  { "glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus },
  { "glClearColor", (uintptr_t)&glClearColor },
  { "glClear", (uintptr_t)&glClear },
  { "glClearDepthf", (uintptr_t)&glClearDepthf },
  { "glColorMask", (uintptr_t)&glColorMask },
  { "glCompileShader", (uintptr_t)&glCompileShader },
  { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D },
  { "glCreateProgram", (uintptr_t)&glCreateProgram },
  { "glCreateShader", (uintptr_t)&glCreateShader },
  { "glCullFace", (uintptr_t)&glCullFace },
  { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
  { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
  { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
  { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers },
  { "glDeleteShader", (uintptr_t)&glDeleteShader },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures },
  { "glDepthFunc", (uintptr_t)&glDepthFunc },
  { "glDepthMask", (uintptr_t)&glDepthMask },
  { "glDisable", (uintptr_t)&glDisable },
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
  { "glDrawArrays", (uintptr_t)&glDrawArrays },
  { "glDrawElements", (uintptr_t)&glDrawElements },
  { "glEnable", (uintptr_t)&glEnable },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
  { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
  { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
  { "glFrontFace", (uintptr_t)&glFrontFace },
  { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
  { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
  { "glGenTextures", (uintptr_t)&glGenTextures },
  { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
  { "glGetError", (uintptr_t)&glGetError },
  { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
  { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
  { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
  { "glGetShaderiv", (uintptr_t)&glGetShaderiv },
  { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog },
  { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
  { "glLinkProgram", (uintptr_t)&glLinkProgram },
  { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage },
  { "glScissor", (uintptr_t)&glScissor },
  { "glShaderSource", (uintptr_t)&glShaderSource },
  { "glTexImage2D", (uintptr_t)&glTexImage2D },
  { "glTexParameteri", (uintptr_t)&glTexParameteri },
  { "glTexSubImage2D", (uintptr_t)&glTexSubImage2D },
  { "glUniform1i", (uintptr_t)&glUniform1i },
  { "glUniform4fv", (uintptr_t)&glUniform4fv },
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv },
  { "glUseProgram", (uintptr_t)&glUseProgram },
  { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer },
  { "glViewport", (uintptr_t)&glViewport },

  // --- OpenSLES (minimal SDL2-backed shim, see opensles.c) ------------------
  { "slCreateEngine", (uintptr_t)&slCreateEngine },
  #define SL_IID(n) { "SL_IID_" #n, (uintptr_t)&SL_IID_##n }
  SL_IID(BUFFERQUEUE), SL_IID(EFFECTSEND), SL_IID(ENGINE),
  SL_IID(PLAY), SL_IID(PLAYBACKRATE), SL_IID(VOLUME),
  SL_IID(ANDROIDSIMPLEBUFFERQUEUE), SL_IID(OUTPUTMIX), SL_IID(OBJECT),
  #undef SL_IID
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void aom_resolve_imports(so_module *cxx_mod, so_module *game_mod) {
  // C++ runtime first, so the engine's std::/RTTI imports resolve from it.
  if (cxx_mod) {
    so_relocate(cxx_mod);
    so_resolve(cxx_mod, dynlib_functions, dynlib_numfunctions, 1);
  }
  so_relocate(game_mod);
  so_resolve(game_mod, dynlib_functions, dynlib_numfunctions, 1);
}
