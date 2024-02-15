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

#ifndef VSTMIDIDRV_MIDISYNTH_H
#define VSTMIDIDRV_MIDISYNTH_H

class VSTDriver;

namespace VSTMIDIDRV {

	enum : unsigned int
	{
		FailedToOpen = 0,
		FailedToPrepare = 1,
		FailedToReset = 2,
		FailedToUnprepare = 3,
		FailedToClose = 4,
		FailedToWrite = 5,
		FailedToPause = 6,
		FailedToResume = 7,
		FailedToGetPosition = 8,
		FailedToGetSamples = 9,
	};

class MidiSynth {
private:
	unsigned int sampleRate;
	unsigned int midiLatency, midiLatencyMS;
	unsigned int bufferSize, bufferSizeMS;
	unsigned int chunkSize, chunkSizeMS;
	bool usingFloat;
	bool useAsio;
	float outputGain;
	float midiVol[2];
	
	unsigned char statusBuff[2]; //running status buffer
	bool isPortOff[2];
	bool isSinglePort32Ch;
	bool enableSinglePort32ChMode;
	unsigned int virtualPortNum;
	unsigned int lastOpenedPort;
	
	WORD channels;	

	short *buffer;
	float *bufferf;
	DWORD framesRendered;

	VSTDriver * vstDriver;

	unsigned int MillisToFrames(unsigned int millis);
	bool PreprocessMIDI(unsigned int& uDeviceID, DWORD& msg);
	bool PreprocessSysEx(unsigned int& uDeviceID, unsigned char* bufpos, const DWORD len);
	void LoadSettings();
	static void CALLBACK ResetSynthTimerProc(UINT uTimerID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2);

	MidiSynth();

public:	
		
	unsigned int getSampleRate() { return sampleRate; }
	unsigned int getBufferSizeMS() { return bufferSizeMS; }
	bool getUsingFloat() { return usingFloat; }
	WORD getChannels() { return channels; }

	static MidiSynth &getInstance();
	int Init(unsigned uDeviceID);
	void InitDialog(unsigned uDeviceID);
	void Close();
	int Reset(unsigned uDeviceID);	
	void Render(short *bufpos, DWORD totalFrames);
	void RenderFloat(float *bufpos, DWORD totalFrames);		
	void PushMIDI(unsigned uDeviceID, DWORD msg);
	void PlaySysEx(unsigned uDeviceID, unsigned char *bufpos, DWORD len);	
	void SetVolume(unsigned uDeviceID, float volume);
};

}

#endif
