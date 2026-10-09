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

// Build as a 32-bit console program with src/CpuScheduling.cpp and src/Logger.cpp; it returns 0
// when every check passes. Asserts are on in Debug builds only.

#include "CpuScheduling.h"

#include <cassert>

using CpuScheduling::LogicalProcessor;
using CpuScheduling::PerformanceCoreMask;
using CpuScheduling::SelectPerformanceCpuSets;

int main()
{
	// 2 P-cores with hyperthreading (class 1, physical cores 0 and 1) followed by 4 E-cores
	// (class 0, physical cores 2 to 5).
	std::vector<LogicalProcessor> const hybrid{
		{0x100, 0, 0, 1, 0}, {0x101, 0, 1, 1, 0}, {0x102, 0, 2, 1, 1}, {0x103, 0, 3, 1, 1},
		{0x104, 0, 4, 0, 2}, {0x105, 0, 5, 0, 3}, {0x106, 0, 6, 0, 4}, {0x107, 0, 7, 0, 5},
	};

	auto selected = SelectPerformanceCpuSets(hybrid, 0xFF);
	assert((selected == std::vector<uint32_t>{0x100, 0x101, 0x102, 0x103}));
	assert(PerformanceCoreMask(hybrid, 0xFF) == 0x0F);

	// Affinity narrows the candidates.
	selected = SelectPerformanceCpuSets(hybrid, 0x36);
	assert((selected == std::vector<uint32_t>{0x101, 0x102}));
	assert(PerformanceCoreMask(hybrid, 0x36) == 0x06);

	// Pinned to one P-core, or only E-cores allowed: nothing to narrow.
	assert(SelectPerformanceCpuSets(hybrid, 0x01).empty());
	assert(SelectPerformanceCpuSets(hybrid, 0xF0).empty());
	assert(PerformanceCoreMask(hybrid, 0xF0) == 0);

	// One P-core among E-cores is still the core to pin a single-core game to.
	assert(PerformanceCoreMask(hybrid, 0x11) == 0x01);

	// Homogeneous CPU.
	std::vector<LogicalProcessor> const uniform{{0x100, 0, 0, 0}, {0x101, 0, 1, 0}, {0x102, 0, 2, 0}};
	assert(SelectPerformanceCpuSets(uniform, 0x07).empty());
	assert(PerformanceCoreMask(uniform, 0x07) == 0);

	// Processors outside group 0 are unreachable for a 32-bit affinity mask.
	std::vector<LogicalProcessor> const grouped{
		{0x100, 0, 0, 0}, {0x101, 0, 1, 0}, {0x200, 1, 0, 1}, {0x201, 1, 1, 1},
	};
	assert(SelectPerformanceCpuSets(grouped, 0x03).empty());
	assert(PerformanceCoreMask(grouped, 0x03) == 0);

	assert(SelectPerformanceCpuSets({}, 0xFF).empty());

	// SMT siblings share a physical core; an E-core has none.
	assert(CpuScheduling::SameCoreMask(hybrid, 0) == 0x03);
	assert(CpuScheduling::SameCoreMask(hybrid, 3) == 0x0C);
	assert(CpuScheduling::SameCoreMask(hybrid, 5) == 0x20);
	// Without processor information, just the processor itself.
	assert(CpuScheduling::SameCoreMask({}, 4) == 0x10);

	// The parallel render cull's workers: the P-cores minus the game's physical core.
	assert((PerformanceCoreMask(hybrid, 0xFF) & ~CpuScheduling::SameCoreMask(hybrid, 0)) == 0x0C);

	// Calls the runtime-resolved Windows APIs; Debug /RTC1 aborts on a mismatched stdcall signature.
	assert(CpuScheduling::Apply({ true, true }).logicalProcessors >= 1);
	return 0;
}
