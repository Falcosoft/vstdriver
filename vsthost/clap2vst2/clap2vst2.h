#pragma once

#include <tchar.h>
#include "../../external_packages/aeffect.h"

AEffect *CreateClapVst2(audioMasterCallback audioMaster, const TCHAR *path);
