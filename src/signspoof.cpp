// TapSDK signature bypass -- no dex modification, native only.
//
// Verified chain (classes3.dex):
//   GatekeeperRepository$initialize$1$networkData$1.invokeSuspend()
//     -> TapSignKit.INSTANCE.getSign(Context)                    (line 204)
//          -> PackageInfo.signatures[0].toByteArray()
//          -> CommonUtils.getMD5(byte[])                         (TapSignKit:86)
//     -> new GateKeeperRequest("android", packageName, md5)
//     -> GatekeeperApiService.fetchGatekeeper(...)   <-- server decides
//
// TapSignKit.getSign has four callers (Gatekeeper init, persistent queue,
// game-duration tracker, update breakpoint store) and all of them only consume
// the digest string.  CommonUtils.getMD5([B)Ljava/lang/String; itself is public
// static with exactly ONE caller in the whole application, so replacing that
// implementation is surgical: the login request carries the original certificate
// digest and nothing else in the app changes.
//
// Mechanism: JNI RegisterNatives on the existing method.
//   ART's RegisterNative() -> ArtMethod::RegisterNative(fnPtr, generic_jni_stub)
//     - flips the method to native (kAccNative)
//     - stores fnPtr in data_ and the *runtime provided* generic JNI trampoline
//       in entry_point_from_quick_compiled_code_
//   ART therefore performs the quick->JNI ABI transition itself, so we never
//   hard-code ArtMethod offsets nor fake a calling convention.  If the platform
//   refuses (some ART builds enforce "declared native"), RegisterNatives returns
//   non-zero and we simply do not install -- the game keeps running untouched.
#include "cheats.h"
#include "jni_env.h"
#include "log.h"

#include <jni.h>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace {

// 92:1E:E8:3D:E6:04:25:6B:2E:27:E6:24:D6:CF:D6:35  (original certificate MD5)
// CommonUtils.getHashString() renders every byte through Integer.toHexString(),
// so getSign() yields lowercase, 32 chars, no separators.
const char *kOriginalMd5 = "921ee83de604256b2e27e624d6cfd635";

std::atomic<int> g_logs{0};
std::atomic<int> g_calls{0};

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

// Replacement implementation for CommonUtils.getMD5([B)String.
// Standard JNI ABI: ART hands us JNIEnv* / jclass / the real argument.
jstring JNICALL mw_getMD5(JNIEnv *env, jclass, jbyteArray bytes) {
    g_calls.fetch_add(1);
    if (g_cheat.bypassSign.load()) {
        if (g_logs.fetch_add(1) < 6) LOGI("getMD5 -> spoofed %s", kOriginalMd5);
        return env->NewStringUTF(kOriginalMd5);
    }
    // toggle off: behave exactly like the original implementation
    if (!bytes) return env->NewStringUTF("");
    jsize n = env->GetArrayLength(bytes);
    jbyte *p = env->GetByteArrayElements(bytes, nullptr);
    if (!p) { env->ExceptionClear(); return env->NewStringUTF(""); }
    char hex[33] = {0};
    Md5 md5;
    md5.update(p, (size_t) n);
    md5.final_hex(hex);
    env->ReleaseByteArrayElements(bytes, p, JNI_ABORT);
    if (g_logs.fetch_add(1) < 12) LOGI("getMD5 -> real %s", hex);
    return env->NewStringUTF(hex);
}

const char *kCls   = "com/taptap/sdk/kit/internal/utils/CommonUtils";
const char *kName  = "getMD5";
const char *kProto = "([B)Ljava/lang/String;";

} // namespace

// ---------------------------------------------------------------------------
// Installs the redirect.  Safe to call repeatedly; returns true once live.
// ---------------------------------------------------------------------------
extern "C" bool SignSpoof_Install() {
    static std::atomic<int> state{0};   // 0 = not tried, 1 = live, 2 = gave up
    if (state.load() == 1) return true;
    if (state.load() == 2) return false;

    JNIEnv *env = MW_GetEnv();
    if (!env) { LOGW("sign spoof: no JNIEnv"); return false; }

    jclass cls = env->FindClass(kCls);
    if (!cls) {
        env->ExceptionClear();          // TapSDK not loaded yet -> retry later
        LOGW("sign spoof: %s not loaded yet", kCls);
        return false;
    }

    static const JNINativeMethod kMethods[] = {
        {(char *) kName, (char *) kProto, (void *) mw_getMD5},
    };

    jint rc = env->RegisterNatives(cls, kMethods, 1);
    bool threw = env->ExceptionCheck() != JNI_FALSE;
    if (threw) env->ExceptionDescribe();
    if (threw) env->ExceptionClear();
    env->DeleteLocalRef(cls);

    if (rc != JNI_OK) {
        LOGW("sign spoof: RegisterNatives refused (rc=%d) -- bypass inactive", rc);
        state.store(2);
        return false;
    }

    state.store(1);
    LOGI("sign spoof: CommonUtils.getMD5 replaced, bypass default %s",
         g_cheat.bypassSign.load() ? "ON" : "OFF");

    // Self-check: call it once so a broken redirect shows up in the log
    // instead of silently failing at login time.
    jclass c2 = env->FindClass(kCls);
    if (c2) {
        jmethodID m = env->GetStaticMethodID(c2, kName, kProto);
        if (m) {
            jbyteArray empty = env->NewByteArray(0);
            jstring got = (jstring) env->CallStaticObjectMethod(c2, m, empty);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                LOGW("sign spoof: self-check threw");
            } else if (got) {
                const char *s = env->GetStringUTFChars(got, nullptr);
                LOGI("sign spoof: self-check -> %s", s ? s : "(null)");
                if (s) env->ReleaseStringUTFChars(got, s);
                env->DeleteLocalRef(got);
            }
            if (empty) env->DeleteLocalRef(empty);
        } else {
            env->ExceptionClear();
        }
        env->DeleteLocalRef(c2);
    }
    return true;
}
