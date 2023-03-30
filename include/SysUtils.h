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

#pragma once
#include <thread>
#include <string>
#include "MediaCore.h"

namespace SysUtils
{
MEDIACORE_API void SetThreadName(std::thread& t, const std::string& name);
MEDIACORE_API std::string ExtractFileBaseName(const std::string& path);
MEDIACORE_API std::string ExtractFileExtName(const std::string& path);
MEDIACORE_API std::string ExtractFileName(const std::string& path);
MEDIACORE_API std::string ExtractDirectoryPath(const std::string& path);
}