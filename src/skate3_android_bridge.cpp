#include "skate3_android_bridge.h"

#if defined(__ANDROID__)

#include <jni.h>

#include <string>

#include <SDL3/SDL_system.h>
#include <rex/logging.h>

namespace skate3::android {
namespace {

struct ActivityMethod {
  JNIEnv* env = nullptr;
  jobject activity = nullptr;
  jclass cls = nullptr;
  jmethodID method = nullptr;

  ~ActivityMethod() {
    if (!env) {
      return;
    }
    if (cls) {
      env->DeleteLocalRef(cls);
    }
    if (activity) {
      env->DeleteLocalRef(activity);
    }
  }

  // Logs and clears a pending Java exception; true if there was one.
  bool Failed(const char* what) {
    if (!env->ExceptionCheck()) {
      return false;
    }
    env->ExceptionDescribe();
    env->ExceptionClear();
    REXLOG_WARN("android bridge: {} threw", what);
    return true;
  }
};

// Resolves `name` as a static method on the activity's own class. SDL vends a
// JNIEnv for the calling thread and a local reference to the activity.
bool Resolve(ActivityMethod& m, const char* name, const char* signature) {
  m.env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  m.activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!m.env || !m.activity) {
    REXLOG_WARN("android bridge: no JNI environment or activity for {}", name);
    return false;
  }
  m.cls = m.env->GetObjectClass(m.activity);
  m.method = m.env->GetStaticMethodID(m.cls, name, signature);
  if (m.env->ExceptionCheck()) {
    m.env->ExceptionClear();
    m.method = nullptr;
  }
  if (!m.method) {
    REXLOG_WARN("android bridge: the activity has no static {}{}", name, signature);
    return false;
  }
  return true;
}

}  // namespace

std::filesystem::path PickDocument(std::string_view title) {
  ActivityMethod m;
  if (!Resolve(m, "pickDocument", "(Ljava/lang/String;)I")) {
    return {};
  }
  jstring java_title = m.env->NewStringUTF(std::string(title).c_str());
  const jint fd = m.env->CallStaticIntMethod(m.cls, m.method, java_title);
  m.env->DeleteLocalRef(java_title);
  if (m.Failed("pickDocument")) {
    return {};
  }
  if (fd < 0) {
    REXLOG_INFO("android bridge: document picker returned nothing ({})", title);
    return {};
  }
  REXLOG_INFO("android bridge: document picker returned fd {} ({})", int(fd), title);
  return std::filesystem::path("/proc/self/fd") / std::to_string(fd);
}

bool RequestRestart() {
  ActivityMethod m;
  if (!Resolve(m, "requestRestart", "()Z")) {
    return false;
  }
  const jboolean accepted = m.env->CallStaticBooleanMethod(m.cls, m.method);
  if (m.Failed("requestRestart")) {
    return false;
  }
  return accepted == JNI_TRUE;
}

}  // namespace skate3::android

#endif  // defined(__ANDROID__)
