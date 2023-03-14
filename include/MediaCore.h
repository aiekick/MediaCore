#pragma once
#ifdef _WIN32
#if MEDIACORE_SHARED
#define MEDIACORE_API __declspec( dllexport )
#else
#define MEDIACORE_API
#endif
#else
#define MEDIACORE_API
#endif