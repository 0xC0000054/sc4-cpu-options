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

#include "CpuScheduling.h"
#include "Logger.h"
#include "ParallelRenderCull.h"
#include "SimTickBudget.h"
#include "version.h"
#include "cIGZCmdLine.h"
#include "cIGZFrameWork.h"
#include "cRZBaseString.h"
#include "cRZCOMDllDirector.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <Windows.h>

static constexpr uint32_t kCPUOptionsDllDirector = 0x0C148B57;

static constexpr std::string_view PluginLogFileName = "SC4CPUOptions.log";

using namespace std::literals::string_view_literals; // Required for the sv suffix

namespace
{
	bool EqualsIgnoreCase(const std::string_view& lhs, const std::string_view& rhs)
	{
		return lhs.length() == rhs.length()
			&& _strnicmp(lhs.data(), rhs.data(), lhs.length()) == 0;
	}

	std::filesystem::path GetDllFolderPath()
	{
		// This DLL, found from the address of one of its own functions
		HMODULE module = nullptr;
		GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&GetDllFolderPath),
			&module);

		// Grown until the whole path fits, for paths longer than MAX_PATH
		std::vector<wchar_t> path(MAX_PATH);
		for (;;)
		{
			DWORD const length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
			if (length == 0)
			{
				return {};
			}
			if (length < path.size())
			{
				return std::filesystem::path(std::wstring(path.data(), length)).parent_path();
			}
			path.resize(path.size() * 2);
		}
	}

	DWORD_PTR threadAffinityMask = 0;

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
		//
		// Only the game's own threads are affected: threads other DLLs create, the parallel
		// render cull's workers among them, go through those DLLs' imports.

		const DWORD_PTR imageBase = reinterpret_cast<DWORD_PTR>(GetModuleHandle(nullptr));
		const PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(imageBase);
		const PIMAGE_NT_HEADERS ntHeader = reinterpret_cast<PIMAGE_NT_HEADERS>(imageBase + dosHeader->e_lfanew);

		auto importBaseAddress = ntHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
		auto importDescriptor = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(imageBase + importBaseAddress);

		for (; importDescriptor->Name != 0; importDescriptor++)
		{
			auto importName = reinterpret_cast<const char*>(imageBase + importDescriptor->Name);

			if (_stricmp(importName, "Kernel32.DLL") != 0 || importDescriptor->OriginalFirstThunk == 0)
			{
				continue;
			}

			const auto originalFirstThunkData = reinterpret_cast<PIMAGE_THUNK_DATA>(imageBase + importDescriptor->OriginalFirstThunk);
			auto functionAddresses = reinterpret_cast<PDWORD_PTR>(imageBase + importDescriptor->FirstThunk);

			for (DWORD i = 0; originalFirstThunkData[i].u1.Ordinal != 0; i++)
			{
				const auto& originalThunk = originalFirstThunkData[i];

				// SimCity 4 imports CreateThread by name.
				if ((originalThunk.u1.Ordinal & IMAGE_ORDINAL_FLAG) != 0)
				{
					continue;
				}

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

	unsigned GetBitIndex(DWORD_PTR singleBit)
	{
		unsigned index = 0;
		while ((singleBit >>= 1) != 0)
		{
			++index;
		}
		return index;
	}

	unsigned CountBits(DWORD_PTR mask)
	{
		unsigned count = 0;
		for (; mask != 0; mask &= mask - 1)
		{
			++count;
		}
		return count;
	}

	// Puts the game's own threads on one core: a performance core on a hybrid CPU. Returns the
	// core's affinity bit, or 0 when the threads were left alone.
	DWORD_PTR ConfigureForSingleCPU(const CpuScheduling::Result& scheduling)
	{
		Logger& logger = Logger::GetInstance();

		DWORD_PTR const candidates = scheduling.performanceCoreMask & scheduling.allowedMask;
		DWORD_PTR const core = GetLowestSetBitMask(candidates != 0 ? candidates : scheduling.allowedMask);
		if (core == 0)
		{
			return 0;
		}

		threadAffinityMask = core;

		// Install a hook that sets the thread affinity for any additional threads SC4 creates.
		if (!InstallCreateThreadIATHook())
		{
			threadAffinityMask = 0;
			logger.WriteLine(LogLevel::Error, "Failed to install the CreateThread hook.");
			return 0;
		}

		// Set the thread affinity for the current thread, which should be the game's main
		// thread.
		if (SetThreadAffinityMask(GetCurrentThread(), core) == 0)
		{
			logger.WriteLineFormatted(
				LogLevel::Error,
				"Failed to set the main thread's affinity (Win32 error %lu).",
				GetLastError());
		}

		logger.WriteLineFormatted(
			LogLevel::Info,
			"Configured the game's threads to use 1 CPU core: logical processor %u%s.",
			GetBitIndex(core),
			candidates != 0 ? ", a performance core" : "");
		return core;
	}

	void ProcessCPUPriorityValue(const std::string& priority, bool isCPUPriorityArgument)
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
			if (SetPriorityClass(GetCurrentProcess(), cpuPriority))
			{
				logger.WriteLineFormatted(
					LogLevel::Info,
					"Set the game's CPU priority to %s.",
					priority.c_str());
			}
			else
			{
				logger.WriteLineFormatted(
					LogLevel::Error,
					"An OS error occurred when setting the CPU priority (Win32 error %lu).",
					GetLastError());
			}
		}
	}
}

class CPUOptionsDllDirector final : public cRZCOMDllDirector
{
public:

	CPUOptionsDllDirector()
	{
		std::filesystem::path logFilePath = GetDllFolderPath();
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
		cIGZFrameWork* const pFramework = RZGetFramework();

		const cIGZCmdLine* pCmdLine = pFramework->CommandLine();

		// Everything is automatic. On a hybrid CPU the process stays on the performance cores.
		CpuScheduling::Result const scheduling = CpuScheduling::Apply({ true, true });

		// SimCity 4's own threads run on one core, which avoids the game's multi-core crashes,
		// and the parallel render cull's workers get the other cores. With -CPUCount on the
		// command line the game decides its cores itself, and the workers share them.
		cRZBaseString value;
		unsigned workers = 0;
		DWORD_PTR workerAffinity = 0;

		if (pCmdLine->IsSwitchPresent(cRZBaseString("CPUCount"), value, true))
		{
			Logger::GetInstance().WriteLineFormatted(
				LogLevel::Info,
				"Left the game's threads on its own cores because the command line contains -CPUCount:%s.",
				value.ToChar());

			// The render thread gathers too, so one processor stays its own.
			workerAffinity = scheduling.allowedMask;
			workers = CountBits(workerAffinity) > 0 ? CountBits(workerAffinity) - 1 : 0;
		}
		else if (DWORD_PTR const gameCore = ConfigureForSingleCPU(scheduling); gameCore != 0)
		{
			// Off the game's whole physical core, its SMT sibling too.
			workerAffinity = scheduling.allowedMask
				& ~CpuScheduling::SameCoreMask(scheduling.processors, GetBitIndex(gameCore));
			workers = CountBits(workerAffinity);
		}
		else
		{
			workerAffinity = scheduling.allowedMask;
			workers = CountBits(workerAffinity) > 0 ? CountBits(workerAffinity) - 1 : 0;
		}

		// High priority, unless the command line asks for another with -CPUPriority. The game
		// applies -CPUPriority:Low itself; the plugin adds the other values.
		if (pCmdLine->IsSwitchPresent(cRZBaseString("CPUPriority"), value, true))
		{
			ProcessCPUPriorityValue(value.ToChar(), true);
		}
		else
		{
			ProcessCPUPriorityValue("High", false);
		}

		// -ParallelCull:off turns the parallel render cull off, for troubleshooting.
		ParallelRenderCull::Mode cullMode = ParallelRenderCull::Mode::Parallel;
		if (pCmdLine->IsSwitchPresent(cRZBaseString("ParallelCull"), value, true))
		{
			cullMode = ParallelRenderCull::ParseMode(value.ToChar());
		}
		ParallelRenderCull::Install(workers, workerAffinity, cullMode);

		// -SimTickCap:<milliseconds|off> changes the simulation tick budget, for troubleshooting.
		int simTickCap = SimTickBudget::kDefaultCapMs;
		if (pCmdLine->IsSwitchPresent(cRZBaseString("SimTickCap"), value, true))
		{
			simTickCap = std::atoi(value.ToChar());
		}
		SimTickBudget::Install(simTickCap);

		// For PreAppShutdown, which takes the patches out again.
		pFramework->AddHook(this);

		return true;
	}

	bool PreAppShutdown()
	{
		ParallelRenderCull::Uninstall();
		SimTickBudget::Uninstall();
		return true;
	}
};

cRZCOMDllDirector* RZGetCOMDllDirector()
{
	static CPUOptionsDllDirector sDirector;
	return &sDirector;
}
