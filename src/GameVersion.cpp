/*
 * This file is part of sc4-cpu-options, a DLL Plugin for SimCity 4
 * that configures the CPU core count and priority.
 *
 * Copyright (C) 2024, 2025, 2026 Nicholas Hayes
 *
 * sc4-cpu-options is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * sc4-cpu-options is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with sc4-cpu-options.
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include "GameVersion.h"

#include <cstdint>
#include <vector>

#include <Windows.h>

bool GameVersion::IsSupportedForPatches()
{
	static int cached = -1;

	if (cached < 0)
	{
		cached = 0;

		wchar_t path[MAX_PATH]{};
		if (GetModuleFileNameW(nullptr, path, MAX_PATH) != 0)
		{
			DWORD ignored = 0;
			DWORD const size = GetFileVersionInfoSizeW(path, &ignored);
			if (size != 0)
			{
				std::vector<uint8_t> data(size);
				VS_FIXEDFILEINFO* info = nullptr;
				UINT infoSize = 0;

				if (GetFileVersionInfoW(path, 0, size, data.data())
					&& VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize)
					&& info != nullptr
					&& infoSize >= sizeof(VS_FIXEDFILEINFO))
				{
					cached = HIWORD(info->dwFileVersionMS) == 1
						&& LOWORD(info->dwFileVersionMS) == 1
						&& HIWORD(info->dwFileVersionLS) == 641;
				}
			}
		}
	}

	return cached != 0;
}
