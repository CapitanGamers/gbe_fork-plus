#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>

struct XUser { uint64_t xuid; std::string gamertag; };
struct XTaskQueueObject { uint32_t dummy; };
struct XStoreContext { uint32_t dummy; };

struct XAsyncBlock {
    XTaskQueueObject* queue;
    void* context;
    void (CALLBACK* callback)(XAsyncBlock* block);
    uintptr_t internal[4];
};

static bool g_initialized = false;
static XUser g_user = { 0x01100001DEADBEEFULL, "WorldSpinPlayer" };

extern "C" {

__declspec(dllexport) HRESULT WINAPI XGameRuntimeInitialize() {
    g_initialized = true;
    return S_OK;
}

__declspec(dllexport) void WINAPI XGameRuntimeUninitialize() {
    g_initialized = false;
}

__declspec(dllexport) HRESULT WINAPI XTaskQueueCreate(int, int, XTaskQueueObject** q) {
    if (!q) return E_INVALIDARG;
    *q = new XTaskQueueObject{1};
    return S_OK;
}

__declspec(dllexport) void WINAPI XTaskQueueCloseHandle(XTaskQueueObject* q) { delete q; }
__declspec(dllexport) bool WINAPI XTaskQueueDispatch(XTaskQueueObject*, int, uint32_t) { return false; }

__declspec(dllexport) HRESULT WINAPI XTaskQueueSubmitCallback(XTaskQueueObject*, int, void* ctx, void (CALLBACK* cb)(void*, bool)) {
    if (cb) cb(ctx, false);
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI XUserAddAsync(uint32_t, XAsyncBlock* async) {
    if (async && async->callback) async->callback(async);
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI XUserAddResult(XAsyncBlock*, XUser** u) {
    if (!u) return E_INVALIDARG;
    *u = &g_user;
    return S_OK;
}

__declspec(dllexport) void WINAPI XUserCloseHandle(XUser*) {}

__declspec(dllexport) HRESULT WINAPI XUserGetId(XUser* u, uint64_t* id) {
    if (!id) return E_INVALIDARG;
    *id = u ? u->xuid : g_user.xuid;
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI XUserGetGamertag(XUser* u, int, size_t sz, char* buf, size_t* used) {
    if (!buf || sz == 0) return E_INVALIDARG;
    const std::string& t = u ? u->gamertag : g_user.gamertag;
    strncpy_s(buf, sz, t.c_str(), _TRUNCATE);
    if (used) *used = strlen(buf) + 1;
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI XUserGetState(XUser*, uint32_t* st) {
    if (!st) return E_INVALIDARG;
    *st = 0;
    return S_OK;
}

struct XStoreGameLicense {
    char skuStoreId[18];
    bool isActive;
    bool isTrialOwnedByThisUser;
    bool isDiscLicense;
    bool isTrial;
    uint32_t trialTimeRemainingInSeconds;
    char trialUniqueId[64];
    int64_t expirationDate;
};

__declspec(dllexport) HRESULT WINAPI XStoreQueryGameLicenseAsync(XStoreContext*, XAsyncBlock* async) {
    if (async && async->callback) async->callback(async);
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI XStoreQueryGameLicenseResult(XAsyncBlock*, XStoreGameLicense* lic) {
    if (!lic) return E_INVALIDARG;
    memset(lic, 0, sizeof(*lic));
    lic->isActive = true;
    lic->isTrial = false;
    return S_OK;
}

}
