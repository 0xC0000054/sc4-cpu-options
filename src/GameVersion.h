////////////////////////////////////////////////////////////////////////
//
// This file is part of sc4-cpu-options, a DLL Plugin for SimCity 4
// that configures the CPU core count and priority.
//
// This file is licensed under terms of the MIT License.
// See LICENSE.txt for more information.
//
////////////////////////////////////////////////////////////////////////

#pragma once

namespace GameVersion
{
	// True when the running SimCity 4.exe is version 1.1.641, the only build whose code
	// addresses the ParallelRenderCull and SimTickBudget patches know.
	bool IsSupportedForPatches();
}
