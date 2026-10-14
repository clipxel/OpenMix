#include "DriverCheck.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <setupapi.h>
 #if JUCE_MSVC
  #pragma comment (lib, "setupapi.lib")
 #endif
#endif

namespace om::DriverCheck
{

#if JUCE_WINDOWS

juce::StringArray presentUsbIds()
{
    juce::StringArray out;
    HDEVINFO set = SetupDiGetClassDevsW (nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return out;

    SP_DEVINFO_DATA info {};
    info.cbSize = sizeof (info);
    for (DWORD i = 0; SetupDiEnumDeviceInfo (set, i, &info); ++i)
    {
        wchar_t ids[2048] {};
        if (! SetupDiGetDeviceRegistryPropertyW (set, &info, SPDRP_HARDWAREID, nullptr, (PBYTE) ids, sizeof (ids) - sizeof (wchar_t), nullptr))
            continue;
        // REG_MULTI_SZ: "USB\VID_1234&PID_5678&REV_0100\0USB\VID_1234&PID_5678\0\0"
        for (const wchar_t* p = ids; *p != 0; p += wcslen (p) + 1)
        {
            const auto s = juce::String (p).toLowerCase();
            const int v = s.indexOf ("vid_"), q = s.indexOf ("pid_");
            if (v >= 0 && q >= 0)
                out.addIfNotAlreadyThere (s.substring (v + 4, v + 8) + ":" + s.substring (q + 4, q + 8));
        }
    }
    SetupDiDestroyDeviceInfoList (set);
    return out;
}

juce::File expandPath (const juce::String& path)
{
    wchar_t buf[MAX_PATH * 2] {};
    if (ExpandEnvironmentStringsW (path.toWideCharPointer(), buf, (DWORD) (MAX_PATH * 2)) == 0)
        return {};
    return juce::File (juce::String (buf));
}

#else

juce::StringArray presentUsbIds() { return {}; }

juce::File expandPath (const juce::String& path)
{
    return juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
}

#endif

} // namespace om::DriverCheck
