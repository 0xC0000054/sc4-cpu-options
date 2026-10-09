////////////////////////////////////////////////////////////////////////
//
// This file is part of sc4-cpu-options, a DLL Plugin for SimCity 4
// that configures the CPU core count and priority.
//
// This file is licensed under terms of the MIT License.
// See LICENSE.txt for more information.
//
////////////////////////////////////////////////////////////////////////

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
