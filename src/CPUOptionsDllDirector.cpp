////////////////////////////////////////////////////////////////////////
//
// This file is part of sc4-cpu-options, a DLL Plugin for SimCity 4
// that configures the CPU core count and priority.
//
// Copyright (c) 2024, 2025 Nicholas Hayes
//
// This file is licensed under terms of the MIT License.
// See LICENSE.txt for more information.
//
////////////////////////////////////////////////////////////////////////

#include "Logger.h"
#include "version.h"
#include "cIGZCmdLine.h"
#include "cIGZFrameWork.h"
#include "cRZBaseString.h"
#include "cRZCOMDllDirector.h"

#include <boost/algorithm/string.hpp>
#include "boost/property_tree/ptree.hpp"
#include "boost/property_tree/ini_parser.hpp"

#include <string>

#include <Windows.h>
#include "wil/resource.h"
#include "wil/result.h"
#include "wil/win32_helpers.h"

static constexpr uint32_t kCPUOptionsDllDirector = 0x0C148B57;

static constexpr std::string_view PluginConfigFileName = "SC4CPUOptions.ini";
static constexpr std::string_view PluginLogFileName = "SC4CPUOptions.log";

using namespace std::literals::string_view_literals; // Required for the sv suffix

namespace
{
	bool EqualsIgnoreCase(const std::string_view& lhs, const std::string_view& rhs)
	{
		return lhs.length() == rhs.length()
			&& boost::iequals(lhs, rhs);
	}

	std::filesystem::path GetDllFolderPath()
	{
		wil::unique_cotaskmem_string modulePath = wil::GetModuleFileNameW(wil::GetModuleInstanceHandle());

		std::filesystem::path temp(modulePath.get());

		return temp.parent_path();
	}

	static DWORD_PTR threadAffinityMask = 0;

	HANDLE WINAPI HookedCreateThread(
		_In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
		_In_ SIZE_T dwStackSize,
		_In_ LPTHREAD_START_ROUTINE lpStartAddress,
		_In_opt_ __drv_aliasesMem LPVOID lpParameter,
		_In_ DWORD dwCreationFlags,
		_Out_opt_ LPDWORD lpThreadId
	)
	{
		HANDLE hThread = CreateThread(lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, lpThreadId);

		if (hThread && threadAffinityMask != 0)
		{
			SetThreadAffinityMask(hThread, threadAffinityMask);
		}

		return hThread;
	}

	bool InstallCreateThreadIATHook()
	{
		// We walk the Import Address Table (IAT) in SimCity 4.exe and replace the imported
		// pointer to CreateThread with a pointer to our CreateThread wrapper function.
		// This makes any new threads that SC4 creates go through our wrapper that sets the thread affinity.

		const DWORD_PTR imageBase = reinterpret_cast<DWORD_PTR>(GetModuleHandle(nullptr));
		const PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(imageBase);
		const PIMAGE_NT_HEADERS ntHeader = reinterpret_cast<PIMAGE_NT_HEADERS>(imageBase + dosHeader->e_lfanew);

		auto importBaseAddress = ntHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
		auto importDescriptor = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(imageBase + importBaseAddress);

		for (; importDescriptor->Name != 0; importDescriptor++)
		{
			auto importName = reinterpret_cast<const char*>(imageBase + importDescriptor->Name);

			if (_stricmp(importName, "Kernel32.DLL"))
			{
				const auto originalFirstThunkData = reinterpret_cast<PIMAGE_THUNK_DATA>(imageBase + importDescriptor->OriginalFirstThunk);
				auto functionAddresses = reinterpret_cast<PDWORD_PTR>(imageBase + importDescriptor->FirstThunk);

				if (importDescriptor->OriginalFirstThunk != 0)
				{
					for (DWORD i = 0; originalFirstThunkData->u1.Ordinal != 0; i++)
					{
						const auto& originalThunk = originalFirstThunkData[i];

						// SimCity 4 imports CreateThread by name.
						if ((originalThunk.u1.Ordinal & IMAGE_ORDINAL_FLAG) == 0)
						{
							const auto importData = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(imageBase + originalThunk.u1.AddressOfData);

							if (strcmp(importData->Name, "CreateThread") == 0)
							{
								DWORD oldProtect = 0;

								if (!VirtualProtect(&functionAddresses[i], sizeof(DWORD_PTR), PAGE_READWRITE, &oldProtect))
								{
									return false;
								}

								functionAddresses[i] = reinterpret_cast<DWORD_PTR>(&HookedCreateThread);

								DWORD oldProtect2 = 0;

								VirtualProtect(&functionAddresses[i], sizeof(DWORD_PTR), oldProtect, &oldProtect2);

								return true;
							}
						}
					}
				}
			}
		}

		return false;
	}

	DWORD_PTR GetLowestSetBitMask(DWORD_PTR value)
	{
		// Adapted from https://stackoverflow.com/a/12250963
		//
		// This relies on the fact that C++ integers are "two's complement".
		// For example: 15 (00001111) & -15 (11110001) would return 1 (00000001).
		return static_cast<DWORD_PTR>(static_cast<uintptr_t>(value) & -static_cast<intptr_t>(value));
	}

	void ConfigureForSingleCPU()
	{
		Logger& logger = Logger::GetInstance();

		try
		{
			DWORD_PTR processAffinityMask = 0;
			DWORD_PTR systemAffinityMask = 0;

			THROW_IF_WIN32_BOOL_FALSE(GetProcessAffinityMask(GetCurrentProcess(), &processAffinityMask, &systemAffinityMask));

			// SetThreadAffinityMask takes a bit mask that specifies which physical/logical CPU cores
			// are used for the process, we select the first core that is enabled in the system mask.
			//
			// We do this instead of hard-coding a value of 1 to handle the case where the system mask
			// doesn't have the first logical processor enabled.
			threadAffinityMask = GetLowestSetBitMask(systemAffinityMask);

			// Install a hook that sets the thread affinity for any additional threads SC4 creates.
			if (InstallCreateThreadIATHook())
			{
				// Set the thread affinity for the current thread, which should be the game's main
				// thread.
				SetThreadAffinityMask(GetCurrentThread(), threadAffinityMask);
				logger.WriteLine(
					LogLevel::Info,
					"Configured the game's threads to use 1 CPU core.");
			}
			else
			{
				logger.WriteLine(
					LogLevel::Error,
					"Failed to install the CreateThread hook.");
			}
		}
		catch (const wil::ResultException& e)
		{
			logger.WriteLineFormatted(
				LogLevel::Error,
				"An OS error occurred when configuring the game's threads to use 1 CPU core: %s.",
				e.what());
		}
	}

	void ProcessCPUPriorityValue(const std::string& priority, bool isCPUPriorityArgument = false)
	{
		Logger& logger = Logger::GetInstance();

		DWORD cpuPriority = 0;

		if (EqualsIgnoreCase(priority, "High"sv))
		{
			cpuPriority = HIGH_PRIORITY_CLASS;
		}
		else if (EqualsIgnoreCase(priority, "AboveNormal"sv))
		{
			cpuPriority = ABOVE_NORMAL_PRIORITY_CLASS;
		}
		else if (EqualsIgnoreCase(priority, "Normal"sv))
		{
			// Normal should be the default for a new process, but there is no harm in
			// allowing the user to select it anyway.
			cpuPriority = NORMAL_PRIORITY_CLASS;
		}
		else if (EqualsIgnoreCase(priority, "BelowNormal"sv))
		{
			cpuPriority = BELOW_NORMAL_PRIORITY_CLASS;
		}
		else if (EqualsIgnoreCase(priority, "Idle"sv))
		{
			cpuPriority = IDLE_PRIORITY_CLASS;
		}
		else if (EqualsIgnoreCase(priority, "Low"sv))
		{
			// If we are processing a value from the game's -CPUPriority command line
			// argument we treat the value of Low as a no-op, because SC4 will have
			// applied it before the DLL is loaded.
			// Otherwise it is treated as an alias for Idle.
			if (isCPUPriorityArgument)
			{
				logger.WriteLine(LogLevel::Info, "SC4 set its CPU priority to Low.");
			}
			else
			{
				cpuPriority = IDLE_PRIORITY_CLASS;
			}
		}
		else
		{
			logger.WriteLineFormatted(
				LogLevel::Error,
				"Unsupported CPU priority value: %s",
				priority.c_str());
		}


		if (cpuPriority != 0)
		{
			try
			{
				THROW_IF_WIN32_BOOL_FALSE(SetPriorityClass(GetCurrentProcess(), cpuPriority));

				logger.WriteLineFormatted(
					LogLevel::Info,
					"Set the game's CPU priority to %s.",
					priority.c_str());
			}
			catch (const wil::ResultException& e)
			{
				logger.WriteLineFormatted(
					LogLevel::Error,
					"An OS error occurred when setting the CPU priority: %s.",
					e.what());
			}
		}
	}
}

class CPUOptionsDllDirector final : public cRZCOMDllDirector
{
public:

	CPUOptionsDllDirector()
	{
		std::filesystem::path dllFolderPath = GetDllFolderPath();

		configFilePath = dllFolderPath;
		configFilePath /= PluginConfigFileName;

		std::filesystem::path logFilePath = dllFolderPath;
		logFilePath /= PluginLogFileName;

		Logger& logger = Logger::GetInstance();
		logger.Init(logFilePath, LogLevel::Error);
		logger.WriteLogFileHeader("SC4CPUOptions v" PLUGIN_VERSION_STR);
	}

	uint32_t GetDirectorID() const
	{
		return kCPUOptionsDllDirector;
	}

	bool OnStart(cIGZCOM* pCOM)
	{
		Logger& logger = Logger::GetInstance();

		cIGZFrameWork* const pFramework = RZGetFramework();

		const cIGZCmdLine* pCmdLine = pFramework->CommandLine();

		// If the user set the -CPUCount and/or -CPUPriority command line arguments,
		// those values will be used in place of the plugin's default options.
		// When those command line arguments are not present, the plugin will configure
		// SC4 to use 1 CPU and the CPU priority specified in the configuration file.

		cRZBaseString value;
		if (pCmdLine->IsSwitchPresent(cRZBaseString("CPUCount"), value, true))
		{
			logger.WriteLineFormatted(
				LogLevel::Info,
				"Skipped forcing the game to a single CPU because the command line contains -CPUCount:%s.",
				value.ToChar());
		}
		else
		{
			ConfigureForSingleCPU();
		}

		if (pCmdLine->IsSwitchPresent(cRZBaseString("CPUPriority"), value, true))
		{
			// We extend the -CPUPriority command line argument with a few more supported values.
			// SC4 only supports 1 value -CPUPriority:Low, which we treat as a no-op because the
			// game will have already applied it by the time the DLL is loaded.
			ProcessCPUPriorityValue(value.ToChar(), true);
		}
		else
		{
			SetCPUPriorityFromConfigFile();
		}

		return true;
	}

private:

	void SetCPUPriorityFromConfigFile() const
	{
		Logger& logger = Logger::GetInstance();

		try
		{
			std::ifstream stream(configFilePath, std::ifstream::in);

			if (!stream)
			{
				throw std::runtime_error("Failed to open the settings file.");
			}

			boost::property_tree::ptree tree;

			boost::property_tree::ini_parser::read_ini(stream, tree);

			std::string cpuPriority = tree.get<std::string>("CPUOptions.Priority");

			ProcessCPUPriorityValue(cpuPriority);
		}
		catch (const std::exception& e)
		{
			logger.WriteLineFormatted(
				LogLevel::Error,
				"Error when setting the CPU priority: %s",
				e.what());
		}
	}

	std::filesystem::path configFilePath;
};

cRZCOMDllDirector* RZGetCOMDllDirector() {
	static CPUOptionsDllDirector sDirector;
	return &sDirector;
}