// Fork: deterministic SDK ownership double, loaded by the real SDRplay driver.
#include "../../plugins/sdr_sources/sdrplay_sdr_support/sdrplay/sdrplay_api.h"
#include <cstring>
namespace { int releases = 0, invalid_releases = 0; bool selected = false, fail_select = false; }
extern "C" void fake_sdrplay_reset() { releases = invalid_releases = 0; selected = fail_select = false; }
extern "C" void fake_sdrplay_fail_selection() { fail_select = true; }
extern "C" int fake_sdrplay_releases() { return releases; }
extern "C" int fake_sdrplay_invalid_releases() { return invalid_releases; }
extern "C" sdrplay_api_ErrT sdrplay_api_Open() { return sdrplay_api_Success; }
extern "C" sdrplay_api_ErrT sdrplay_api_Close() { return sdrplay_api_Success; }
extern "C" sdrplay_api_ErrT sdrplay_api_ApiVersion(float *version) { *version = SDRPLAY_API_VERSION; return sdrplay_api_Success; }
extern "C" sdrplay_api_ErrT sdrplay_api_SelectDevice(sdrplay_api_DeviceT *)
{
    if (fail_select) { fail_select = false; return sdrplay_api_Fail; }
    selected = true;
    return sdrplay_api_Success;
}
extern "C" sdrplay_api_ErrT sdrplay_api_ReleaseDevice(sdrplay_api_DeviceT *)
{
    ++releases;
    if (!selected) ++invalid_releases;
    selected = false;
    return sdrplay_api_Success;
}
extern "C" sdrplay_api_ErrT sdrplay_api_GetDevices(sdrplay_api_DeviceT *devices, unsigned int *count, unsigned int)
{
    devices[0] = {};
    devices[0].hwVer = SDRPLAY_RSP1_ID;
    std::strcpy(devices[0].SerNo, "1");
    *count = 1;
    return sdrplay_api_Success;
}
extern "C" sdrplay_api_ErrT sdrplay_api_UnlockDeviceApi() { return sdrplay_api_Success; }
extern "C" sdrplay_api_ErrT sdrplay_api_DebugEnable(HANDLE, sdrplay_api_DbgLvl_t) { return sdrplay_api_Success; }
extern "C" sdrplay_api_ErrT sdrplay_api_GetDeviceParams(HANDLE, sdrplay_api_DeviceParamsT **) { return sdrplay_api_Fail; }
extern "C" sdrplay_api_ErrT sdrplay_api_Init(HANDLE, sdrplay_api_CallbackFnsT *, void *) { return sdrplay_api_Success; }
extern "C" sdrplay_api_ErrT sdrplay_api_Uninit(HANDLE) { return sdrplay_api_Success; }
extern "C" sdrplay_api_ErrT sdrplay_api_Update(HANDLE, sdrplay_api_TunerSelectT, sdrplay_api_ReasonForUpdateT, sdrplay_api_ReasonForUpdateExtension1T) { return sdrplay_api_Success; }
