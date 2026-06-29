/* jni_fake.c -- fake JNI environment for the MCF/Si engine (libmcfandroid.so)
 *
 * The engine (mcfplandroid_jni.cpp) caches the JNIEnv + MainActivity class and
 * drives all platform services through static MainActivity.func* callbacks. We
 * provide a functional JNIEnv so GetStaticMethodID + CallStatic*Method resolve
 * to native C implementations here, plus the MCFLib_Sensor int[] builder that
 * pushSensor() consumes.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "jni_fake.h"
#include "gfx.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

// ---------------------------------------------------------------------------
// fake object model
// ---------------------------------------------------------------------------

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'  heap object (freeable)
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_PRIARR = 0x50415231, // 'PAR1'
  TAG_ID     = 0x4d494431, // 'MID1'  pooled, never freed
  TAG_CLASS  = 0x434c5331, // 'CLS1'  singleton, never freed
};

typedef struct { uint32_t tag; char label[64]; } FakeObject;
typedef struct { uint32_t tag; char *utf; } FakeString;
typedef struct { uint32_t tag; int len; void **items; } FakeObjArray;
typedef struct { uint32_t tag; int len; int elem_size; void *data; } FakePriArray;
typedef struct { uint32_t tag; char name[64]; char sig[96]; } FakeID;

volatile int jni_quit_requested = 0;

// ---------------------------------------------------------------------------
// local reference registry (the engine brackets JNI use with DeleteLocalRef /
// Push+PopLocalFrame, which we honour so transient strings/arrays don't leak)
// ---------------------------------------------------------------------------

#define MAX_LOCALS 16384
#define MAX_FRAMES 64
static void *locals[MAX_LOCALS];
static int locals_top = 0;
static int frames[MAX_FRAMES];
static int frame_top = 0;
static Mutex locals_lock;

static void *reg_local(void *ref) {
  if (ref) {
    mutexLock(&locals_lock);
    if (locals_top < MAX_LOCALS)
      locals[locals_top++] = ref;
    else
      debugPrintf("JNI: local ref table full, leaking %p\n", ref);
    mutexUnlock(&locals_lock);
  }
  return ref;
}

static void free_ref(void *ref) {
  if (!ref)
    return;
  switch (*(uint32_t *)ref) {
    case TAG_STRING: { FakeString *s = ref; free(s->utf); free(s); break; }
    case TAG_PRIARR: { FakePriArray *a = ref; free(a->data); free(a); break; }
    case TAG_OBJARR: { FakeObjArray *a = ref; free(a->items); free(a); break; }
    case TAG_OBJECT: free(ref); break;
    default: break; // TAG_ID / TAG_CLASS are never freed
  }
}

static void delete_local(void *ref) {
  if (!ref)
    return;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--) {
    if (locals[i] == ref) {
      locals[i] = locals[--locals_top];
      free_ref(ref);
      break;
    }
  }
  mutexUnlock(&locals_lock);
}

// ---------------------------------------------------------------------------
// fake object constructors
// ---------------------------------------------------------------------------

void *jni_make_string(const char *utf) {
  FakeString *s = calloc(1, sizeof(*s));
  s->tag = TAG_STRING;
  s->utf = strdup(utf ? utf : "");
  return reg_local(s);
}

static void *make_pri_array_adopt(void *data, int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = len;
  a->elem_size = elem_size;
  a->data = data;
  return reg_local(a);
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING)
    return s->utf;
  return "";
}

// singletons (never freed)
static FakeObject *g_class = NULL;   // MainActivity class
static FakeObject *g_thiz = NULL;    // MainActivity instance / Activity
static FakeObject *g_assetmgr = NULL;

void *jni_make_thiz(void) {
  if (!g_thiz) {
    g_thiz = calloc(1, sizeof(*g_thiz));
    g_thiz->tag = TAG_CLASS;
    strcpy(g_thiz->label, "MainActivity");
  }
  return g_thiz;
}

void *jni_make_asset_manager(void) {
  if (!g_assetmgr) {
    g_assetmgr = calloc(1, sizeof(*g_assetmgr));
    g_assetmgr->tag = TAG_CLASS;
    strcpy(g_assetmgr->label, "AssetManager");
  }
  return g_assetmgr;
}

static void *get_class(void) {
  if (!g_class) {
    g_class = calloc(1, sizeof(*g_class));
    g_class->tag = TAG_CLASS;
    strcpy(g_class->label, "MainActivityClass");
  }
  return g_class;
}

// ---------------------------------------------------------------------------
// method/field ID pool
// ---------------------------------------------------------------------------

#define MAX_IDS 256
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *name, const char *sig) {
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig))
      return &id_pool[i];
  if (id_count >= MAX_IDS) {
    debugPrintf("JNI: id pool exhausted (%s)\n", name);
    return &id_pool[0];
  }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->name, name, sizeof(id->name) - 1);
  strncpy(id->sig, sig, sizeof(id->sig) - 1);
  return id;
}

// ---------------------------------------------------------------------------
// device language reported to the engine (funcGetDeviceLanguage)
// ---------------------------------------------------------------------------

static const char *device_language_string(void) {
  static const char *langs[] = {
    "ja", "en", "fr", "de", "it", "es", "ko", "zh_CN", "zh_TW"
  };
  const int n = (int)(sizeof(langs) / sizeof(*langs));
  const int li = (config.language >= 0 && config.language < n) ? config.language : LANG_EN;
  return langs[li];
}

// ---------------------------------------------------------------------------
// font rendering for funcFontWidth / funcFontHeight / funcFontDrawStringToImage
// ---------------------------------------------------------------------------

// Rasterize `text` at `size` into the w*h ARGB destination, tinted (r,g,b,a)
// (the engine bakes the colour into the bitmap, so we recolour the glyphs).
static void font_draw_into(FakePriArray *dst, int w, int h, int size,
                           int r, int g, int b, int a, const char *text) {
  if (!dst || dst->tag != TAG_PRIARR || !dst->data)
    return;
  if (w <= 0 || h <= 0)
    return;
  int count = 0;
  int *pix = gfx_draw_font(text, size > 0 ? size : 16, w, h, &count);
  if (!pix)
    return;
  const int pixels = w * h;
  int n = (count > 1) ? count - 1 : 0;
  if (n > pixels) n = pixels;
  if (n > dst->len) n = dst->len;
  int *out = (int *)dst->data;
  const int cr = r & 0xff, cg = g & 0xff, cb = b & 0xff, ca = a & 0xff;
  for (int i = 0; i < n; i++) {
    const int cov = (pix[1 + i] >> 24) & 0xff;   // glyph coverage (white alpha)
    const int oa = (cov * ca) / 255;             // modulate by paint alpha
    out[i] = (oa << 24) | (cr << 16) | (cg << 8) | cb;
  }
  free(pix);
}

// ---------------------------------------------------------------------------
// text input (hero name etc.): funcViewDialog(type==1,...) pops the libnx
// software keyboard synchronously; funcViewDialogGetString returns the result.
// ---------------------------------------------------------------------------

static char g_kbd_result[256] = "";

static void show_switch_keyboard(const char *header, const char *initial) {
  SwkbdConfig kbd;
  char out[sizeof(g_kbd_result)];
  if (R_FAILED(swkbdCreate(&kbd, 0))) {
    debugPrintf("swkbd: create failed\n");
    g_kbd_result[0] = 0;
    return;
  }
  swkbdConfigMakePresetDefault(&kbd);
  if (header && header[0])
    swkbdConfigSetHeaderText(&kbd, header);
  swkbdConfigSetInitialText(&kbd, initial ? initial : "");
  swkbdConfigSetStringLenMax(&kbd, (u32)sizeof(out) - 1);
  Result rc = swkbdShow(&kbd, out, sizeof(out));
  swkbdClose(&kbd);
  if (R_SUCCEEDED(rc))
    snprintf(g_kbd_result, sizeof(g_kbd_result), "%s", out);
  else
    g_kbd_result[0] = 0; // cancelled
}

// ---------------------------------------------------------------------------
// MainActivity.func* dispatch (by name; static methods)
// ---------------------------------------------------------------------------

static juint call_int(const char *name, va_list va) {
  if (!strcmp(name, "funcFontWidth")) {
    // funcFontWidth(p0=face/flags, p1=size, p2=text); size is the 2nd int
    (void)va_arg(va, int);                 // p0 (unused)
    const int size = va_arg(va, int);      // p1 = size
    const char *text = obj_str(va_arg(va, void *)); // p2
    return (juint)gfx_measure_text_width(text, size > 0 ? size : 16);
  }
  if (!strcmp(name, "funcFontHeight")) {
    (void)va_arg(va, int);                 // p0 (unused)
    const int size = va_arg(va, int);      // p1 = size
    (void)obj_str(va_arg(va, void *));     // p2
    return (juint)(size > 0 ? size : 16);  // line height ~= pixel size
  }
  if (!strcmp(name, "funcGetAppVersionCode")) return 1;
  if (!strcmp(name, "funcGetStereoDevice"))   return 0;
  if (!strcmp(name, "funcSetStereoDevice"))   { (void)va_arg(va, int); return 0; }
  if (!strcmp(name, "funcIsDeviceAndroidTV")) return 0;
  if (!strcmp(name, "funcAchievementIsSignedIn")) return 0;
  if (!strcmp(name, "funcViewDialog")) {
    // (p0 type: 1=text input, 0=alert; p1 flag; p2 title; p3 msg; p4 default)
    const int type      = va_arg(va, int);
    (void)va_arg(va, int);
    const char *title   = obj_str(va_arg(va, void *));
    (void)obj_str(va_arg(va, void *));
    const char *initial = obj_str(va_arg(va, void *));
    if (type == 1)
      show_switch_keyboard(title, initial); // hero-name etc. -> system keyboard
    else
      g_kbd_result[0] = 0;                  // plain alert: nothing to return
    return 1;                               // dialog handled
  }
  if (!strcmp(name, "funcViewDialogIsDone"))  return 1; // swkbd ran synchronously
  if (!strcmp(name, "GetCloudSaveAlive"))     return 0;
  (void)va;
  return 0;
}

static juint call_long(const char *name, va_list va) {
  (void)va;
  // memory queries: report a generous, fixed pool
  if (!strcmp(name, "funcDeviceMaxMemory"))   return 768ull * 1024 * 1024;
  if (!strcmp(name, "funcDeviceTotalMemory")) return 768ull * 1024 * 1024;
  if (!strcmp(name, "funcDeviceFreeMemory"))  return 384ull * 1024 * 1024;
  if (!strcmp(name, "funcNativeTotalMemory")) return (juint)(MEMORY_MB * 1024ull * 1024);
  if (!strcmp(name, "funcNativeFreeMemory"))  return (juint)(MEMORY_MB * 1024ull * 1024 / 2);
  return 0;
}

static void *call_object(const char *name, va_list va) {
  (void)va;
  if (!strcmp(name, "funcGetDeviceLanguage")) return jni_make_string(device_language_string());
  if (!strcmp(name, "funcGetDeviceName"))     return jni_make_string("Nintendo Switch");
  if (!strcmp(name, "funcGetAppVersionName")) return jni_make_string("1.1.4");
  // MUST be empty: a non-empty path puts the engine in "OBB mode" (opens it as
  // the archive); empty -> "assets mode", reading sk1/sk1.mpk via AAssetManager.
  if (!strcmp(name, "funcGetObbFilePath"))    return jni_make_string("");
  if (!strcmp(name, "funcGetObbMountedPath")) return jni_make_string("");
  if (!strcmp(name, "funcViewDialogGetString")) return jni_make_string(g_kbd_result);
  return NULL;
}

static void call_void(const char *name, va_list va) {
  if (!strcmp(name, "funcApplicationExit")) { jni_quit_requested = 1; return; }
  if (!strcmp(name, "funcFontDrawStringToImage")) {
    // ([I dst, p1=w, p2=h, p3, p4=size, p5, p6, p7=R, p8=G, p9=B, p10=A, String)
    // arg meaning recovered from MainActivity.funcFontDrawStringToImage:
    //   createBitmap(p1,p2); setTextSize(p4); setARGB(p10,p7,p8,p9)
    FakePriArray *dst = va_arg(va, void *);
    const int w    = va_arg(va, int); // p1
    const int h    = va_arg(va, int); // p2
    (void)va_arg(va, int);            // p3 (unused)
    const int size = va_arg(va, int); // p4
    (void)va_arg(va, int);            // p5 (unused)
    (void)va_arg(va, int);            // p6 (unused)
    const int r = va_arg(va, int);    // p7
    const int g = va_arg(va, int);    // p8
    const int b = va_arg(va, int);    // p9
    const int a = va_arg(va, int);    // p10
    const char *text = obj_str(va_arg(va, void *)); // p11
    font_draw_into(dst, w, h, size, r, g, b, a, text);
    return;
  }
  // fire-and-forget stubs
  if (!strcmp(name, "funcBackButtonAppCapture") ||
      !strcmp(name, "funcSetStereoDevice")) { (void)va_arg(va, int); return; }
  // funcCloseDialog / funcStartCloudSave / funcLaunchBrowser /
  // funcMediaScannerConnection_scanFile / funcAchievement* : no-ops
  (void)va;
}

// ---------------------------------------------------------------------------
// JNIEnv function implementations
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }
static void *j_FindClass(void *env, const char *name) { (void)env; (void)name; return get_class(); }
static void *j_GetObjectClass(void *env, void *obj) { (void)env; (void)obj; return get_class(); }
static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; (void)cls; return get_id(name, sig);
}

static void *j_NewGlobalRef(void *env, void *obj) {
  (void)env;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--)
    if (locals[i] == obj) { locals[i] = locals[--locals_top]; break; }
  mutexUnlock(&locals_lock);
  return obj;
}
static void j_DeleteGlobalRef(void *env, void *obj) { (void)env; free_ref(obj); }
static void j_DeleteLocalRef(void *env, void *obj) { (void)env; delete_local(obj); }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static juint j_IsSameObject(void *env, void *a, void *b) { (void)env; return a == b; }
static juint j_EnsureLocalCapacity(void *env, int cap) { (void)env; (void)cap; return 0; }

static juint j_PushLocalFrame(void *env, int cap) {
  (void)env; (void)cap;
  mutexLock(&locals_lock);
  if (frame_top < MAX_FRAMES)
    frames[frame_top++] = locals_top;
  mutexUnlock(&locals_lock);
  return 0;
}
static void *j_PopLocalFrame(void *env, void *result) {
  (void)env;
  mutexLock(&locals_lock);
  const int mark = frame_top > 0 ? frames[--frame_top] : 0;
  for (int i = mark; i < locals_top; i++)
    if (locals[i] != result)
      free_ref(locals[i]);
  locals_top = mark;
  if (result && locals_top < MAX_LOCALS)
    locals[locals_top++] = result;
  mutexUnlock(&locals_lock);
  return result;
}

// --- Call<type>Method (instance + static share name dispatch) ---------------

#define CALL_VARIADIC(fn, ret_t, dispatch) \
  static ret_t fn(void *env, void *recv, FakeID *id, ...) { \
    (void)env; (void)recv; va_list va; va_start(va, id); \
    ret_t r = dispatch(id->name, va); va_end(va); return r; } \
  static ret_t fn##V(void *env, void *recv, FakeID *id, va_list va) { \
    (void)env; (void)recv; return dispatch(id->name, va); }

CALL_VARIADIC(j_CallObjectMethod, void *, call_object)
CALL_VARIADIC(j_CallIntMethod, juint, call_int)
CALL_VARIADIC(j_CallBooleanMethod, juint, call_int)
CALL_VARIADIC(j_CallLongMethod, juint, call_long)

static void j_CallVoidMethod(void *env, void *recv, FakeID *id, ...) {
  (void)env; (void)recv; va_list va; va_start(va, id); call_void(id->name, va); va_end(va);
}
static void j_CallVoidMethodV(void *env, void *recv, FakeID *id, va_list va) {
  (void)env; (void)recv; call_void(id->name, va);
}

#define j_CallStaticObjectMethod   j_CallObjectMethod
#define j_CallStaticObjectMethodV  j_CallObjectMethodV
#define j_CallStaticIntMethod      j_CallIntMethod
#define j_CallStaticIntMethodV     j_CallIntMethodV
#define j_CallStaticBooleanMethod  j_CallBooleanMethod
#define j_CallStaticBooleanMethodV j_CallBooleanMethodV
#define j_CallStaticLongMethod     j_CallLongMethod
#define j_CallStaticLongMethodV    j_CallLongMethodV
#define j_CallStaticVoidMethod     j_CallVoidMethod
#define j_CallStaticVoidMethodV    j_CallVoidMethodV

// --- strings ----------------------------------------------------------------

static void *j_NewStringUTF(void *env, const char *utf) { (void)env; return jni_make_string(utf); }
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0; return obj_str(jstr);
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) { (void)env; (void)jstr; (void)utf; }
static juint j_GetStringUTFLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }
static juint j_GetStringLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }

// --- arrays -----------------------------------------------------------------

static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *a = arr; // len at same offset in both array structs
  if (a && (a->tag == TAG_PRIARR || a->tag == TAG_OBJARR))
    return a->len;
  return 0;
}

static void *new_pri_array(int len, int elem_size) {
  void *data = calloc(len ? len : 1, elem_size);
  return make_pri_array_adopt(data, len, elem_size);
}
static void *j_NewByteArray(void *env, int len) { (void)env; return new_pri_array(len, 1); }
static void *j_NewIntArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }
static void *j_NewFloatArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  FakePriArray *a = arr;
  return (a && a->tag == TAG_PRIARR) ? a->data : NULL;
}
static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetPriArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy(buf, (char *)a->data + (size_t)start * a->elem_size, (size_t)len * a->elem_size);
}
static void j_SetPriArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy((char *)a->data + (size_t)start * a->elem_size, buf, (size_t)len * a->elem_size);
}

// --- misc -------------------------------------------------------------------

static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env; (void)cls; (void)methods; (void)n; return 0;
}
static juint j_GetJavaVM(void *env, void **vm) { (void)env; *vm = fake_vm; return JNI_OK; }
static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void j_void1(void *env) { (void)env; }
static juint j_unimplemented(void) {
  debugPrintf("JNI: unimplemented slot called\n");
  return 0;
}

// ---------------------------------------------------------------------------
// table assembly (indices per the JNI specification)
// ---------------------------------------------------------------------------

static void *env_table[233];
static void **env_table_ptr = env_table;
void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args; if (env) *env = fake_env; return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version; if (env) *env = fake_env; return JNI_OK;
}
static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  mutexInit(&locals_lock);

  for (int i = 0; i < 233; i++)
    env_table[i] = (void *)j_unimplemented;

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void1; // ExceptionDescribe
  env_table[17]  = (void *)j_void1; // ExceptionClear
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteGlobalRef;
  env_table[23]  = (void *)j_DeleteLocalRef;
  env_table[24]  = (void *)j_IsSameObject;
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_EnsureLocalCapacity;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[53]  = (void *)j_CallLongMethod;
  env_table[54]  = (void *)j_CallLongMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  env_table[113] = (void *)j_GetMethodID;            // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[132] = (void *)j_CallStaticLongMethod;
  env_table[133] = (void *)j_CallStaticLongMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  env_table[144] = (void *)j_GetMethodID;            // GetStaticFieldID
  env_table[164] = (void *)j_GetStringLength;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[176] = (void *)j_NewByteArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[181] = (void *)j_NewFloatArray;
  for (int i = 183; i <= 190; i++) env_table[i] = (void *)j_GetPriArrayElements;
  for (int i = 191; i <= 198; i++) env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++) env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++) env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[219] = (void *)j_GetJavaVM;
  env_table[222] = (void *)j_GetPriArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePriArrayElements; // ReleasePrimitiveArrayCritical
  env_table[226] = (void *)j_NewGlobalRef;            // NewWeakGlobalRef
  env_table[227] = (void *)j_DeleteGlobalRef;         // DeleteWeakGlobalRef
  env_table[228] = (void *)j_ExceptionCheck;

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread;
}

// ---------------------------------------------------------------------------
// MCFLib_Sensor int[] builder consumed by pushSensor(). Layout (37 ints, from
// getIntArray() / disassembling pushSensor):
//   [0]key_now [1]key_last [2]key_on [3]key_off
//   [4]touch_ptr_max [5]touch_count [6]touch_last_ptr
//   [7]touch_now [8]touch_last [9]touch_on [10]touch_off [11]moving [12]move
//   [13]touch_max_x [14]touch_max_y
//   start_x[0..3]=[15..18] start_y=[19..22] move_x=[23..26] move_y=[27..30]
// Edges are derived here (Java did this in updateInput()).
// ---------------------------------------------------------------------------

#define SENSOR_LEN 37

void *jni_build_sensor_array(const AomInput *in) {
  static int key_last = 0;
  static int touch_last = 0;
  // one persistent array; pushSensor() reads it synchronously, so no per-frame alloc
  static int storage[SENSOR_LEN];
  static FakePriArray holder = { TAG_PRIARR, SENSOR_LEN, 4, storage };

  int *a = storage;
  memset(a, 0, sizeof(storage));

  const int key_now = in ? in->key_now : 0;
  a[0] = key_now;
  a[1] = key_last;
  a[2] = key_now & ~key_last;   // key_on_b  (newly pressed)
  a[3] = key_last & ~key_now;   // key_off_b (newly released)

  // pointer 0. The release handler reads move_x[0]/move_y[0], so on the release
  // frame report the *last* position there (else taps highlight but don't fire).
  static int last_x = 0, last_y = 0;
  static int start_x = 0, start_y = 0;

  const int down = (in && in->touch_count > 0) ? 1 : 0;
  int cx, cy;
  if (down) {
    cx = in->touch[0].x; cy = in->touch[0].y;
    if (!touch_last) { start_x = cx; start_y = cy; } // press: record start
    last_x = cx; last_y = cy;
  } else {
    cx = last_x; cy = last_y;  // release: report the last press position
  }

  a[4]  = AOM_MAX_TOUCH;                        // touch_ptr_max
  a[5]  = down;                                 // touch_count
  a[6]  = 0;                                    // touch_last_ptr
  a[7]  = down;                                 // touch_now_b  (bit0 = finger 0)
  a[8]  = touch_last;                           // touch_last_b
  a[9]  = down & ~touch_last;                   // touch_on_b   (press edge)
  a[10] = touch_last & ~down;                   // touch_off_b  (release edge)
  a[11] = down;                                 // touch_moving_b
  a[12] = down;                                 // touch_move_b
  a[13] = in ? in->touch_max_x : screen_width;  // touch_max_x
  a[14] = in ? in->touch_max_y : screen_height; // touch_max_y

  a[15] = start_x;   // start_x[0]
  a[19] = start_y;   // start_y[0]
  a[23] = cx;        // move_x[0]  (read by the engine on press/move/RELEASE)
  a[27] = cy;        // move_y[0]

  key_last = key_now;
  touch_last = down;

  return &holder; // persistent; not a registered local ref
}
