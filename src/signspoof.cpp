// TapSDK signature bypass.
//
// The dex is patched (tools/patch_sign.py) so that
//   com/taptap/sdk/kit/internal/utils/TapSignKit.getSign(Context)
// is declared native; this library implements it.
//
// Why the dex had to change: ART's RegisterNatives only accepts methods that are
// already ACC_NATIVE -- a plain Java method is rejected (rc=-1), and inline-
// hooking its compiled body would require a quick-ABI -> JNI-ABI trampoline that
// breaks on any ART layout change.  The patch touches exactly one method body
// (no new strings/types/methods) and keeps name + prototype intact.
//
// Chain (classes3.dex):
//   GatekeeperRepository$initialize$1$networkData$1.invokeSuspend()
//     -> TapSignKit.getSign(Context)                     (reports the digest)
//     -> new GateKeeperRequest("android", packageName, digest)
//     -> GatekeeperApiService.fetchGatekeeper(...)       <-- server verdict
#include "cheats.h"
#include "jni_env.h"
#include "log.h"

#include <jni.h>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace {

// 92:1E:E8:3D:E6:04:25:6B:2E:27:E6:24:D6:CF:D6:35
// CommonUtils.getHashString() renders bytes via Integer.toHexString(), so the
// string getSign() produced was lowercase, 32 chars, no separators.
// The server compares GateKeeperRequest.packageSign against the fingerprint
// registered in the TapTap console, which is the certificate's SHA-256
// (EC:E5:01:39:...:0B:D7).  The MD5 form did not match, so ship the SHA-256.
const char *kOriginalSign = "ECE50139FA8F41D26D096F1426EAC377623EDCEB2A426D5F65ED84D492C10BD7";

std::atomic<int> g_logs{0};

// --------------------------------------------------------------------- MD5
struct Md5 {
    unsigned int a, b, c, d;
    unsigned long long len;
    unsigned char buf[64];
    size_t used;

    Md5() : a(0x67452301), b(0xefcdab89), c(0x98badcfe), d(0x10325476), len(0), used(0) {}
    static unsigned int rol(unsigned int x, int s) { return (x << s) | (x >> (32 - s)); }

    void block(const unsigned char *p) {
        static const unsigned int K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
            0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
            0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
            0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
            0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
            0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
            0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
            0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
            0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        unsigned int m[16];
        for (int i = 0; i < 16; ++i)
            m[i] = (unsigned int) p[i * 4] | ((unsigned int) p[i * 4 + 1] << 8) |
                   ((unsigned int) p[i * 4 + 2] << 16) | ((unsigned int) p[i * 4 + 3] << 24);
        unsigned int A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; ++i) {
            unsigned int f;
            int g;
            if (i < 16)      { f = (B & C) | (~B & D); g = i; }
            else if (i < 32) { f = (D & B) | (~D & C); g = (5 * i + 1) & 15; }
            else if (i < 48) { f = B ^ C ^ D;          g = (3 * i + 5) & 15; }
            else             { f = C ^ (B | ~D);       g = (7 * i) & 15; }
            unsigned int tmp = D;
            D = C; C = B;
            B = B + rol(A + f + K[i] + m[g], S[i]);
            A = tmp;
        }
        a += A; b += B; c += C; d += D;
    }

    void update(const void *data, size_t n) {
        const unsigned char *p = (const unsigned char *) data;
        len += (unsigned long long) n * 8;
        while (n--) {
            buf[used++] = *p++;
            if (used == 64) { block(buf); used = 0; }
        }
    }

    void final_hex(char out[33]) {
        unsigned long long bits = len;
        unsigned char pad = 0x80, zero = 0;
        update(&pad, 1);
        while (used != 56) update(&zero, 1);
        unsigned char lenb[8];
        for (int i = 0; i < 8; ++i) lenb[i] = (unsigned char) ((bits >> (8 * i)) & 0xff);
        update(lenb, 8);
        unsigned int v[4] = {a, b, c, d};
        static const char *hex = "0123456789abcdef";
        for (int i = 0; i < 16; ++i) {
            unsigned char byte = (unsigned char) ((v[i / 4] >> (8 * (i % 4))) & 0xff);
            out[i * 2] = hex[byte >> 4];
            out[i * 2 + 1] = hex[byte & 0xf];
        }
        out[32] = 0;
    }
};

// Reads the live signing certificate and digests it the same way TapSDK did.
bool computeRealMd5(JNIEnv *env, char out[33]) {
    jclass clsAT = env->FindClass("android/app/ActivityThread");
    jclass clsCtx = env->FindClass("android/content/Context");
    jclass clsPM = env->FindClass("android/content/pm/PackageManager");
    jclass clsPI = env->FindClass("android/content/pm/PackageInfo");
    jclass clsSig = env->FindClass("android/content/pm/Signature");
    if (!clsAT || !clsCtx || !clsPM || !clsPI || !clsSig) { env->ExceptionClear(); return false; }

    bool ok = false;
    jmethodID mCur = env->GetStaticMethodID(clsAT, "currentApplication", "()Landroid/app/Application;");
    jmethodID mGetPM = env->GetMethodID(clsCtx, "getPackageManager",
                                        "()Landroid/content/pm/PackageManager;");
    jmethodID mPkg = env->GetMethodID(clsCtx, "getPackageName", "()Ljava/lang/String;");
    jmethodID mInfo = env->GetMethodID(clsPM, "getPackageInfo",
                                       "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
    jfieldID fSigs = env->GetFieldID(clsPI, "signatures", "[Landroid/content/pm/Signature;");
    jmethodID mBytes = env->GetMethodID(clsSig, "toByteArray", "()[B");

    if (mCur && mGetPM && mPkg && mInfo && fSigs && mBytes) {
        jobject app = env->CallStaticObjectMethod(clsAT, mCur);
        if (app) {
            jobject pm = env->CallObjectMethod(app, mGetPM);
            jstring pkg = (jstring) env->CallObjectMethod(app, mPkg);
            if (pm && pkg) {
                jobject info = env->CallObjectMethod(pm, mInfo, pkg, (jint) 64);
                if (info) {
                    jobjectArray sigs = (jobjectArray) env->GetObjectField(info, fSigs);
                    if (sigs && env->GetArrayLength(sigs) > 0) {
                        jobject sig = env->GetObjectArrayElement(sigs, 0);
                        if (sig) {
                            jbyteArray b = (jbyteArray) env->CallObjectMethod(sig, mBytes);
                            if (b) {
                                jsize n = env->GetArrayLength(b);
                                jbyte *p = env->GetByteArrayElements(b, nullptr);
                                if (p) {
                                    Md5 md5;
                                    md5.update(p, (size_t) n);
                                    md5.final_hex(out);
                                    ok = true;
                                    env->ReleaseByteArrayElements(b, p, JNI_ABORT);
                                }
                                env->DeleteLocalRef(b);
                            }
                            env->DeleteLocalRef(sig);
                        }
                        env->DeleteLocalRef(sigs);
                    }
                    env->DeleteLocalRef(info);
                }
                env->DeleteLocalRef(pkg);
                env->DeleteLocalRef(pm);
            }
            env->DeleteLocalRef(app);
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(clsAT); env->DeleteLocalRef(clsCtx); env->DeleteLocalRef(clsPM);
    env->DeleteLocalRef(clsPI); env->DeleteLocalRef(clsSig);
    return ok;
}

// Implementation of the now-native TapSignKit.getSign(Context).
jstring JNICALL mw_getSign(JNIEnv *env, jobject thiz, jobject context) {
    (void) thiz; (void) context;
    if (g_cheat.bypassSign.load()) {
        if (g_logs.fetch_add(1) < 8) LOGI("TapSignKit.getSign -> spoofed %s", kOriginalSign);
        return env->NewStringUTF(kOriginalSign);
    }
    char real[33] = {0};
    if (computeRealMd5(env, real)) {
        if (g_logs.fetch_add(1) < 16) LOGI("TapSignKit.getSign -> real %s", real);
        return env->NewStringUTF(real);
    }
    LOGW("TapSignKit.getSign -> real digest failed, using original");
    return env->NewStringUTF(kOriginalSign);
}

const char *kCls   = "com/taptap/sdk/kit/internal/utils/TapSignKit";
const char *kName  = "getSign";
const char *kProto = "(Landroid/content/Context;)Ljava/lang/String;";

} // namespace

extern "C" bool SignSpoof_Install() {
    static std::atomic<int> state{0};   // 0 = untried, 1 = live, 2 = gave up
    if (state.load() == 1) return true;
    if (state.load() == 2) return false;

    JNIEnv *env = MW_GetEnv();
    if (!env) return false;

    // must go through the app ClassLoader
    jclass cls = MW_FindAppClass(env, kCls);
    if (!cls) {
        env->ExceptionClear();          // TapSDK not loaded yet -> retry later
        return false;
    }

    static const JNINativeMethod kMethods[] = {
        {(char *) kName, (char *) kProto, (void *) mw_getSign},
    };

    jint rc = env->RegisterNatives(cls, kMethods, 1);
    bool threw = env->ExceptionCheck() != JNI_FALSE;
    if (threw) env->ExceptionDescribe();
    if (threw) env->ExceptionClear();

    if (rc != JNI_OK) {
        LOGW("sign spoof: RegisterNatives refused (rc=%d) -- dex patch missing?", rc);
        env->DeleteLocalRef(cls);
        state.store(2);
        return false;
    }
    LOGI("sign spoof: TapSignKit.getSign implemented natively, bypass default %s",
         g_cheat.bypassSign.load() ? "ON" : "OFF");

    // Self-check so a broken redirect shows up now, not at login time.
    // getSign is an instance method: build a TapSignKit (its ctor takes nothing).
    jmethodID mCtor = env->GetMethodID(cls, "<init>", "()V");
    jmethodID m = env->GetMethodID(cls, kName, kProto);
    jobject inst = mCtor ? env->NewObject(cls, mCtor) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); }
    if (inst && m) {
        jstring got = (jstring) env->CallObjectMethod(inst, m, (jobject) nullptr);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            LOGW("sign spoof: self-check threw");
        } else if (got) {
            const char *s = env->GetStringUTFChars(got, nullptr);
            LOGI("sign spoof: self-check -> %s", s ? s : "(null)");
            if (s) env->ReleaseStringUTFChars(got, s);
            env->DeleteLocalRef(got);
        }
    } else {
        env->ExceptionClear();
    }
    if (inst) env->DeleteLocalRef(inst);
    env->DeleteLocalRef(cls);

    state.store(1);
    return true;
}
