// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "winget/PortableARPEntry.h"
#include "winget/Manifest.h"

using namespace AppInstaller::Utility;

#define VALUENAMECASE(valueName) case PortableValueName::valueName: return s_##valueName;

namespace AppInstaller::Registry::Portable
{
#ifndef AICLI_DISABLE_TEST_HOOKS
    static HKEY s_UserRoot_TestHook_Override = nullptr;
    static HKEY s_MachineRoot_TestHook_Override = nullptr;

    void TestHook_SetPortableARPEntryRoots_Override(HKEY userRoot, HKEY machineRoot)
    {
        THROW_HR_IF(E_INVALIDARG, (userRoot == nullptr) != (machineRoot == nullptr));
        THROW_HR_IF(E_UNEXPECTED, userRoot && s_UserRoot_TestHook_Override);
        s_UserRoot_TestHook_Override = userRoot;
        s_MachineRoot_TestHook_Override = machineRoot;
    }
#endif

    namespace
    {
        constexpr std::wstring_view s_UninstallRegistryX64 = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
        constexpr std::wstring_view s_UninstallRegistryX86 = L"Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
        constexpr std::wstring_view s_DisplayName = L"DisplayName";
        constexpr std::wstring_view s_DisplayVersion = L"DisplayVersion";
        constexpr std::wstring_view s_Publisher = L"Publisher";
        constexpr std::wstring_view s_InstallDate = L"InstallDate";
        constexpr std::wstring_view s_URLInfoAbout = L"URLInfoAbout";
        constexpr std::wstring_view s_HelpLink = L"HelpLink";
        constexpr std::wstring_view s_UninstallString = L"UninstallString";
        constexpr std::wstring_view s_WinGetInstallerType = L"WinGetInstallerType";
        constexpr std::wstring_view s_InstallLocation = L"InstallLocation";
        constexpr std::wstring_view s_PortableTargetFullPath = L"TargetFullPath";
        constexpr std::wstring_view s_PortableSymlinkFullPath = L"SymlinkFullPath";
        constexpr std::wstring_view s_SHA256 = L"SHA256";
        constexpr std::wstring_view s_WinGetPackageIdentifier = L"WinGetPackageIdentifier";
        constexpr std::wstring_view s_WinGetSourceIdentifier = L"WinGetSourceIdentifier";
        constexpr std::wstring_view s_InstallDirectoryCreated = L"InstallDirectoryCreated";
        constexpr std::wstring_view s_InstallDirectoryAddedToPath = L"InstallDirectoryAddedToPath";
    }

    PortableARPEntry::PortableARPEntry(Manifest::ScopeEnum scope, Utility::Architecture arch, const std::string& productCode)
    {
        m_scope = scope;
        m_arch = arch;
        m_productCode = productCode;

        if (m_scope == Manifest::ScopeEnum::Machine)
        {
            m_root = HKEY_LOCAL_MACHINE;
            if (m_arch == Utility::Architecture::X64)
            {
                m_subKey = s_UninstallRegistryX64;
                m_samDesired = KEY_WOW64_64KEY;
            }
            else
            {
                m_subKey = s_UninstallRegistryX86;
                m_samDesired = KEY_WOW64_32KEY;
            }
        }
        else
        {
            // HKCU uninstall registry share the x64 registry view.
            m_root = HKEY_CURRENT_USER;
            m_subKey = s_UninstallRegistryX64;
            m_samDesired = KEY_WOW64_64KEY;
        }

#ifndef AICLI_DISABLE_TEST_HOOKS
        if (s_UserRoot_TestHook_Override)
        {
            m_root = m_scope == Manifest::ScopeEnum::Machine ?
                s_MachineRoot_TestHook_Override : s_UserRoot_TestHook_Override;
        }
#endif

        m_subKey += L"\\" + ConvertToUTF16(m_productCode);
        m_key = Key::OpenIfExists(m_root, m_subKey, 0, KEY_ALL_ACCESS);
        m_exists = static_cast<bool>(m_key);
    }

    std::wstring_view ToString(PortableValueName valueName)
    {
        switch (valueName)
        {
            VALUENAMECASE(DisplayName);
            VALUENAMECASE(DisplayVersion);
            VALUENAMECASE(Publisher);
            VALUENAMECASE(InstallDate);
            VALUENAMECASE(URLInfoAbout);
            VALUENAMECASE(HelpLink);
            VALUENAMECASE(UninstallString);
            VALUENAMECASE(WinGetInstallerType);
            VALUENAMECASE(InstallLocation);
            VALUENAMECASE(PortableTargetFullPath);
            VALUENAMECASE(PortableSymlinkFullPath);
            VALUENAMECASE(SHA256);
            VALUENAMECASE(WinGetPackageIdentifier);
            VALUENAMECASE(WinGetSourceIdentifier);
            VALUENAMECASE(InstallDirectoryCreated);
            VALUENAMECASE(InstallDirectoryAddedToPath);
            default: return {};
        }
    }

    std::optional<Value> PortableARPEntry::operator[](PortableValueName valueName) const
    {
        const auto key = GetKey();
        return key ? key[std::wstring{ ToString(valueName) }] : std::nullopt;
    }

    void PortableARPEntry::SetValue(PortableValueName valueName, const std::wstring& value)
    {
        GetOrCreateKey().SetValue(std::wstring{ ToString(valueName) }, value, REG_SZ);
    }

    void PortableARPEntry::SetValue(PortableValueName valueName, const std::string_view& value)
    {
        SetValue(valueName, ConvertToUTF16(value));
    }

    void PortableARPEntry::SetValue(PortableValueName valueName, bool& value)
    {
        GetOrCreateKey().SetValue(std::wstring{ ToString(valueName) }, value);
    }

    bool PortableARPEntry::IsEmpty() const
    {
        DWORD subKeys = 0;
        DWORD values = 0;
        THROW_IF_WIN32_ERROR(RegQueryInfoKeyW(m_key, nullptr, nullptr, nullptr, &subKeys, nullptr, nullptr,
            &values, nullptr, nullptr, nullptr, nullptr));
        return subKeys == 0 && values == 0;
    }

    Registry::Key PortableARPEntry::GetKey() const
    {
        return m_key ? m_key : Key::OpenIfExists(m_root, m_subKey, 0, KEY_ALL_ACCESS);
    }

    Key& PortableARPEntry::GetOrCreateKey()
    {
        if (!m_key)
        {
#ifndef AICLI_DISABLE_TEST_HOOKS
            if (m_root == s_UserRoot_TestHook_Override || m_root == s_MachineRoot_TestHook_Override)
            {
                m_key = Key::Create(m_root, m_subKey, REG_OPTION_VOLATILE);
                return m_key;
            }
#endif
            m_key = Key::Create(m_root, m_subKey);
        }

        return m_key;
    }

    void PortableARPEntry::Delete()
    {
        Registry::Key::Delete(m_root, m_subKey, m_samDesired);
    }
}