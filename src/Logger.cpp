////////////////////////////////////////////////////////////////////////
//
// This file is part of sc4-cpu-options, a DLL Plugin for SimCity 4
// that configures the CPU core count and priority.
//
// Copyright (c) 2024 Nicholas Hayes
//
// This file is licensed under terms of the MIT License.
// See LICENSE.txt for more information.
//
////////////////////////////////////////////////////////////////////////

#include "Logger.h"
#include <Windows.h>

namespace
{
	std::string GetTimeStamp()
	{
		char buffer[1024]{};

		GetTimeFormatA(
			LOCALE_USER_DEFAULT,
			0,
			nullptr,
			nullptr,
			buffer,
			_countof(buffer));

		std::string time(buffer);

		// Add a space to the end of the time string if it doe not have one.
		if (time.size() > 0 && time[time.size() - 1] != ' ')
		{
			time.append(1, ' ');
		}

		return time;
	}

#ifdef _DEBUG
	void PrintLineToDebugOutput(const char* timeStamp, const char* line)
	{
		OutputDebugStringA(timeStamp);
		OutputDebugStringA(line);
		OutputDebugStringA("\n");
	}
#endif // _DEBUG
}

Logger& Logger::GetInstance()
{
	static Logger logger;

	return logger;
}

Logger::Logger() : initialized(false), logFile(), logLevel(LogLevel::Error)
{
}

Logger::~Logger()
{
	initialized = false;
}

void Logger::Init(std::filesystem::path logFilePath, LogLevel options)
{
	if (!initialized)
	{
		initialized = true;

		logFile.open(logFilePath, std::ofstream::out | std::ofstream::trunc);
		logLevel = options;
	}
}

bool Logger::IsEnabled(LogLevel level) const
{
	return logLevel >= level;
}

void Logger::WriteLogFileHeader(const char* const text)
{
	if (initialized && logFile)
	{
		logFile << text << std::endl;
	}
}

void Logger::WriteLine(LogLevel level, const char* const message)
{
	if (!IsEnabled(level))
	{
		return;
	}

	WriteLineCore(message);
}

void Logger::WriteLineFormatted(LogLevel level, const char* const format, ...)
{
	if (!IsEnabled(level))
	{
		return;
	}

	va_list args;
	va_start(args, format);

	va_list argsCopy;
	va_copy(argsCopy, args);

	int formattedStringLength = std::vsnprintf(nullptr, 0, format, argsCopy);

	va_end(argsCopy);

	if (formattedStringLength > 0)
	{
		size_t formattedStringLengthWithNull = static_cast<size_t>(formattedStringLength) + 1;

		std::unique_ptr<char[]> buffer = std::make_unique_for_overwrite<char[]>(formattedStringLengthWithNull);

		std::vsnprintf(buffer.get(), formattedStringLengthWithNull, format, args);

		WriteLineCore(buffer.get());
	}

	va_end(args);
}

void Logger::WriteLineCore(const char* const message)
{
	if (initialized && logFile)
	{
		std::string timeStamp = GetTimeStamp();

#ifdef _DEBUG
		PrintLineToDebugOutput(timeStamp.c_str(), message);
#endif // _DEBUG

		logFile << timeStamp << message << std::endl;
	}
}