# vstdriver - MIDI synthesizer driver for VST Instruments

A Windows user-mode software MIDI synthesizer driver which is capable of using any 32 or 64 bit VST Instrument

Copyright (C) 2011 Chris Moeller, Brad Miller  
... and maybe others; this seems to be a more or less abandoned project.
The above is the copyright notice I found in readme.txt.

Based on the Github project history:  
Parts Copyright (c) 2013-2021 Christopher Snowhill (kode54)

And finally:  
Parts Copyright (c) 2021 Hermann Seib

After the above:
Parts Copyright (c) 2023 Zoltán Bacsko (Falcosoft) 

## Building

Currently, the following development environments are supported:  
Visual Studio 2008, Visual Studio 2017-2022 
In theory, VS2010-VS2015 should work, too, but I didn't investigate that.

Falcosoft: Visual studio 2005 also works with the vstdriver.08.sln solution file.
Visual Studio 2005 can be used to build WinNT4 SP6 compatible version of the driver.

### Additional requirements

To build it, you need the following software installed:

#### Nullsoft Scriptable Install System (NSIS) V3.0a or higher
See https://sourceforge.net/projects/nsis/  
Additionally, the locked-list plug-in needs to be installed; you can get that from
https://nsis.sourceforge.io/LockedList_plug-in  
After having fetched the plug-in, copy its contents into the NSIS installation directory.

Then, from the NSIS installation directory, you have to copy the contents of the Examples\Plugin\nsis folder into this project's ummidiplg\nsis subdirectory. Since they may vary between NSIS installer versions, I didn't want to include them directly.


## Falcosoft:
Windows Template Library 9.1 headers have been integrated to the project since they work with all versions of Visual Studio
and can be used to compile a driver that is compatible with all supported Windows versions (from WinNT4 SP6 to Win11). 

The version forked by Arakula (Hermann Seib) used the Bass/Basswasapi libraries to produce audio.
There were 2 serious problems with this approach:

1. VST Midi driver is an in-process driver. It cannot use its own Bass libraries if the Midi client already uses some other versions of Bass libraries.
https://www.vogons.org/viewtopic.php?p=596545#p596545

This is also true the other way around: If a Midi client uses VST Midi driver then it cannot use its own Bass libraries afterward since the libraries are already loaded to the client's address space through VST Midi driver. 
https://www.vogons.org/viewtopic.php?p=1066041#p1066041

Both situations can cause subtle and hard to debug problems.

2. Midi precision depends on Bass update periods that are 5-10 ms at best case. 
This Midi precision depends on Bass update periods problem has already been fixed by Ian for BassMidi (async mode) and by me in my Bass_VST version but unfortunately none of them applies to VST Midi drivers using Bass libraries. 
https://www.un4seen.com/forum/?topic=18485.msg129797#msg129797

https://www.un4seen.com/forum/?topic=19639.msg137463#msg137463

Interestingly the previous version that used traditional WaveOut also used timestamped Midi messages and waveOutGetPosition to achieve perfect Midi timing/precision.
So I restored the WaveOut version that cures both problems listed above. 
The only drawback would be higher latency but the fun fact is that because of missing proper configuration (BASS_CONFIG_BUFFER) and some path reelated bugs the Arakula version that used Bass/Basswasapi had a terrible 500ms latenccy.
So actually the WaveOut version has also much better latency now.
