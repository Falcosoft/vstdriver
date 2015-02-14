/* Copyright (C) 2011, 2012 Sergey V. Mikayev
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "stdafx.h"
#include "VSTDriver.h"

#ifndef BASSMIDIDRV_MIDISYNTH_H
#define VSTMIDIDRV_MIDISYNTH_H

namespace VSTMIDIDRV {

class MidiSynth {
private:
	unsigned int sampleRate;
	unsigned int midiLatency;
	unsigned int bufferSize;
	unsigned int chunkSize;
	bool useRingBuffer;
	bool usingFloat;
	bool resetEnabled;
	float outputGain;
	VSTDriver * vst_driver;
	short *buffer;
	float *bufferf;
	DWORD framesRendered;

	//BOOL LoadBass();
    
   // bool LoadFontItem(unsigned uDeviceID, const TCHAR * in_path);
    
   /// void LoadFonts(unsigned uDeviceID, const TCHAR * name);
   // void FreeFonts(unsigned uDeviceID);

	unsigned int MillisToFrames(unsigned int millis);
	void LoadSettings();

	MidiSynth();

public:
	static MidiSynth &getInstance();
	int Init();
	void Close();
	int Reset(unsigned uDeviceID);
	void RenderAvailableSpace();
	void Render(short *bufpos, DWORD totalFrames);
	void RenderFloat(float *bufpos, DWORD totalFrames);
	void PushMIDI(unsigned uDeviceID, DWORD msg);
	void PlaySysex(unsigned uDeviceID, unsigned char *bufpos, DWORD len);
};

}
#endif
