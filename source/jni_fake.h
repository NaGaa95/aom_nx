/* jni_fake.h -- fake JNI environment for the MCF/Si engine (libmcfandroid.so)
 *
 * The engine drives platform services through static MainActivity.func*
 * callbacks; we provide a functional JNIEnv so they resolve to native C here.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

extern void *fake_vm;  // JavaVM *
extern void *fake_env; // JNIEnv *

extern volatile int jni_quit_requested; // set by funcApplicationExit

void jni_init(void);
void *jni_make_thiz(void);          // MainActivity instance/class token
void *jni_make_string(const char *utf);
void *jni_make_asset_manager(void); // AssetManager passed to JniConstruct

// MCFLib_Sensor button bit positions (KEY_BIT_*); key_now is 1<<bit.
enum {
  AOM_BIT_UP = 0, AOM_BIT_DOWN, AOM_BIT_LEFT, AOM_BIT_RIGHT,
  AOM_BIT_A, AOM_BIT_B, AOM_BIT_X, AOM_BIT_Y,
  AOM_BIT_START, AOM_BIT_SELECT,
  AOM_BIT_L1, AOM_BIT_L2, AOM_BIT_L3,
  AOM_BIT_R1, AOM_BIT_R2, AOM_BIT_R3,
  AOM_BIT_BACK, AOM_BIT_MODE, AOM_BIT_MENU,
};

#define AOM_MAX_TOUCH 4

// per-frame input snapshot, pushed to the engine via pushSensor()
typedef struct {
  int key_now;            // button bitmask (1<<AOM_BIT_*)
  int touch_max_x, touch_max_y;
  int touch_count;
  struct { int x, y; } touch[AOM_MAX_TOUCH];
} AomInput;

// build the MCFLib_Sensor int[] from `in`; returns the jintArray for pushSensor()
void *jni_build_sensor_array(const AomInput *in);

#endif
