/*
 *  This file is part of sc4-cpu-options. It comes from SCD3D11, a free Direct3D 11
 *  driver for SimCity 4's SimGL interface (https://github.com/UlisseWolf/scd3d11).
 *  Copyright (C) 2026 Casper Van Gheluwe
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <cstdint>
#include <vector>

namespace CpuScheduling
{
	struct LogicalProcessor
	{
		uint32_t cpuSetId;
		uint16_t group;
		uint8_t index;           // logical processor index within its group
		uint8_t efficiencyClass; // higher is faster (P-cores above E-cores)
		uint8_t coreIndex;       // the physical core within the group; SMT siblings share it
	};

	// CPU set ids of the fastest efficiency class within the affinity mask. Empty when there
	// is nothing to narrow: a homogeneous CPU, or fewer than two fast processors allowed.
	std::vector<uint32_t> SelectPerformanceCpuSets(
		std::vector<LogicalProcessor> const& processors,
		uintptr_t affinityMask);

	// The affinity bits of the fastest efficiency class within the affinity mask, or 0 on a
	// homogeneous CPU.
	uintptr_t PerformanceCoreMask(
		std::vector<LogicalProcessor> const& processors,
		uintptr_t affinityMask);

	// The affinity bits of every logical processor on the same physical core as the one at
	// index (in group 0), so SMT siblings come out together. Just that bit when the processor
	// list does not know it.
	uintptr_t SameCoreMask(
		std::vector<LogicalProcessor> const& processors,
		unsigned index);

	struct Options
	{
		bool preferPerformanceCores;
		bool disablePowerThrottling;
	};

	struct Result
	{
		// How many logical processors the process may run on afterwards.
		unsigned logicalProcessors;
		// The performance cores' affinity bits, or 0 on a homogeneous CPU.
		uintptr_t performanceCoreMask;
		// The affinity bits the process may run on afterwards: the performance cores when the
		// threads were narrowed to them, otherwise the process affinity mask.
		uintptr_t allowedMask;
		// What Windows reports about each logical processor; empty before Windows 10.
		std::vector<LogicalProcessor> processors;
	};

	// Keeps SimCity 4 off E-cores and out of Windows power throttling on hybrid CPUs: opts out
	// of EcoQoS, and makes the P-cores the default CPU sets for every thread.
	Result Apply(Options const& options);
}
