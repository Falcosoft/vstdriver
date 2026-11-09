#pragma once

#include "legacy_clap.h"
#include "legacy_win32.h"

#include "clap2vst2.h"
#include "../../external_packages/aeffectx.h"

#include <windows.h>
#include <string>
#include <vector>

class ClapAsVst2;

class Vst2MemoryReader
{
public:
    explicit Vst2MemoryReader(const void *data, size_t size)
        : _data((const uint8_t *)data), _size(size), _pos(0) {}
    static int64_t read(const clap_istream_t *stream, void *buffer, uint64_t size);
private:
    const uint8_t *_data;
    size_t _size;
    size_t _pos;
};

class Vst2MemoryWriter
{
public:
    explicit Vst2MemoryWriter(std::vector<uint8_t> &data) : _data(data) {}
    static int64_t write(const clap_ostream_t *stream, const void *buffer, uint64_t size);
private:
    std::vector<uint8_t> &_data;
};

class ClapAsVst2
{
public:
    explicit ClapAsVst2(audioMasterCallback master);
    ~ClapAsVst2();

    bool open(const TCHAR *clapPath);
    AEffect *effect() { return &_effect; }

    VstIntPtr dispatcher(VstInt32 opcode, VstInt32 index, VstIntPtr value, void *ptr, float opt);
    void processReplacing(float **inputs, float **outputs, VstInt32 sampleFrames);
    void setParameter(VstInt32 index, float value);
    float getParameter(VstInt32 index);

    void editOpen(void *parent);
    void editClose();
    bool editGetRect(ERect **rect);
    void editIdle();
    void syncClapParameters();

    // Wrapper-side targets for the small set of CLAP host extensions exposed
    // by this legacy host layer.
    void mark_dirty();
    void restartPlugin();
    void request_callback();
    void param_rescan(clap_param_rescan_flags flags);
    void param_clear(clap_id param, clap_param_clear_flags flags);
    void param_request_flush();
    void latency_changed();
    void tail_changed();
    bool gui_can_resize();
    bool gui_request_resize(uint32_t width, uint32_t height);
    bool gui_request_show();
    bool gui_request_hide();

    const clap_plugin_t *clap() const { return _plugin ? _plugin->_plugin : NULL; }
    const clap_plugin_audio_ports_t *audioPorts() const { return _plugin ? _plugin->_ext._audioports : NULL; }
    const clap_plugin_note_ports_t *notePorts() const { return _plugin ? _plugin->_ext._noteports : NULL; }
    const clap_plugin_params_t *params() const { return _plugin ? _plugin->_ext._params : NULL; }

    void processEvents(const VstEvents *events);
    bool selectProgram(int32_t program);
    std::string getProgramDisplayName(size_t index) const;

private:
    struct Param
    {
        Param() : id(0), flags(0), minValue(0.0), maxValue(1.0), def(0.0) {}
        clap_id id;
        uint32_t flags;
        double minValue;
        double maxValue;
        double def;
        std::string name;
        std::string module;
        std::string display;
    };

    struct PendingMidi
    {
        PendingMidi() : delta(0) { data[0] = data[1] = data[2] = 0; }
        int32_t delta;
        uint8_t data[3];
    };

    struct PendingSysEx
    {
        PendingSysEx() : delta(0) {}
        int32_t delta;
        std::vector<uint8_t> data;
    };

    struct OutputEvent
    {
        OutputEvent() : time(0), kind(0), param_id(0), param_value(0.0)
        {
            data[0] = data[1] = data[2] = 0;
        }
        int32_t time;
        uint8_t kind; // 1 MIDI, 2 SysEx, 3 parameter
        uint8_t data[3];
        clap_id param_id;
        double param_value;
        std::vector<uint8_t> sysex;
    };

    static VstIntPtr VSTCALLBACK dispatcherProc(AEffect *effect, VstInt32 opcode, VstInt32 index,
                                                 VstIntPtr value, void *ptr, float opt);
    static void VSTCALLBACK processReplacingProc(AEffect *effect, float **inputs, float **outputs,
                                                  VstInt32 sampleFrames);
    static void VSTCALLBACK setParameterProc(AEffect *effect, VstInt32 index, float value);
    static float VSTCALLBACK getParameterProc(AEffect *effect, VstInt32 index);

    // clap_host_t callbacks
    static const void *CLAP_ABI hostGetExtension(const clap_host_t *host, const char *extensionId);
    static void CLAP_ABI hostRequestRestart(const clap_host_t *host);
    static void CLAP_ABI hostRequestProcess(const clap_host_t *host);
    static void CLAP_ABI hostRequestCallback(const clap_host_t *host);

    // CLAP_EXT_PARAMS host callbacks
    static void CLAP_ABI hostParamsRescan(const clap_host_t *host, clap_param_rescan_flags flags);
    static void CLAP_ABI hostParamsClear(const clap_host_t *host, clap_id paramId,
                                         clap_param_clear_flags flags);
    static void CLAP_ABI hostParamsRequestFlush(const clap_host_t *host);

    // CLAP_EXT_STATE host callback
    static void CLAP_ABI hostStateMarkDirty(const clap_host_t *host);

    // CLAP_EXT_GUI host callbacks
    static void CLAP_ABI hostGuiResizeHintsChanged(const clap_host_t *host);
    static bool CLAP_ABI hostGuiRequestResize(const clap_host_t *host, uint32_t width, uint32_t height);
    static bool CLAP_ABI hostGuiRequestShow(const clap_host_t *host);
    static bool CLAP_ABI hostGuiRequestHide(const clap_host_t *host);
    static void CLAP_ABI hostGuiClosed(const clap_host_t *host, bool wasDestroyed);

    // CLAP_EXT_LATENCY / CLAP_EXT_TAIL host callbacks
    static void CLAP_ABI hostLatencyChanged(const clap_host_t *host);
    static void CLAP_ABI hostTailChanged(const clap_host_t *host);

    static int64_t CLAP_ABI stateRead(const clap_istream_t *stream, void *buffer, uint64_t size);
    static int64_t CLAP_ABI stateWrite(const clap_ostream_t *stream, const void *buffer, uint64_t size);

    static uint32_t CLAP_ABI inputEventsSize(const clap_input_events *list);
    static const clap_event_header_t *CLAP_ABI inputEventsGet(const clap_input_events *list,
                                                               uint32_t index);
    static bool CLAP_ABI outputEventsTryPush(const clap_output_events *list,
                                              const clap_event_header_t *event);

    bool loadLibrary(const TCHAR *clapPath);
    void initializeHost();
    void serviceMainThreadCallback();
    void populateEffectStruct();
    void queryPluginInfo();
    void updateFlags();
    void buildAudioLayout();
    void buildParameters();
    void buildNoteDialect();
    bool activatePlugin();
    void convertVstEventsToClap(uint32_t frames);
    void sendClapProcess(uint32_t frames, float **inputs, float **outputs);
    void collectOutputEvent(const clap_event_header_t *event);
    void buildTransport(uint32_t frames);
    void sendOutputEventsToHost();

    static VstInt32 stableId32(const char *s);
    static int64_t roundToInt64(double v);
    static bool isNan(double v) { return v != v; }
    bool isInstrumentLike(const clap_plugin_audio_ports_t *audioports,
                          const clap_plugin_note_ports_t *noteports,
                          const clap_plugin_t *plugin) const;

    audioMasterCallback _master;
    AEffect _effect;
    ERect _rect;

    Clap::Library *_library;
    Clap::Plugin *_plugin;
    // Win32Lock is forced to its CRITICAL_SECTION path because wrapper/CLAP
    // callbacks can re-enter while the owning thread already holds this lock.
    mutable Win32Lock _pluginMutex;
    const DWORD _mainThreadId;
    volatile LONG _mainCallbackPending;
    bool _servicingMainCallback;

    clap_host_t _clapHost;
    clap_host_params_t _hostParams;
    clap_host_state_t _hostState;
    clap_host_gui_t _hostGui;
    clap_host_latency_t _hostLatency;
    clap_host_tail_t _hostTail;

    std::string _name;
    std::string _vendor;
    std::string _version;
    int32_t _currentProgram;

    std::vector<Param> _params;
    // All accesses are serialized by _pluginMutex; a C++11 atomic array is not
    // required in this legacy build.
    std::vector<double> _pendingParams;
    std::vector<double> _lastSentParams;

    uint32_t _inputChannels;
    uint32_t _outputChannels;
    bool _usingClapNotes;
    uint16_t _notePortIndex;
    uint32_t _noteDialect;

    std::vector<clap_audio_buffer_t> _inPorts;
    std::vector<clap_audio_buffer_t> _outPorts;
    std::vector<float *> _inPtrs;
    std::vector<float *> _outPtrs;
    std::vector<clap_event_midi_t> _midiEvents;
    std::vector<clap_event_note_t> _noteEvents;
    std::vector<clap_event_midi_sysex_t> _sysexEvents;
    std::vector<std::vector<uint8_t> > _sysexStorage;
    std::vector<clap_event_header_t *> _eventPtrs;
    std::vector<OutputEvent> _clapOutputEvents;
    clap_event_transport_t _transport;

    // Events received through VST2 effProcessEvents are kept until the next
    // CLAP process() call. The processing vectors are swapped with the pending
    // vectors at the start of conversion, preserving the proven two-buffer design.
    std::vector<PendingMidi> _pendingMidi;
    std::vector<PendingSysEx> _pendingSysEx;
    std::vector<PendingMidi> _processingMidi;
    std::vector<PendingSysEx> _processingSysEx;

    std::vector<uint8_t> _state;
    std::vector<uint8_t> _stateScratch;
    bool _opened;
    bool _active;
    bool _processing;
    HWND _editorParent;
    bool _editorOpen;

    clap_input_events_t _inputEvents;
    clap_output_events_t _clapOutEvents;
    clap_process_t _process;

    float _lastSampleRate;
    uint32_t _configuredBlockSize;
    uint32_t _activeMaxFrames;
    double _transportTempo;
    bool _dirty;
};

inline ClapAsVst2 *asVst2(AEffect *effect)
{
    return static_cast<ClapAsVst2 *>(effect->object);
}
