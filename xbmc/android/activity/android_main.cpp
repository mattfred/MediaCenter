/*
 *      Copyright (C) 2012-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "system.h"

#include <stdlib.h>
#include <errno.h>
#include <android_native_app_glue.h>
#include "EventLoop.h"
#include "XBMCApp.h"
#include "androidjni/SurfaceTexture.h"
#include "utils/StringUtils.h"
#include "CompileInfo.h"

#if defined(HAVE_BREAKPAD)
#define __STDC_FORMAT_MACROS
#include "client/linux/handler/minidump_descriptor.h"
#include "client/linux/handler/exception_handler.h"
#endif

#include "android/activity/JNIMainActivity.h"
#include "android/activity/JNIXBMCVideoView.h"
#include "android/activity/JNIXBMCAudioManagerOnAudioFocusChangeListener.h"
#include "android/activity/JNIXBMCSurfaceTextureOnFrameAvailableListener.h"
#include "android/activity/JNIXBMCNsdManagerDiscoveryListener.h"
#include "android/activity/JNIXBMCNsdManagerRegistrationListener.h"
#include "android/activity/JNIXBMCNsdManagerResolveListener.h"
#include "android/activity/JNIXBMCMediaSession.h"

#if defined(HAVE_BREAKPAD)
static void *startCrashHandler(void* arg)
{
  CJNIMainActivity::startCrashHandler();
  return NULL;
}

static bool dumpCallback(const google_breakpad::MinidumpDescriptor& descriptor,
                          void* context, bool succeeded)
{
  // Issue with breakpad to use JNI from callback. Have to use a thread
  // https://code.google.com/p/android/issues/detail?id=162663
  pthread_t t;
  pthread_create(&t, NULL, startCrashHandler, NULL);
  pthread_join(t, NULL);
  return succeeded;
}
#endif


// redirect stdout / stderr to logcat
// https://codelab.wordpress.com/2014/11/03/how-to-use-standard-output-streams-for-logging-in-android-apps/
static int pfd[2];
static pthread_t thr;
static const char *tag = "myapp";

static void *thread_logger(void*)
{
  ssize_t rdsz;
  char buf[128];
  while((rdsz = read(pfd[0], buf, sizeof buf - 1)) > 0)
  {
    if(buf[rdsz - 1] == '\n')
      --rdsz;
    buf[rdsz] = 0;  /* add null-terminator */
    __android_log_write(ANDROID_LOG_DEBUG, tag, buf);
  }
  return 0;
}

int start_logger(const char *app_name)
{
  tag = app_name;

  /* make stdout line-buffered and stderr unbuffered */
  setvbuf(stdout, 0, _IOLBF, 0);
  setvbuf(stderr, 0, _IONBF, 0);

  /* create the pipe and redirect stdout and stderr */
  pipe(pfd);
  dup2(pfd[1], 1);
  dup2(pfd[1], 2);

  /* spawn the logging thread */
  if(pthread_create(&thr, 0, thread_logger, 0) == -1)
    return -1;
  pthread_detach(thr);
  return 0;
}


// copied from new android_native_app_glue.c
static void process_input(struct android_app* app, struct android_poll_source* source) {
    AInputEvent* event = NULL;
    int processed = 0;
    while (AInputQueue_getEvent(app->inputQueue, &event) >= 0) {
        if (AInputQueue_preDispatchEvent(app->inputQueue, event)) {
            continue;
        }
        int32_t handled = 0;
        if (app->onInputEvent != NULL) handled = app->onInputEvent(app, event);
        AInputQueue_finishEvent(app->inputQueue, event, handled);
        processed = 1;
    }
    if (processed == 0) {
        CXBMCApp::android_printf("process_input: Failure reading next input event: %s", strerror(errno));
    }
}

extern void android_main(struct android_app* state)
{
  {
    // make sure that the linker doesn't strip out our glue
    app_dummy();

    // revector inputPollSource.process so we can shut up
    // its useless verbose logging on new events (see ouya)
    // and fix the error in handling multiple input events.
    // see https://code.google.com/p/android/issues/detail?id=41755
    state->inputPollSource.process = process_input;

    CEventLoop eventLoop(state);
    CXBMCApp xbmcApp(state->activity);
    if (xbmcApp.isValid())
    {
#if defined(HAVE_BREAKPAD)
      google_breakpad::MinidumpDescriptor descriptor(google_breakpad::MinidumpDescriptor::kMicrodumpOnConsole);
      google_breakpad::ExceptionHandler eh(descriptor,
                                        NULL,
                                        dumpCallback,
                                        NULL,
                                        true,
                                        -1);
#endif
      start_logger("SPMC_STD");

      IInputHandler inputHandler;
      eventLoop.run(xbmcApp, inputHandler);
    }
    else
      CXBMCApp::android_printf("android_main: setup failed");

    CXBMCApp::android_printf("android_main: Exiting");
    // We need to call exit() so that all loaded libraries are properly unloaded
    // otherwise on the next start of the Activity android will simple re-use
    // those loaded libs in the state they were in when we quit XBMC last time
    // which will lead to crashes because of global/static classes that haven't
    // been properly uninitialized
  }
  exit(0);
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
  jint version = JNI_VERSION_1_6;
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), version) != JNI_OK)
    return -1;

  std::string pkgRoot = CCompileInfo::GetClass();
  
  std::string mainClass = pkgRoot + "/Main";
  std::string bcReceiver = pkgRoot + "/XBMCBroadcastReceiver";
  std::string settingsObserver = pkgRoot + "/XBMCSettingsContentObserver";

  CJNIXBMCAudioManagerOnAudioFocusChangeListener::RegisterNatives(env);
  CJNIXBMCSurfaceTextureOnFrameAvailableListener::RegisterNatives(env);
  CJNIXBMCVideoView::RegisterNatives(env);
  jni::CJNIXBMCNsdManagerDiscoveryListener::RegisterNatives(env);
  jni::CJNIXBMCNsdManagerRegistrationListener::RegisterNatives(env);
  jni::CJNIXBMCNsdManagerResolveListener::RegisterNatives(env);
  jni::CJNIXBMCMediaSession::RegisterNatives(env);
  
  jclass cMain = env->FindClass(mainClass.c_str());
  if(cMain)
  {
    JNINativeMethod methods[] = 
    {
      {"_onNewIntent", "(Landroid/content/Intent;)V", (void*)&CJNIMainActivity::_onNewIntent},
      {"_onActivityResult", "(IILandroid/content/Intent;)V", (void*)&CJNIMainActivity::_onActivityResult},
      {"_doFrame", "(J)V", (void*)&CJNIMainActivity::_doFrame},
      {"_callNative", "(JJ)V", (void*)&CJNIMainActivity::_callNative},
      {"_onAudioDeviceAdded", "([Landroid/media/AudioDeviceInfo;)V", (void*)&CJNIMainActivity::_onAudioDeviceAdded},
      {"_onAudioDeviceRemoved", "([Landroid/media/AudioDeviceInfo;)V", (void*)&CJNIMainActivity::_onAudioDeviceRemoved},
      {"_onCaptureAvailable", "(Landroid/media/Image;)V", (void*)&CJNIMainActivity::_onCaptureAvailable},
      {"_onScreenshotAvailable", "(Landroid/media/Image;)V", (void*)&CJNIMainActivity::_onScreenshotAvailable},
      {"_onVisibleBehindCanceled", "()V", (void*)&CJNIMainActivity::_onVisibleBehindCanceled},
    };
    env->RegisterNatives(cMain, methods, sizeof(methods)/sizeof(methods[0]));
  }

  jclass cBroadcastReceiver = env->FindClass(bcReceiver.c_str());
  if(cBroadcastReceiver)
  {
    JNINativeMethod methods[] = 
    {
      {"_onReceive", "(Landroid/content/Intent;)V", (void*)&CJNIBroadcastReceiver::_onReceive},
    };
    env->RegisterNatives(cBroadcastReceiver, methods, sizeof(methods)/sizeof(methods[0]));
  }

  jclass cSettingsObserver = env->FindClass(settingsObserver.c_str());
  if(cSettingsObserver)
  {
    JNINativeMethod methods[] = 
    {
      {"_onVolumeChanged", "(I)V", (void*)&CJNIMainActivity::_onVolumeChanged},
    };
    env->RegisterNatives(cSettingsObserver, methods, sizeof(methods)/sizeof(methods[0]));
  }

  return version;
}
