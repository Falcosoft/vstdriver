// Shared commands and short-MIDI batch bound for the driver and bridge.
#ifndef VSTDRIVER_MIDI_PROTOCOL_H
#define VSTDRIVER_MIDI_PROTOCOL_H

namespace Command {
    enum {
        Exit = 0,
        GetChunkData = 1,
        SetChunkData = 2,
        HasEditor = 3, // Reserved: the editor-query command is currently unused.
        DisplayEditorModal = 4,
        SetSampleRate = 5,
        Reset = 6,
        SendMidiEvent = 7,
        SendMidiSysExEvent = 8,
        RenderAudioSamples = 9,
        DisplayEditorModalThreaded = 10,
        RenderAudioSamples4channel = 11,
        SetHighDpiMode = 12,
        SetSinglePort32ChMode = 13,
        InitSysTray = 14,
        SendMidiEvents = 15
    };
}

namespace VSTDriverMidiProtocol {
    enum {
        MaxBatchEvents = 128
    };
}

#endif
