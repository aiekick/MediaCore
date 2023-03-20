#include "MediaCore.h"
void MediaCore::GetVersion(int& major, int& minor, int& patch, int& build)
{
    major = MEDIACORE_VERSION_MAJOR;
    minor = MEDIACORE_VERSION_MINOR;
    patch = MEDIACORE_VERSION_PATCH;
    build = MEDIACORE_VERSION_BUILD;
}