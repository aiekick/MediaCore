/*
    Copyright (c) 2023 CodeWin

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "SysUtils.h"
#if defined(__linux__)
#include "pthread.h"
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace SysUtils
{
void SetThreadName(std::thread& t, const std::string& name)
{
#if defined(__linux__)
    auto handle = t.native_handle();
    if (name.length() > 15)
    {
        std::string shortName = name.substr(0, 15);
        pthread_setname_np(handle, shortName.c_str());
    }
    else
    {
        pthread_setname_np(handle, name.c_str());
    }
#elif defined(_WIN32)
    DWORD threadId = ::GetThreadId(static_cast<HANDLE>(t.native_handle()));
    SetThreadName(threadId, name.c_str());
#endif
}

#if defined(_WIN32)
static const char _PATH_SEPARATOR = '\\';
#else
static const char _PATH_SEPARATOR = '/';
#endif
static const char _FILE_EXT_SEPARATOR = '.';

std::string ExtractFileBaseName(const std::string& path)
{
    auto lastSlashPos = path.rfind(_PATH_SEPARATOR);
    auto lastDotPos = path.rfind(_FILE_EXT_SEPARATOR);
    if (lastSlashPos == path.length()-1)
    {
        return "";
    }
    else if (lastSlashPos == std::string::npos)
    {
        if (lastDotPos == std::string::npos || lastDotPos == 0)
            return path;
        else
            return path.substr(0, lastDotPos);
    }
    else
    {
        if (lastDotPos == std::string::npos || lastDotPos <= lastSlashPos+1)
            return path.substr(lastSlashPos+1);
        else
            return path.substr(lastSlashPos+1, lastDotPos-lastSlashPos-1);
    }
}

std::string ExtractFileExtName(const std::string& path)
{
    auto lastSlashPos = path.rfind(_PATH_SEPARATOR);
    auto lastDotPos = path.rfind(_FILE_EXT_SEPARATOR);
    if (lastSlashPos == path.length()-1)
    {
        return "";
    }
    else if (lastSlashPos == std::string::npos)
    {
        if (lastDotPos == std::string::npos || lastDotPos == 0)
            return "";
        else
            return path.substr(lastDotPos);
    }
    else
    {
        if (lastDotPos == std::string::npos || lastDotPos <= lastSlashPos+1)
            return "";
        else
            return path.substr(lastDotPos);
    }
}

std::string ExtractFileName(const std::string& path)
{
    auto lastSlashPos = path.rfind(_PATH_SEPARATOR);
    if (lastSlashPos == path.length()-1)
    {
        return "";
    }
    else if (lastSlashPos == std::string::npos)
    {
        return path;
    }
    else
    {
        return path.substr(lastSlashPos+1);
    }
}

std::string ExtractDirectoryPath(const std::string& path)
{
    auto lastSlashPos = path.rfind(_PATH_SEPARATOR);
    if (lastSlashPos == path.length()-1)
    {
        return path;
    }
    else if (lastSlashPos == std::string::npos)
    {
        return "";
    }
    else
    {
        return path.substr(0, lastSlashPos+1);
    }
}
}