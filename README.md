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

#### Nullsoft Scriptable Install System (NSIS) V3.0a or higher
See https://sourceforge.net/projects/nsis/  
Additionally, the locked-list plug-in needs to be installed; you can get that from
https://nsis.sourceforge.io/LockedList_plug-in  
After having fetched the plug-in, copy its contents into the NSIS installation directory.

Then, from the NSIS installation directory, you have to copy the contents of the Examples\Plugin\nsis folder into this project's ummidiplg\nsis subdirectory. Since they may vary between NSIS installer versions, I didn't want to include them directly.

#### Windows Template Library 9.0 or 9.1
See https://sourceforge.net/projects/wtl/  
I've put it into the subdirectory wtl in this project; if you want to use it in an unmodified form,
copy the WTL contents to this directory. If you already got it installed somewhere else, you need to
modify the project settings of the drivercfg sub-project.

#### The BASS MIDI Library
Only necessary if you want to use a newer version; the current one (as of 2021-12-25) is already
available in the external_packages subdirectory. The latest version of BASS can always be found at the BASS website:  
	www.un4seen.com  
Both the bass library and the basswasapi library are needed.

## Known Problems

I am not sure whether this is really based on the latest kode54 version; there's an installer available on the Internet that seems to have been built in November 2018. Unfortunately, authoritative sources for that cannot be found.
