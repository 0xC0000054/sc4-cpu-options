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

namespace ParallelRenderCull
{

	enum class Mode
	{
		Off,      // do not install; stock code runs untouched
		Passthru, // install, but call the stock code (debugging aid)
		TailOnly, // skip the gather, run only the sort and draw (debugging aid)
		Serial,   // reimplemented gather, single threaded (debugging aid)
		Parallel, // gather on a worker pool (default)
	};

	// "off" / "0" / "false", "passthru", "tailonly", "serial"; anything else is Parallel.
	Mode ParseMode(const char* value);

	// The most workers the pool starts; the render thread gathers alongside them.
	constexpr unsigned kMaxWorkers = 7;

	// Installs inline hooks on cSC43DRender::DrawStaticView / DrawDynamicView that
	// move the quad-grid visibility gather + sort-key computation off the render
	// thread onto a small worker pool. Sorting and draw-call submission stay on the
	// render thread, byte-for-byte identical to the stock path.
	//
	// workers is how many pool threads to start (at most kMaxWorkers), and
	// workerAffinity the processors they may run on (0 for anywhere). The pool's
	// threads are the plugin's own, so they may run on cores the game's threads are
	// kept off. Parallel mode needs at least two workers; with fewer, nothing is
	// installed.
	//
	// Requires SimCity 4 Deluxe 1.1.641. Safe to call more than once; only the first
	// call installs. Returns true if the hooks are active afterwards.
	bool Install(unsigned workers, uintptr_t workerAffinity, Mode mode);

	// Removes the hooks and tears the worker pool down. Safe if Install never ran.
	void Uninstall(void);

}
