/*
 * The shim that makes a Java contract a loadable Xmip module.
 *
 * The C entrypoint, contract table and lifecycle live here, as in the C
 * contract; the judgement is forwarded to xmip.contract.Contract through JNI.
 * The JVM is hosted in-process (owner, 2026-09-07): its library is loaded at
 * `start`, never linked, so the module builds without a JDK to link against
 * and runs against whichever JVM XMIP_JVM_LIBRARY names, or the one under
 * JAVA_HOME. The class path is XMIP_JAVA_CLASSPATH, or the current directory.
 *
 * One JVM per process is a JNI rule, so a second module instance attaches to
 * the JVM the first created rather than creating another.
 */

#include "xmip_module.h"

#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  define JVM_DEFAULT "bin\\server\\jvm.dll"
#  define PATH_SEP "\\"
#elif defined(__APPLE__)
#  include <dlfcn.h>
#  define JVM_DEFAULT "lib/server/libjvm.dylib"
#  define PATH_SEP "/"
#else
#  include <dlfcn.h>
#  define JVM_DEFAULT "lib/server/libjvm.so"
#  define PATH_SEP "/"
#endif

#define XMIP_JAVA_MESSAGE_MAX 512

typedef jint (JNICALL *CreateJavaVM)(JavaVM **, void **, void *);
typedef jint (JNICALL *GetCreatedJavaVMs)(JavaVM **, jsize, jsize *);

typedef struct {
    char   *descriptor;
    size_t  descriptor_len;
} Contract;

typedef struct {
    JavaVM        *jvm;
    XmipDiagnostic diagnostic;
    char           message[XMIP_JAVA_MESSAGE_MAX];
    char           error[XMIP_JAVA_MESSAGE_MAX];
    char           implied[XMIP_JAVA_MESSAGE_MAX];
} State;

static XmipStr str_of(const char *text, size_t len) {
    XmipStr s;
    s.ptr = (const uint8_t *)text;
    s.len = len;
    return s;
}

static void fail(State *s, const char *what) {
    strncpy(s->error, what, XMIP_JAVA_MESSAGE_MAX - 1);
    s->error[XMIP_JAVA_MESSAGE_MAX - 1] = '\0';
}

static void *load_symbol(const char *library, const char *name) {
#if defined(_WIN32)
    HMODULE handle = LoadLibraryA(library);
    if (handle == NULL) return NULL;
    return (void *)GetProcAddress(handle, name);
#else
    void *handle = dlopen(library, RTLD_NOW | RTLD_GLOBAL);
    if (handle == NULL) return NULL;
    return dlsym(handle, name);
#endif
}

/* Where the JVM library is: named outright, or under JAVA_HOME. */
static int jvm_library(char *out, size_t cap) {
    const char *named = getenv("XMIP_JVM_LIBRARY");
    const char *home = getenv("JAVA_HOME");
    if (named && *named) { snprintf(out, cap, "%s", named); return 1; }
    if (home && *home) { snprintf(out, cap, "%s" PATH_SEP JVM_DEFAULT, home); return 1; }
    return 0;
}

static XmipStatus configure(void *state, XmipStr toml) { (void)state; (void)toml; return XMIP_OK; }

static XmipStatus start(void *state) {
    State *s = (State *)state;
    char library[1024];
    char classpath[2048];
    const char *cp = getenv("XMIP_JAVA_CLASSPATH");
    JavaVMOption options[1];
    JavaVMInitArgs args;
    JNIEnv *env = NULL;
    CreateJavaVM create;
    GetCreatedJavaVMs created;
    jsize count = 0;
    if (s->jvm != NULL) return XMIP_OK;
    if (!jvm_library(library, sizeof library)) {
        fail(s, "no JVM: set XMIP_JVM_LIBRARY or JAVA_HOME");
        return XMIP_E_UNAVAILABLE;
    }
    created = (GetCreatedJavaVMs)load_symbol(library, "JNI_GetCreatedJavaVMs");
    create = (CreateJavaVM)load_symbol(library, "JNI_CreateJavaVM");
    if (created == NULL || create == NULL) {
        fail(s, "the JVM library could not be loaded");
        return XMIP_E_UNAVAILABLE;
    }
    if (created(&s->jvm, 1, &count) == JNI_OK && count > 0 && s->jvm != NULL) return XMIP_OK;
    snprintf(classpath, sizeof classpath, "-Djava.class.path=%s", (cp && *cp) ? cp : ".");
    options[0].optionString = classpath;
    options[0].extraInfo = NULL;
    args.version = JNI_VERSION_10;
    args.nOptions = 1;
    args.options = options;
    args.ignoreUnrecognized = JNI_TRUE;
    if (create(&s->jvm, (void **)&env, &args) != JNI_OK) {
        s->jvm = NULL;
        fail(s, "the JVM refused to start");
        return XMIP_E_UNAVAILABLE;
    }
    return XMIP_OK;
}

static XmipStatus stop(void *state) { (void)state; return XMIP_OK; }

static XmipStatus load(void *state, XmipStr descriptor, void **out_contract) {
    Contract *contract;
    (void)state;
    if (out_contract == NULL) return XMIP_E_INVALID;
    contract = (Contract *)calloc(1, sizeof *contract);
    if (contract == NULL) return XMIP_E_CAPACITY;
    if (descriptor.len > 0) {
        contract->descriptor = (char *)malloc(descriptor.len + 1);
        if (contract->descriptor == NULL) { free(contract); return XMIP_E_CAPACITY; }
        memcpy(contract->descriptor, descriptor.ptr, descriptor.len);
        contract->descriptor[descriptor.len] = '\0';
        contract->descriptor_len = descriptor.len;
    }
    *out_contract = contract;
    return XMIP_OK;
}

static void release(void *state, void *contract) {
    Contract *c = (Contract *)contract;
    (void)state;
    if (c == NULL) return;
    free(c->descriptor);
    free(c);
}

/* An attached JNIEnv for this thread, or NULL with the error recorded. */
static JNIEnv *attached(State *s) {
    JNIEnv *env = NULL;
    if (s->jvm == NULL) { fail(s, "validate before start"); return NULL; }
    if ((*s->jvm)->GetEnv(s->jvm, (void **)&env, JNI_VERSION_10) == JNI_OK) return env;
    if ((*s->jvm)->AttachCurrentThread(s->jvm, (void **)&env, NULL) != JNI_OK) {
        fail(s, "this thread could not attach to the JVM");
        return NULL;
    }
    return env;
}

/* Call a static String method on the contract class; copies the answer into
 * `into`, returns 1 for a non-null answer, 0 for null, -1 on a JVM error. */
static int call_string(State *s, JNIEnv *env, const char *method, const char *signature,
                       jobject first, jobject second, char *into, size_t cap) {
    jclass cls = (*env)->FindClass(env, "xmip/contract/Contract");
    jmethodID id;
    jstring answer;
    const char *text;
    if (cls == NULL) { (*env)->ExceptionClear(env); fail(s, "xmip.contract.Contract is not on the class path"); return -1; }
    id = (*env)->GetStaticMethodID(env, cls, method, signature);
    if (id == NULL) { (*env)->ExceptionClear(env); fail(s, "the contract class lacks the method"); return -1; }
    answer = (jstring)(*env)->CallStaticObjectMethod(env, cls, id, first, second);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); fail(s, "the contract threw"); return -1; }
    if (answer == NULL) return 0;
    text = (*env)->GetStringUTFChars(env, answer, NULL);
    snprintf(into, cap, "%s", text ? text : "");
    (*env)->ReleaseStringUTFChars(env, answer, text);
    return 1;
}

static XmipStatus validate(void *state, void *contract, const XmipReader *in,
                           const XmipDiagnostic **out, size_t *out_len) {
    State *s = (State *)state;
    Contract *c = (Contract *)contract;
    JNIEnv *env;
    uint8_t *bytes = NULL;
    size_t len = 0, cap = 0;
    jbyteArray array;
    jstring descriptor;
    int answered;
    if (s == NULL || c == NULL || in == NULL || in->read == NULL || out == NULL || out_len == NULL) {
        return XMIP_E_INVALID;
    }
    *out = NULL;
    *out_len = 0;
    env = attached(s);
    if (env == NULL) return XMIP_E_STATE;
    for (;;) {
        int64_t got;
        if (cap - len < 4096) {
            size_t grown = cap == 0 ? 8192 : cap * 2;
            uint8_t *bigger = (uint8_t *)realloc(bytes, grown);
            if (bigger == NULL) { free(bytes); return XMIP_E_CAPACITY; }
            bytes = bigger;
            cap = grown;
        }
        got = in->read(in->ctx, bytes + len, cap - len);
        if (got < 0) { free(bytes); return (XmipStatus)got; }
        if (got == 0) break;
        len += (size_t)got;
    }
    array = (*env)->NewByteArray(env, (jsize)len);
    if (array == NULL) { free(bytes); fail(s, "the JVM is out of memory"); return XMIP_E_CAPACITY; }
    (*env)->SetByteArrayRegion(env, array, 0, (jsize)len, (const jbyte *)bytes);
    free(bytes);
    descriptor = (*env)->NewStringUTF(env, c->descriptor ? c->descriptor : "");
    answered = call_string(s, env, "validate", "(Ljava/lang/String;[B)Ljava/lang/String;",
                           descriptor, array, s->message, sizeof s->message);
    (*env)->DeleteLocalRef(env, array);
    (*env)->DeleteLocalRef(env, descriptor);
    if (answered < 0) return XMIP_E_INTERNAL;
    if (answered == 0 || s->message[0] == '\0') return XMIP_OK;
    s->diagnostic.code = XMIP_E_CONTRACT;
    s->diagnostic.message = str_of(s->message, strlen(s->message));
    s->diagnostic.location = str_of("", 0);
    s->diagnostic.offset = UINT64_MAX;
    *out = &s->diagnostic;
    *out_len = 1;
    return XMIP_E_CONTRACT;
}

static XmipStatus implies(void *state, void *contract, XmipStr key, XmipStr *out) {
    State *s = (State *)state;
    Contract *c = (Contract *)contract;
    JNIEnv *env;
    char key_text[256];
    jstring descriptor, jkey;
    int answered;
    if (s == NULL || c == NULL || out == NULL) return XMIP_E_INVALID;
    env = attached(s);
    if (env == NULL) return XMIP_E_STATE;
    snprintf(key_text, sizeof key_text, "%.*s", (int)(key.len < 255 ? key.len : 255), (const char *)key.ptr);
    descriptor = (*env)->NewStringUTF(env, c->descriptor ? c->descriptor : "");
    jkey = (*env)->NewStringUTF(env, key_text);
    answered = call_string(s, env, "implies", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                           descriptor, jkey, s->implied, sizeof s->implied);
    (*env)->DeleteLocalRef(env, descriptor);
    (*env)->DeleteLocalRef(env, jkey);
    if (answered < 0) return XMIP_E_INTERNAL;
    if (answered == 0) return XMIP_E_NOT_FOUND;
    *out = str_of(s->implied, strlen(s->implied));
    return XMIP_OK;
}

static XmipStr last_error(void *state) {
    State *s = (State *)state;
    return str_of(s->error, strlen(s->error));
}

/* The JVM outlives the module: JNI forbids restarting one in a process. */
static void destroy(void *state) { free(state); }

static const XmipContractVtable VTABLE = {
    { 1u, 0u, configure, start, stop },
    load, release, validate, implies
};

XMIP_EXPORT XmipStatus xmip_create_module_v1(const XmipHost *host, XmipModule *out) {
    State *state;
    if (host == NULL || out == NULL) return XMIP_E_INVALID;
    if (host->abi_version != XMIP_ABI_VERSION) return XMIP_E_UNSUPPORTED;
    state = (State *)calloc(1, sizeof *state);
    if (state == NULL) return XMIP_E_CAPACITY;
    out->descriptor.abi_version = XMIP_ABI_VERSION;
    out->descriptor.provider = str_of("core", 4);
    out->descriptor.module = str_of("contract", 8);
    out->descriptor.standard = str_of("java", 4);
    out->descriptor.trait_major = 1u;
    out->descriptor.trait_minor = 0u;
    out->descriptor.module_major = 0u;
    out->descriptor.module_minor = 1u;
    out->descriptor.module_patch = 0u;
    out->state = state;
    out->vtable = &VTABLE;
    out->last_error = last_error;
    out->destroy = destroy;
    return XMIP_OK;
}
