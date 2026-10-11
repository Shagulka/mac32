// Prototypes of Carbon functions that still ship in macOS's libraries but are
// no longer declared in the SDK headers (32-bit only APIs, removed headers).
// Read by tools/gen_api.py carbon and included by src/carbon_thunks.c.
#pragma once
#include <CoreServices/CoreServices.h>

extern OSErr FSFindFolder(FSVolumeRefNum vRefNum, OSType folderType, Boolean createFolder, FSRef *foundRef);
