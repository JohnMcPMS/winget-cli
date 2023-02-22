// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "ConfigurationSetProcessorFactoryRemoting.h"

using namespace winrt::Microsoft::Management::Configuration;

namespace AppInstaller::CLI::Workflow::ConfigurationRemoting
{
    namespace details
    {
        // The layout of the memory being mapped.
        struct MappedMemoryValue
        {
            static constexpr ULONG s_MemorySize = 4 << 10;

            HRESULT Result;
            ULONG FactorySize;
            uint8_t FactoryObject[1];

            static ULONG MaxFactorySize()
            {
                static_assert(s_MemorySize > offsetof(MappedMemoryValue, FactoryObject));
                return s_MemorySize - offsetof(MappedMemoryValue, FactoryObject);
            }
        };
    }

	namespace
	{
        // Represents a remote factory object that was created from a specific process.
        struct RemoteFactory : winrt::implements<RemoteFactory, IConfigurationSetProcessorFactory>
        {
            RemoteFactory()
            {
                SECURITY_ATTRIBUTES securityAttributes{};
                securityAttributes.nLength = sizeof(securityAttributes);
                securityAttributes.bInheritHandle = TRUE;
                securityAttributes.lpSecurityDescriptor = nullptr;

                wil::unique_handle memoryHandle{ CreateFileMappingW(INVALID_HANDLE_VALUE, &securityAttributes, PAGE_READWRITE, 0, details::MappedMemoryValue::s_MemorySize, nullptr) };
                THROW_LAST_ERROR_IF_NULL(memoryHandle);

                wil::unique_mapview_ptr<details::MappedMemoryValue> mappedMemory{ reinterpret_cast<details::MappedMemoryValue*>(MapViewOfFile(memoryHandle.get(), FILE_MAP_READ | FILE_MAP_WRITE , 0, 0, 0)) };
                THROW_LAST_ERROR_IF_NULL(mappedMemory);
                // Initialize the result to a failure in case the other process never comes through
                mappedMemory->Result = E_FAIL;

                wil::unique_event initEvent;
                initEvent.create(wil::EventOptions::None, nullptr, &securityAttributes);

                m_completionMutex.create(nullptr, CREATE_MUTEX_INITIAL_OWNER, MUTEX_ALL_ACCESS, &securityAttributes);

                std::ostringstream argumentsStream;
                argumentsStream << "MMCP_OOP_Server.exe " << reinterpret_cast<INT_PTR>(memoryHandle.get()) << ' ' << reinterpret_cast<INT_PTR>(initEvent.get()) << ' ' << reinterpret_cast<INT_PTR>(m_completionMutex.get());
                std::string arguments = argumentsStream.str();

                // TODO: Explicitly find the server exe file
                STARTUPINFOA startupInfo{};
                startupInfo.cb = sizeof(startupInfo);
                wil::unique_process_information processInformation;
                THROW_IF_WIN32_BOOL_FALSE(CreateProcessA(R"(D:\mspkg\src\AppInstallerCLIPackage\bin\x64\Debug\AppX\MMCP_OOP_Server.exe)", &arguments[0], nullptr, nullptr, TRUE, DETACHED_PROCESS, nullptr, nullptr, &startupInfo, &processInformation));

                if (!initEvent.wait(200000))
                {
                    DWORD processExitCode = 0;
                    if (WaitForSingleObject(processInformation.hProcess, 0) == WAIT_OBJECT_0 && GetExitCodeProcess(processInformation.hProcess, &processExitCode) && FAILED(processExitCode))
                    {
                        THROW_HR(static_cast<HRESULT>(processExitCode));
                    }
                    else
                    {
                        THROW_HR(E_FAIL);
                    }
                }

                THROW_IF_FAILED(mappedMemory->Result);

                THROW_HR_IF(E_NOT_SUFFICIENT_BUFFER, mappedMemory->FactorySize == 0);
                THROW_HR_IF(E_NOT_SUFFICIENT_BUFFER, mappedMemory->FactorySize > details::MappedMemoryValue::MaxFactorySize());

                wil::com_ptr<IStream> stream;
                THROW_IF_FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
                THROW_IF_FAILED(stream->Write(mappedMemory->FactoryObject, mappedMemory->FactorySize, nullptr));
                THROW_IF_FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr));

                wil::com_ptr<IUnknown> output;
                THROW_IF_FAILED(CoUnmarshalInterface(stream.get(), winrt::guid_of<IConfigurationSetProcessorFactory>(), reinterpret_cast<void**>(&output)));
                m_remoteFactory = IConfigurationSetProcessorFactory{ output.detach(), winrt::take_ownership_from_abi };
            }

            IConfigurationSetProcessor CreateSetProcessor(const ConfigurationSet& configurationSet)
            {
                return m_remoteFactory.CreateSetProcessor(configurationSet);
            }

        private:
            IConfigurationSetProcessorFactory m_remoteFactory;
            wil::unique_mutex m_completionMutex;
        };
	}

	IConfigurationSetProcessorFactory CreateOutOfProcFactory()
	{
        return winrt::make_self<RemoteFactory>().as<IConfigurationSetProcessorFactory>();
	}
}

HRESULT WindowsPackageManagerConfigurationCompleteOutOfProcessFactoryInitialization(HRESULT result, void* factory, uint64_t memoryHandleIntPtr, uint64_t initEventHandleIntPtr, uint64_t completionMutexHandleIntPtr) try
{
    using namespace AppInstaller::CLI::Workflow::ConfigurationRemoting;

    RETURN_HR_IF(E_POINTER, !memoryHandleIntPtr);

    wil::unique_handle memoryHandle{ reinterpret_cast<HANDLE>(memoryHandleIntPtr) };
    wil::unique_mapview_ptr<details::MappedMemoryValue> mappedMemory{ reinterpret_cast<details::MappedMemoryValue*>(MapViewOfFile(memoryHandle.get(), FILE_MAP_WRITE, 0, 0, 0)) };
    RETURN_LAST_ERROR_IF_NULL(mappedMemory);

    mappedMemory->Result = result;
    mappedMemory->FactorySize = 0;

    if (SUCCEEDED(result))
    {
        wil::com_ptr<IStream> stream;
        RETURN_IF_FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream));

        RETURN_IF_FAILED(CoMarshalInterface(stream.get(), winrt::guid_of<IConfigurationSetProcessorFactory>(), reinterpret_cast<IUnknown*>(factory), MSHCTX_LOCAL, nullptr, MSHLFLAGS_NORMAL));

        ULARGE_INTEGER streamSize{};
        RETURN_IF_FAILED(stream->Seek({}, STREAM_SEEK_CUR, &streamSize));
        RETURN_HR_IF(E_NOT_SUFFICIENT_BUFFER, streamSize.QuadPart > details::MappedMemoryValue::MaxFactorySize());

        ULONG bufferSize = static_cast<ULONG>(streamSize.QuadPart);

        RETURN_IF_FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr));
        ULONG bytesRead = 0;
        RETURN_IF_FAILED(stream->Read(mappedMemory->FactoryObject, bufferSize, &bytesRead));
        RETURN_HR_IF(E_UNEXPECTED, bytesRead != bufferSize);

        mappedMemory->FactorySize = bufferSize;
    }

    wil::unique_event initEvent{ reinterpret_cast<HANDLE>(initEventHandleIntPtr) };
    initEvent.SetEvent();

    // Wait until the caller releases the object
    wil::unique_mutex completionMutex{ reinterpret_cast<HANDLE>(completionMutexHandleIntPtr) };
    std::ignore = completionMutex.acquire();

    return S_OK;
}
CATCH_RETURN();
