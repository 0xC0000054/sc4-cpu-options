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

namespace SimTickBudget
{

	// The ceiling used when neither the configuration file nor the command line sets one.
	constexpr int kDefaultCapMs = 32;

	// cSC4Simulator::OnTick busy-loops the simulation for up to a speed-dependent
	// budget (33/50/66 ms) before returning to the frame loop, so a heavy sim
	// starves rendering and input. This clamps that per-tick budget to a fixed
	// ceiling: the sim still advances every frame, just less per frame, and the
	// frame loop presents in between. The sim clock may lag wall-clock a little
	// more under load, in exchange for a smooth frame rate.
	//
	// capMs is the ceiling in milliseconds, clamped to [15, 500]; 0 or less leaves
	// the game's budget alone.
	//
	// Requires SimCity 4 Deluxe 1.1.641. Safe to call twice; only the first
	// installs. Returns true if the patch is active afterwards.
	bool Install(int capMs);
	void Uninstall(void);

}
