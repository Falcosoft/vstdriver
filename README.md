# vstdriver - MIDI synthesizer driver for VST Instruments

A Windows user-mode software MIDI synthesizer driver which is capable of using any 32 or 64 bit VST Instrument

Copyright (C) 2011 Chris Moeller, Brad Miller
... and maybe others; this seems to be a more or less abandoned project.
The above is the copyright notice I found in readme.txt.

Based on the Github project history:
Parts Copyright (c) 2013-2021 Christopher Snowhill (kode54)

And finally:
Parts Copyright (c) 2021 Hermann Seib

## Building

Currently, the following development environments are supported:
Visual Studio 2008, Visual Studio 2017-2022
In theory, VS2010-VS2015 should work, too, but I didn't investigate that.

### Additional requirements

To build it, you need the following software installed:

#### Nullsoft Installer System (NSIS) V3.0a or higher
See https://sourceforge.net/projects/nsis/
Additionally, the locked-list plug-in needs to be installed; you can get that from
https://nsis.sourceforge.io/LockedList_plug-in

#### Windows Template Library 9.0 or 9.1
See https://sourceforge.net/projects/wtl/
I've put it into the subdirectory wtl in this project; if you want to use it in an unmodified form,
copy the WTL contents to this directory. If you already got it installed somewhere else, you ned to
modify the project settings of the drivercfg sub-project.