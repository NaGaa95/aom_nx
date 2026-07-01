/* main.c -- Adventure of Mana (SK1) Switch wrapper entry point
 *
 * Loads libc++_shared.so + libmcfandroid.so into a minimal emulated Android
 * environment, drives the MCFLibrary.MainActivity lifecycle, and feeds input
 * through pushSensor(). This software may be modified and distributed under the
 * terms of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "gfx.h"
#include "opensles.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;
size_t g_native_heap = 0; // engine heap size, reported via funcNative*Memory

so_module cxx_mod;   // libc++_shared.so
so_module game_mod;  // libmcfandroid.so

// provide a replacement heap init so the newlib heap is separate from the .so
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  // Reserve a slice for the two .so images and give everything else to the
  // newlib/engine heap.
  const size_t so_reserve = 128 * 1024 * 1024;
  fake_heap_size  = size > so_reserve * 2 ? size - so_reserve : size / 2;
  g_native_heap   = fake_heap_size;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");
}

static void check_data(void) {
  struct stat st;
  const char *files[] = { SO_NAME, CXX_SO_NAME, ASSETS_DIR "/sk1/sk1.mpk" };
  for (unsigned i = 0; i < sizeof(files) / sizeof(*files); i++)
    if (stat(files[i], &st) < 0)
      fatal_error("Could not find\n%s.\nCheck your data files.", files[i]);
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920;
      screen_height = 1080;
    } else {
      screen_width = 1280;
      screen_height = 720;
    }
  } else {
    screen_width = w;
    screen_height = h;
  }
}

// ---------------------------------------------------------------------------
// EGL / GLES2 context on the default NWindow
// ---------------------------------------------------------------------------

static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLSurface s_surface = EGL_NO_SURFACE;

static int egl_init(void) {
  s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!s_display) { debugPrintf("egl: no display\n"); return 0; }
  eglInitialize(s_display, NULL, NULL);
  if (!eglBindAPI(EGL_OPENGL_ES_API)) { debugPrintf("egl: bindAPI failed\n"); return 0; }

  const EGLint cfg_attr[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, // GLES2 (the engine uses shaders)
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };
  EGLConfig config;
  EGLint num = 0;
  if (!eglChooseConfig(s_display, cfg_attr, &config, 1, &num) || num < 1) {
    debugPrintf("egl: no config\n");
    return 0;
  }

  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  s_surface = eglCreateWindowSurface(s_display, config, win, NULL);
  if (!s_surface) { debugPrintf("egl: no surface\n"); return 0; }

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, ctx_attr);
  if (!s_context) { debugPrintf("egl: no context\n"); return 0; }

  eglMakeCurrent(s_display, s_surface, s_surface, s_context);
  eglSwapInterval(s_display, 1);
  return 1;
}

static void egl_deinit(void) {
  if (s_display == EGL_NO_DISPLAY)
    return;
  eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (s_context) eglDestroyContext(s_display, s_context);
  if (s_surface) eglDestroySurface(s_display, s_surface);
  eglTerminate(s_display);
  s_display = EGL_NO_DISPLAY;
}

// ---------------------------------------------------------------------------
// engine entry points (the MCFLibrary.MainActivity native methods + JNI_OnLoad)
// ---------------------------------------------------------------------------

#define MCF "Java_jp_co_mcf_android_MCFLibrary_MainActivity_"

static int  (*e_JNI_OnLoad)(void *vm, void *reserved);
static void (*e_JniConstruct)(void *env, void *cls, void *activity, void *path, void *am);
static void (*e_JniOnStart)(void *env, void *cls);
static void (*e_JniOnResume)(void *env, void *cls);
static void (*e_JniOnGLSurfaceChanged)(void *env, void *cls, int w, int h);
static void (*e_JniOnFrameUpdate)(void *env, void *cls);
static void (*e_JniOnFrameRender)(void *env, void *cls);
static void (*e_JniOnPause)(void *env, void *cls);
static void (*e_JniOnStop)(void *env, void *cls);
static void (*e_JniDestruct)(void *env, void *cls);
static void (*e_pushSensor)(void *env, void *cls, void *arr);
static void (*e_setDownloadPath)(void *env, void *cls, void *path);

static void resolve_entry_points(void) {
  e_JNI_OnLoad           = (void *)so_try_find_addr_rx(&game_mod, "JNI_OnLoad");
  e_JniConstruct         = (void *)so_find_addr_rx(&game_mod, MCF "JniConstruct");
  e_JniOnStart           = (void *)so_find_addr_rx(&game_mod, MCF "JniOnStart");
  e_JniOnResume          = (void *)so_find_addr_rx(&game_mod, MCF "JniOnResume");
  e_JniOnGLSurfaceChanged= (void *)so_find_addr_rx(&game_mod, MCF "JniOnGLSurfaceChanged");
  e_JniOnFrameUpdate     = (void *)so_find_addr_rx(&game_mod, MCF "JniOnFrameUpdate");
  e_JniOnFrameRender     = (void *)so_find_addr_rx(&game_mod, MCF "JniOnFrameRender");
  e_JniOnPause           = (void *)so_try_find_addr_rx(&game_mod, MCF "JniOnPause");
  e_JniOnStop            = (void *)so_try_find_addr_rx(&game_mod, MCF "JniOnStop");
  e_JniDestruct          = (void *)so_try_find_addr_rx(&game_mod, MCF "JniDestruct");
  e_pushSensor           = (void *)so_find_addr_rx(&game_mod, MCF "pushSensor");
  e_setDownloadPath      = (void *)so_try_find_addr_rx(&game_mod, MCF "setDownloadPath");
}

static void *thiz_class;     // MainActivity jclass for the static native calls
static void *thiz_activity;  // the Activity object handed to JniConstruct

// ---------------------------------------------------------------------------
// input: Switch HID -> MCFLib_Sensor button bitmask (1 << AOM_BIT_*)
// ---------------------------------------------------------------------------

static PadState pad;

static AomInput s_input;

// Map the left stick onto the on-screen floating joystick:
static void update_stick_as_touch(void) {
  static int engaged = 0;
  if (!config.analog_stick) return;                      // toggled off (default)
  if (s_input.touch_count > 0) { engaged = 0; return; } // real finger wins
  HidAnalogStickState l = padGetStickPos(&pad, 0);
  float lx = l.x / 32767.0f, ly = l.y / 32767.0f;
  if (lx * lx + ly * ly < 0.20f * 0.20f) { engaged = 0; return; } // deadzone

  const int ox = screen_width / 4, oy = screen_height * 2 / 3; // joystick origin
  const int rad = screen_height / 4;
  if (!engaged) {                  // press at the origin (the drag start)
    s_input.touch[0].x = ox;
    s_input.touch[0].y = oy;
    engaged = 1;
  } else {                         // drag by the deflection (screen Y is down)
    s_input.touch[0].x = ox + (int)(lx * rad);
    s_input.touch[0].y = oy - (int)(ly * rad);
  }
  s_input.touch_count = 1;
}

static void update_keys(void) {
  padUpdate(&pad);
  const u64 d = padGetButtons(&pad);
  int m = 0;
  // Nintendo-native: A (right) confirms, B (bottom) cancels.
  // The engine opens the pause menu on the X bit (bit 6), so we put that on
  // Plus (+) and free the X/Y face buttons to act as alternate confirm/cancel.
  if (d & HidNpadButton_A) m |= 1 << AOM_BIT_A;
  if (d & HidNpadButton_B) m |= 1 << AOM_BIT_B;
  if (d & HidNpadButton_X) m |= 1 << AOM_BIT_A; // alt confirm
  if (d & HidNpadButton_Y) m |= 1 << AOM_BIT_Y; // submenu (game prompts Y)
  if (d & HidNpadButton_L) m |= 1 << AOM_BIT_L1;
  if (d & HidNpadButton_R) m |= 1 << AOM_BIT_R1;
  if (d & HidNpadButton_ZL) m |= 1 << AOM_BIT_L2;
  if (d & HidNpadButton_ZR) m |= 1 << AOM_BIT_R2;
  if (d & HidNpadButton_StickL) m |= 1 << AOM_BIT_L3;
  if (d & HidNpadButton_StickR) m |= 1 << AOM_BIT_R3;
  if (d & HidNpadButton_Plus)  m |= 1 << AOM_BIT_X;      // pause menu
  if (d & HidNpadButton_Minus) m |= 1 << AOM_BIT_SELECT;
  u64 dirs = d;
  if (!config.analog_stick)
    dirs |= (d & HidNpadButton_StickLUp    ? HidNpadButton_Up    : 0)
          | (d & HidNpadButton_StickLDown  ? HidNpadButton_Down  : 0)
          | (d & HidNpadButton_StickLLeft  ? HidNpadButton_Left  : 0)
          | (d & HidNpadButton_StickLRight ? HidNpadButton_Right : 0);
  if (dirs & HidNpadButton_Up)    m |= 1 << AOM_BIT_UP;
  if (dirs & HidNpadButton_Down)  m |= 1 << AOM_BIT_DOWN;
  if (dirs & HidNpadButton_Left)  m |= 1 << AOM_BIT_LEFT;
  if (dirs & HidNpadButton_Right) m |= 1 << AOM_BIT_RIGHT;

  s_input.key_now = m;
}

static void update_touch(void) {
  s_input.touch_max_x = screen_width;
  s_input.touch_max_y = screen_height;
  s_input.touch_count = 0;

  HidTouchScreenState st = {0};
  if (hidGetTouchScreenStates(&st, 1) && st.count > 0) {
    const float sx = (float)screen_width / 1280.0f;
    const float sy = (float)screen_height / 720.0f;
    int n = st.count < AOM_MAX_TOUCH ? st.count : AOM_MAX_TOUCH;
    for (int i = 0; i < n; i++) {
      s_input.touch[i].x = (int)(st.touches[i].x * sx);
      s_input.touch[i].y = (int)(st.touches[i].y * sy);
    }
    s_input.touch_count = n;
  }
}

int main(void) {
  cpu_boost(1);

  if (read_config(CONFIG_NAME) != 0)
    write_config(CONFIG_NAME);

  check_syscalls();
  check_data();
  set_screen_size(config.screen_width, config.screen_height);

  // shared system font for funcFontDrawStringToImage
  plInitialize(PlServiceType_User);
  gfx_init();

  // SDL backs the OpenSL ES shim (audio only)
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO) < 0)
    debugPrintf("SDL_Init(audio) failed: %s\n", SDL_GetError());

  if (!egl_init())
    fatal_error("Failed to create an OpenGL ES 2 context.");

  jni_init();

  // C++ runtime first, then the engine above it in the so heap (no overlap).
  if (so_load(&cxx_mod, CXX_SO_NAME, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", CXX_SO_NAME);
  const size_t cxx_used = ALIGN_MEM(cxx_mod.load_size, 0x1000);
  void *game_base = (char *)heap_so_base + cxx_used;
  const size_t game_limit = heap_so_limit - cxx_used;
  if (so_load(&game_mod, SO_NAME, game_base, game_limit) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  aom_resolve_imports(&cxx_mod, &game_mod);

  // resolve entry points while the symbol tables (in load_base) are live
  resolve_entry_points();
  if (!e_JniConstruct || !e_JniOnFrameRender || !e_JniOnFrameUpdate || !e_pushSensor)
    fatal_error("Could not resolve MCF engine entry points.");

  so_finalize(&cxx_mod);
  so_finalize(&game_mod);
  so_flush_caches(&cxx_mod);
  so_flush_caches(&game_mod);

  // the engine reads its stack-protector canary from tpidr_el0+0x28
  tls_setup_guard();

  // C++ static init first (the engine's constructors may use libc++)
  so_execute_init_array(&cxx_mod);
  so_execute_init_array(&game_mod);
  so_free_temp(&cxx_mod);
  so_free_temp(&game_mod);

  thiz_class = jni_make_thiz();
  thiz_activity = jni_make_thiz();

  if (e_JNI_OnLoad)
    e_JNI_OnLoad(fake_vm, NULL);

  // MainActivity.JniConstruct(activity, downloadPath, assetManager)
  void *download_path = jni_make_string(".");
  void *asset_mgr = jni_make_asset_manager();
  if (e_setDownloadPath)
    e_setDownloadPath(fake_env, thiz_class, download_path);
  e_JniConstruct(fake_env, thiz_class, thiz_activity, download_path, asset_mgr);

  e_JniOnStart(fake_env, thiz_class);
  e_JniOnResume(fake_env, thiz_class);

  e_JniOnGLSurfaceChanged(fake_env, thiz_class, screen_width, screen_height);

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  int boot_frames = 0;
  while (appletMainLoop() && !jni_quit_requested) {
    update_keys();
    update_touch();
    update_stick_as_touch(); // analog stick -> virtual-joystick touch (360°)

    void *sensor = jni_build_sensor_array(&s_input);
    e_pushSensor(fake_env, thiz_class, sensor);

    e_JniOnFrameUpdate(fake_env, thiz_class);
    e_JniOnFrameRender(fake_env, thiz_class);

    eglSwapBuffers(s_display, s_surface);

    if (boot_frames < 10 && ++boot_frames == 10)
      cpu_boost(0); // drop the load-time CPU boost once we're running
  }

  if (e_JniOnPause) e_JniOnPause(fake_env, thiz_class);
  if (e_JniOnStop)  e_JniOnStop(fake_env, thiz_class);
  if (e_JniDestruct) e_JniDestruct(fake_env, thiz_class);

  opensles_shutdown();
  egl_deinit();
  plExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
