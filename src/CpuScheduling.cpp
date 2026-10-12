/*
 *  This file is part of sc4-cpu-options. It comes from SCD3D11, a free Direct3D 11
 *  driver for SimCity 4's SimGL interface (https://github.com/caspervg/scd3d11). * 
 *  Copyright (C) 2026 Casper Van Gheluwe
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "CpuScheduling.h"

#include "Logger.h"

#include <Windows.h>

#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

namespace CpuScheduling
{
	namespace
	{
		unsigned CountBits(uintptr_t mask)
		{
			unsigned count = 0;
			for (; mask != 0; mask &= mask - 1)
			{
				++count;
			}
			return count;
		}

		// Windows 8 / 10 APIs, resolved at runtime so the DLL still loads on Windows 7. The type comes
		// from the SDK declaration: a hand-written stdcall signature with the wrong parameter count
		// unbalances the stack.
#define KERNEL32_FUNCTION(name) \
	reinterpret_cast<decltype(&::name)>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), #name))

		// Without this, Windows 11 treats the game as EcoQoS whenever it is not the foreground
		// window and steers its threads onto E-cores, and ignores its timer-resolution requests.
		void DisablePowerThrottling()
		{
			Logger& logger = Logger::GetInstance();

			auto const setProcessInformation = KERNEL32_FUNCTION(SetProcessInformation);
			if (setProcessInformation == nullptr)
			{
				return;
			}

			PROCESS_POWER_THROTTLING_STATE state{};
			state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
			state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
			state.StateMask = 0;
			if (!setProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)))
			{
				// Windows 10 rejects the timer-resolution bit.
				state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
				if (!setProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)))
				{
					logger.WriteLineFormatted(
						LogLevel::Error,
						"Failed to disable EcoQoS power throttling (Win32 error %lu).",
						GetLastError());
					return;
				}
			}
			logger.WriteLine(LogLevel::Info, "Disabled EcoQoS power throttling.");
		}

		std::vector<LogicalProcessor> ReadProcessors()
		{
			std::vector<LogicalProcessor> processors;

			auto const getInformation = KERNEL32_FUNCTION(GetSystemCpuSetInformation);
			if (getInformation == nullptr)
			{
				return processors;
			}

			ULONG size = 0;
			getInformation(nullptr, 0, &size, GetCurrentProcess(), 0);
			if (size == 0)
			{
				return processors;
			}
			std::vector<uint8_t> buffer(size);
			if (!getInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), size, &size,
				GetCurrentProcess(), 0))
			{
				return processors;
			}

			for (ULONG offset = 0; offset < size;)
			{
				auto const* entry = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION const*>(buffer.data() + offset);
				if (entry->Size == 0)
				{
					break;
				}
				if (entry->Type == CpuSetInformation)
				{
					processors.push_back({
						entry->CpuSet.Id, entry->CpuSet.Group, entry->CpuSet.LogicalProcessorIndex,
						entry->CpuSet.EfficiencyClass, entry->CpuSet.CoreIndex
						});
				}
				offset += entry->Size;
			}

			return processors;
		}

		unsigned PreferPerformanceCores(std::vector<LogicalProcessor> const& processors, uintptr_t affinity)
		{
			Logger& logger = Logger::GetInstance();

			unsigned const allowed = CountBits(affinity);
			auto const setDefaults = KERNEL32_FUNCTION(SetProcessDefaultCpuSets);
			if (setDefaults == nullptr)
			{
				return allowed;
			}

			std::vector<uint32_t> const selected = SelectPerformanceCpuSets(processors, affinity);
			if (selected.empty())
			{
				return allowed;
			}
			if (!setDefaults(GetCurrentProcess(), reinterpret_cast<ULONG const*>(selected.data()),
				static_cast<USHORT>(selected.size())))
			{
				logger.WriteLineFormatted(
					LogLevel::Error,
					"SetProcessDefaultCpuSets failed (Win32 error %lu).",
					GetLastError());
				return allowed;
			}
			logger.WriteLineFormatted(
				LogLevel::Info,
				"Limited the game's threads to %u performance-core logical processors of %u.",
				static_cast<unsigned>(selected.size()),
				allowed);
			return static_cast<unsigned>(selected.size());
		}

		bool IsAllowed(LogicalProcessor const& processor, uintptr_t affinityMask)
		{
			return processor.group == 0
				&& processor.index < sizeof(uintptr_t) * 8
				&& ((affinityMask >> processor.index) & 1) != 0;
		}

		// The fastest efficiency class within the mask, or -1 when every allowed processor is
		// in the same class.
		int FastestClass(std::vector<LogicalProcessor> const& processors, uintptr_t affinityMask)
		{
			uint8_t fastest = 0;
			uint8_t slowest = UINT8_MAX;
			for (auto const& processor : processors)
			{
				if (!IsAllowed(processor, affinityMask))
				{
					continue;
				}
				if (processor.efficiencyClass > fastest)
				{
					fastest = processor.efficiencyClass;
				}
				if (processor.efficiencyClass < slowest)
				{
					slowest = processor.efficiencyClass;
				}
			}
			return fastest > slowest ? fastest : -1;
		}
	}

	std::vector<uint32_t> SelectPerformanceCpuSets(
		std::vector<LogicalProcessor> const& processors,
		uintptr_t affinityMask)
	{
		int const fastest = FastestClass(processors, affinityMask);
		if (fastest < 0)
		{
			return {};
		}

		std::vector<uint32_t> selected;
		for (auto const& processor : processors)
		{
			if (IsAllowed(processor, affinityMask) && processor.efficiencyClass == fastest)
			{
				selected.push_back(processor.cpuSetId);
			}
		}
		if (selected.size() < 2)
		{
			selected.clear();
		}
		return selected;
	}

	uintptr_t PerformanceCoreMask(
		std::vector<LogicalProcessor> const& processors,
		uintptr_t affinityMask)
	{
		int const fastest = FastestClass(processors, affinityMask);
		if (fastest < 0)
		{
			return 0;
		}

		uintptr_t mask = 0;
		for (auto const& processor : processors)
		{
			if (IsAllowed(processor, affinityMask) && processor.efficiencyClass == fastest)
			{
				mask |= uintptr_t{ 1 } << processor.index;
			}
		}
		return mask;
	}

	uintptr_t SameCoreMask(
		std::vector<LogicalProcessor> const& processors,
		unsigned index)
	{
		uintptr_t mask = index < sizeof(uintptr_t) * 8 ? uintptr_t{ 1 } << index : 0;

		for (auto const& processor : processors)
		{
			if (processor.group != 0 || processor.index != index)
			{
				continue;
			}
			for (auto const& sibling : processors)
			{
				if (sibling.group == 0 && sibling.coreIndex == processor.coreIndex
					&& sibling.index < sizeof(uintptr_t) * 8)
				{
					mask |= uintptr_t{ 1 } << sibling.index;
				}
			}
			break;
		}
		return mask;
	}

	Result Apply(Options const& options)
	{
		if (options.disablePowerThrottling)
		{
			DisablePowerThrottling();
		}

		DWORD_PTR processMask = 0;
		DWORD_PTR systemMask = 0;
		if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) || processMask == 0)
		{
			return { 1, 0, 0, {} };
		}

		Result result{ CountBits(processMask), 0, processMask, ReadProcessors() };
		result.performanceCoreMask = PerformanceCoreMask(result.processors, processMask);
		if (options.preferPerformanceCores)
		{
			result.logicalProcessors = PreferPerformanceCores(result.processors, processMask);
			if (result.logicalProcessors != CountBits(processMask))
			{
				result.allowedMask = result.performanceCoreMask;
			}
		}
		return result;
	}
}
