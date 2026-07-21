#include <windows.h>
#include <devguid.h>
#include <newdev.h>
#include <setupapi.h>

#include "SoundSpatializerDriverContract.h"

#include <cwchar>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr wchar_t HardwareId[] = SOUND_SPATIALIZER_AUDIO_HARDWARE_ID;
constexpr wchar_t HardwareIdMultiSz[] = SOUND_SPATIALIZER_AUDIO_HARDWARE_ID L"\0";

class DeviceInfoSet final
{
public:
    explicit DeviceInfoSet(HDEVINFO value) noexcept : value_(value) {}
    ~DeviceInfoSet()
    {
        if (value_ != INVALID_HANDLE_VALUE)
        {
            SetupDiDestroyDeviceInfoList(value_);
        }
    }

    DeviceInfoSet(const DeviceInfoSet&) = delete;
    DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;
    DeviceInfoSet(DeviceInfoSet&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    DeviceInfoSet& operator=(DeviceInfoSet&& other) noexcept
    {
        if (this != &other)
        {
            if (value_ != INVALID_HANDLE_VALUE)
            {
                SetupDiDestroyDeviceInfoList(value_);
            }
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    [[nodiscard]] HDEVINFO get() const noexcept { return value_; }

private:
    HDEVINFO value_ = INVALID_HANDLE_VALUE;
};

std::wstring ErrorMessage(DWORD error)
{
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<wchar_t*>(&message),
        0,
        nullptr);

    std::wstring result = length != 0 && message != nullptr ? std::wstring(message, length) : L"Unknown error";
    if (message != nullptr)
    {
        LocalFree(message);
    }
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' '))
    {
        result.pop_back();
    }
    return result;
}

int Fail(const wchar_t* operation, DWORD error)
{
    std::wcerr << L"SoundSpatializer.DriverCtl: " << operation << L" failed (" << error << L"): "
               << ErrorMessage(error) << L'\n';
    return static_cast<int>(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
}

bool IsElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL success = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned);
    CloseHandle(token);
    return success != FALSE && elevation.TokenIsElevated != 0;
}

bool EqualsHardwareId(const wchar_t* multiSz)
{
    for (const wchar_t* current = multiSz; current != nullptr && *current != L'\0'; current += std::wcslen(current) + 1)
    {
        if (_wcsicmp(current, HardwareId) == 0)
        {
            return true;
        }
    }
    return false;
}

bool DeviceAlreadyExists(DWORD& error)
{
    DeviceInfoSet devices(SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES));
    if (devices.get() == INVALID_HANDLE_VALUE)
    {
        error = GetLastError();
        return false;
    }

    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(devices.get(), index, &device); ++index)
    {
        DWORD propertyType = 0;
        DWORD required = 0;
        SetupDiGetDeviceRegistryPropertyW(
            devices.get(), &device, SPDRP_HARDWAREID, &propertyType, nullptr, 0, &required);
        const DWORD propertyError = GetLastError();
        if (required == 0 || (propertyError != ERROR_INSUFFICIENT_BUFFER && propertyError != ERROR_SUCCESS))
        {
            continue;
        }

        std::vector<BYTE> buffer(required + sizeof(wchar_t), 0);
        if (SetupDiGetDeviceRegistryPropertyW(
                devices.get(), &device, SPDRP_HARDWAREID, &propertyType, buffer.data(), required, nullptr) &&
            (propertyType == REG_MULTI_SZ || propertyType == REG_SZ) &&
            EqualsHardwareId(reinterpret_cast<const wchar_t*>(buffer.data())))
        {
            error = ERROR_SUCCESS;
            return true;
        }
    }

    const DWORD enumerationError = GetLastError();
    if (enumerationError != ERROR_NO_MORE_ITEMS)
    {
        error = enumerationError;
        return false;
    }
    error = ERROR_SUCCESS;
    return false;
}

void RollBackCreatedDevice(HDEVINFO devices, SP_DEVINFO_DATA& device) noexcept
{
    SP_REMOVEDEVICE_PARAMS remove{};
    remove.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    remove.ClassInstallHeader.InstallFunction = DIF_REMOVE;
    remove.Scope = DI_REMOVEDEVICE_GLOBAL;
    remove.HwProfile = 0;
    if (SetupDiSetClassInstallParamsW(
            devices,
            &device,
            &remove.ClassInstallHeader,
            sizeof(remove)))
    {
        SetupDiCallClassInstaller(DIF_REMOVE, devices, &device);
    }
}

int Install(const std::wstring& infPath)
{
    wchar_t fullPathBuffer[32768]{};
    const DWORD fullPathLength = GetFullPathNameW(
        infPath.c_str(), static_cast<DWORD>(std::size(fullPathBuffer)), fullPathBuffer, nullptr);
    if (fullPathLength == 0 || fullPathLength >= std::size(fullPathBuffer))
    {
        return Fail(L"GetFullPathNameW", GetLastError());
    }
    const std::wstring fullPath(fullPathBuffer, fullPathLength);
    const DWORD attributes = GetFileAttributesW(fullPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        const DWORD pathError = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_FILE_NOT_FOUND;
        return Fail(L"locating INF", pathError);
    }

    GUID classGuid{};
    wchar_t className[256]{};
    if (!SetupDiGetINFClassW(fullPath.c_str(), &classGuid, className, 256, nullptr))
    {
        return Fail(L"SetupDiGetINFClassW", GetLastError());
    }
    if (!IsEqualGUID(classGuid, GUID_DEVCLASS_MEDIA))
    {
        return Fail(L"validating MEDIA class", ERROR_INVALID_DATA);
    }

    DWORD discoveryError = ERROR_SUCCESS;
    const bool exists = DeviceAlreadyExists(discoveryError);
    if (discoveryError != ERROR_SUCCESS)
    {
        return Fail(L"enumerating devices", discoveryError);
    }

    DeviceInfoSet createdDevices(INVALID_HANDLE_VALUE);
    SP_DEVINFO_DATA createdDevice{};
    createdDevice.cbSize = sizeof(createdDevice);
    bool created = false;

    if (!exists)
    {
        createdDevices = DeviceInfoSet(SetupDiCreateDeviceInfoList(&classGuid, nullptr));
        if (createdDevices.get() == INVALID_HANDLE_VALUE)
        {
            return Fail(L"SetupDiCreateDeviceInfoList", GetLastError());
        }
        if (!SetupDiCreateDeviceInfoW(
                createdDevices.get(),
                L"SoundSpatializerAudio",
                &classGuid,
                L"Sound Spatializer virtual audio endpoint",
                nullptr,
                DICD_GENERATE_ID,
                &createdDevice))
        {
            return Fail(L"SetupDiCreateDeviceInfoW", GetLastError());
        }
        if (!SetupDiSetDeviceRegistryPropertyW(
                createdDevices.get(),
                &createdDevice,
                SPDRP_HARDWAREID,
                reinterpret_cast<const BYTE*>(HardwareIdMultiSz),
                sizeof(HardwareIdMultiSz)))
        {
            return Fail(L"setting hardware ID", GetLastError());
        }
        if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, createdDevices.get(), &createdDevice))
        {
            return Fail(L"registering root device", GetLastError());
        }
        created = true;
    }

    BOOL rebootRequired = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(
            nullptr,
            HardwareId,
            fullPath.c_str(),
            INSTALLFLAG_FORCE | INSTALLFLAG_NONINTERACTIVE,
            &rebootRequired))
    {
        const DWORD updateError = GetLastError();
        if (created)
        {
            RollBackCreatedDevice(createdDevices.get(), createdDevice);
        }
        return Fail(L"UpdateDriverForPlugAndPlayDevicesW", updateError);
    }

    std::wcout << L"installed hardware-id=" << HardwareId
               << L" reboot-required=" << (rebootRequired ? L"1" : L"0") << L'\n';
    return ERROR_SUCCESS;
}
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc != 4 || _wcsicmp(argv[1], L"install") != 0 || _wcsicmp(argv[2], L"--inf") != 0)
    {
        std::wcerr << L"Usage: SoundSpatializer.DriverCtl.exe install --inf <absolute INF path>\n";
        return ERROR_BAD_ARGUMENTS;
    }
    if (!IsElevated())
    {
        return Fail(L"checking elevation", ERROR_ELEVATION_REQUIRED);
    }
    return Install(argv[3]);
}
