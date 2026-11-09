#include "wrapasvst2.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using namespace Clap;

namespace {
static const char *kHostName = "CLAP-to-VST2 Wrapper";
static const clap_event_header_t *const kNoEvent = NULL;

static int32_t clamp_i32(int64_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return (int32_t)v;
}

struct InputEventSlice
{
    clap_event_header_t *const *events;
    uint32_t count;
};

static bool eventTimeLess(const clap_event_header_t *a, const clap_event_header_t *b)
{
    return a->time < b->time;
}

// CLAP strings are UTF-8, while VST2 exposes its legacy text fields as
// ordinary char buffers. Convert only at the VST2 boundary, keeping UTF-8
// internally so CLAP-specific string handling is not affected.
static std::string utf8ToAnsi(const char *text)
{
    if (!text || !*text) return std::string();

    int wideLen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      text, -1, NULL, 0);
    if (wideLen <= 0) return std::string(text);

    std::vector<wchar_t> wide((size_t)wideLen);
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                             text, -1, &wide[0], wideLen))
        return std::string(text);

    int ansiLen = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS,
                                      &wide[0], -1, NULL, 0,
                                      "?", NULL);
    if (ansiLen <= 0) return std::string(text);

    std::vector<char> ansi((size_t)ansiLen);
    BOOL usedDefault = FALSE;
    if (!WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS,
                             &wide[0], -1, &ansi[0], ansiLen,
                             "?", &usedDefault))
        return std::string(text);

    return std::string(&ansi[0]);
}

static std::string utf8ToAnsi(const std::string &text)
{
    return utf8ToAnsi(text.c_str());
}

enum DescriptorRole
{
    DescriptorRoleUnknown = 0,
    DescriptorRoleInstrument,
    DescriptorRoleEffect
};

static bool descriptorHasFeature(const clap_plugin_descriptor_t *descriptor, const char *feature)
{
    if (!descriptor || !descriptor->features || !feature) return false;

    for (size_t i = 0; descriptor->features[i] != NULL; ++i)
    {
        if (!std::strcmp(descriptor->features[i], feature)) return true;
    }
    return false;
}

static DescriptorRole descriptorRole(const clap_plugin_descriptor_t *descriptor)
{
    const bool isInstrument = descriptorHasFeature(descriptor, CLAP_PLUGIN_FEATURE_INSTRUMENT);
    const bool isEffect = descriptorHasFeature(descriptor, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT);

    if (isInstrument && !isEffect) return DescriptorRoleInstrument;
    if (isEffect && !isInstrument) return DescriptorRoleEffect;
    return DescriptorRoleUnknown;
}

static bool isBassVstHost(audioMasterCallback master)
{
    if (!master) return false;

    char vendor[65];
    char product[65];
    std::memset(vendor, 0, sizeof(vendor));
    std::memset(product, 0, sizeof(product));

    master(NULL, audioMasterGetVendorString, 0, 0, vendor, 0.0f);
    master(NULL, audioMasterGetProductString, 0, 0, product, 0.0f);

    // Falcosoft MIDI Player overrides BASS_VST's host strings this way:
    //   vendor  = "Falcosoft"
    //   product = "BassVST - " + application caption
    if (!std::strcmp(vendor, "Falcosoft") &&
        !std::strncmp(product, "BassVST - ", 10))
        return true;

    // Stock BASS_VST defaults.
    if (!std::strcmp(vendor, "Bjoern Petersen Software Design and Development") &&
        !std::strcmp(product, "BASS_VST @ Silverjuke.Net"))
        return true;

    return false;
}

static DescriptorRole requestedMixedRole(audioMasterCallback master)
{
    // Unknown hosts deliberately keep the historical instrument behavior.
    // This also keeps VST MIDI Driver compatible, since it is instrument-only.
    if (!isBassVstHost(master)) return DescriptorRoleInstrument;

    // BASS_VST exposes its in-construction handle through audioMasterCurrentId.
    // VST instruments created by BASS_VST_ChannelCreate() use BASS stream
    // handles, whose bit 31 is set. Effects created by BASS_VST_ChannelSetDSP()
    // use BASS_VST's positive internal handle range. Test the low 32 bits so
    // this works identically in Win32 and x64 builds.
    const VstIntPtr currentId = master(NULL, audioMasterCurrentId, 0, 0, NULL, 0.0f);
    const uint32_t bassId = (uint32_t)currentId;

    if (bassId != 0 && (bassId & 0x80000000u) == 0)
        return DescriptorRoleEffect;

    return DescriptorRoleInstrument;
}

static void filterMixedRoleDescriptors(Clap::Library *library, audioMasterCallback master)
{
    if (!library || library->plugins.empty()) return;

    bool hasInstrument = false;
    bool hasEffect = false;

    for (size_t i = 0; i < library->plugins.size(); ++i)
    {
        const DescriptorRole role = descriptorRole(library->plugins[i]);
        if (role == DescriptorRoleInstrument) hasInstrument = true;
        else if (role == DescriptorRoleEffect) hasEffect = true;
    }

    // Preserve the proven multi-descriptor-as-program behavior unless the CLAP
    // explicitly contains both fundamental personalities.
    if (!hasInstrument || !hasEffect) return;

    const DescriptorRole wanted = requestedMixedRole(master);
    std::vector<const clap_plugin_descriptor_t *> filtered;
    filtered.reserve(library->plugins.size());

    for (size_t i = 0; i < library->plugins.size(); ++i)
    {
        if (descriptorRole(library->plugins[i]) == wanted)
            filtered.push_back(library->plugins[i]);
    }

    // Both roles were positively detected above, so this should not be empty.
    // Retain the original list if a malformed descriptor set somehow defeats
    // the classification, rather than making the wrapper unloadable.
    if (!filtered.empty()) library->plugins.swap(filtered);
}
}

VstIntPtr VSTCALLBACK ClapAsVst2::dispatcherProc(AEffect *effect, VstInt32 opcode, VstInt32 index,
                                                  VstIntPtr value, void *ptr, float opt)
{
    return asVst2(effect)->dispatcher(opcode, index, value, ptr, opt);
}

void VSTCALLBACK ClapAsVst2::processReplacingProc(AEffect *effect, float **inputs, float **outputs,
                                                   VstInt32 sampleFrames)
{
    asVst2(effect)->processReplacing(inputs, outputs, sampleFrames);
}

void VSTCALLBACK ClapAsVst2::setParameterProc(AEffect *effect, VstInt32 index, float value)
{
    asVst2(effect)->setParameter(index, value);
}

float VSTCALLBACK ClapAsVst2::getParameterProc(AEffect *effect, VstInt32 index)
{
    return asVst2(effect)->getParameter(index);
}

ClapAsVst2::ClapAsVst2(audioMasterCallback master)
    : _master(master),
      _library(NULL),
      _plugin(NULL),
      _pluginMutex(true),
      _mainThreadId(GetCurrentThreadId()),
      _mainCallbackPending(0),
      _servicingMainCallback(false),
      _currentProgram(0),
      _inputChannels(0),
      _outputChannels(0),
      _usingClapNotes(false),
      _notePortIndex(0),
      _noteDialect(0),
      _opened(false),
      _active(false),
      _processing(false),
      _editorParent(NULL),
      _editorOpen(false),
      _lastSampleRate(44100.0f),
      _configuredBlockSize(512),
      _activeMaxFrames(0),
      _transportTempo(120.0),
      _dirty(false)
{
    memset(&_effect, 0, sizeof(_effect));
    memset(&_rect, 0, sizeof(_rect));
    _rect.right = 480;
    _rect.bottom = 320;
    memset(&_transport, 0, sizeof(_transport));
    memset(&_inputEvents, 0, sizeof(_inputEvents));
    memset(&_clapOutEvents, 0, sizeof(_clapOutEvents));
    memset(&_process, 0, sizeof(_process));
    initializeHost();
}

void ClapAsVst2::initializeHost()
{
    memset(&_clapHost, 0, sizeof(_clapHost));
    _clapHost.clap_version = CLAP_VERSION;
    _clapHost.host_data = this;
    _clapHost.name = kHostName;
    _clapHost.vendor = "Falcosoft";
    _clapHost.url = "https://falcosoft.hu";
    _clapHost.version = "0.1";
    _clapHost.get_extension = hostGetExtension;
    _clapHost.request_restart = hostRequestRestart;
    _clapHost.request_process = hostRequestProcess;
    _clapHost.request_callback = hostRequestCallback;

    memset(&_hostParams, 0, sizeof(_hostParams));
    _hostParams.rescan = hostParamsRescan;
    _hostParams.clear = hostParamsClear;
    _hostParams.request_flush = hostParamsRequestFlush;

    memset(&_hostState, 0, sizeof(_hostState));
    _hostState.mark_dirty = hostStateMarkDirty;

    memset(&_hostGui, 0, sizeof(_hostGui));
    _hostGui.resize_hints_changed = hostGuiResizeHintsChanged;
    _hostGui.request_resize = hostGuiRequestResize;
    _hostGui.request_show = hostGuiRequestShow;
    _hostGui.request_hide = hostGuiRequestHide;
    _hostGui.closed = hostGuiClosed;

    memset(&_hostLatency, 0, sizeof(_hostLatency));
    _hostLatency.changed = hostLatencyChanged;

    memset(&_hostTail, 0, sizeof(_hostTail));
    _hostTail.changed = hostTailChanged;
}

ClapAsVst2::~ClapAsVst2()
{
    ScopeLock<Win32Lock> lock(&_pluginMutex);
    editClose();
    if (_plugin)
    {
        if (_processing)
        {
            _plugin->stop_processing();
            _processing = false;
        }
        if (_active)
        {
            _plugin->deactivate();
            _active = false;
        }
        delete _plugin;
        _plugin = NULL;
    }
    delete _library;
    _library = NULL;
}

bool ClapAsVst2::loadLibrary(const TCHAR *clapPath)
{
    delete _library;
    _library = new Clap::Library();
    return _library->load(clapPath);
}

AEffect *CreateClapVst2(audioMasterCallback audioMaster, const TCHAR *path)
{
    if (!audioMaster || !path || !*path) return NULL;

    ClapAsVst2 *instance = new ClapAsVst2(audioMaster);
    if (!instance->open(path))
    {
        delete instance;
        return NULL;
    }
    return instance->effect();
}

bool ClapAsVst2::open(const TCHAR *clapPath)
{
    if (_opened) return true;
    if (!_master) return false;
    if (!_master(NULL, audioMasterVersion, 0, 0, NULL, 0.0f))
    {
        // VST2 hosts occasionally return zero here while still being usable.
    }
    if (!loadLibrary(clapPath) || _library->plugins.empty() || !_library->_pluginFactory) return false;

    filterMixedRoleDescriptors(_library, _master);
    if (_library->plugins.empty()) return false;

    _plugin = Clap::Plugin::createInstance(*_library, 0, &_clapHost);
    if (!_plugin) return false;
    if (!_plugin->initialize())
    {
        delete _plugin;
        _plugin = NULL;
        return false;
    }

    queryPluginInfo();
    buildAudioLayout();
    buildParameters();
    buildNoteDialect();
    updateFlags();

    _midiEvents.reserve(256);
    _noteEvents.reserve(256);
    _sysexEvents.reserve(64);
    _sysexStorage.reserve(64);
    _eventPtrs.reserve(1024);
    _clapOutputEvents.reserve(256);
    _pendingMidi.reserve(256);
    _pendingSysEx.reserve(32);
    populateEffectStruct();

    _opened = true;
    return true;
}

std::string ClapAsVst2::getProgramDisplayName(size_t index) const
{
    if (!_library || index >= _library->plugins.size()) return "Program";

    const char *name = _library->plugins[index] ? _library->plugins[index]->name : NULL;
    if (!name || !*name) return "Program";

    // VST2 program names are limited to kVstMaxProgNameLen characters.
    // For a multi-descriptor CLAP factory, remove the longest useful textual
    // prefix shared by every descriptor so the distinguishing suffix remains
    // visible. This is intentionally generic and is not tied to Nuked-SC55.
    std::string original(name);
    std::string prefix(original);

    for (size_t i = 0; i < _library->plugins.size(); ++i)
    {
        const char *other = _library->plugins[i] ? _library->plugins[i]->name : NULL;
        if (!other)
        {
            prefix.clear();
            break;
        }

        size_t n = 0;
        const size_t otherLen = std::strlen(other);
        const size_t maxLen = prefix.size() < otherLen ? prefix.size() : otherLen;
        while (n < maxLen && prefix[n] == other[n]) ++n;
        prefix.resize(n);
        if (prefix.empty()) break;
    }

    // Keep only a whole textual word. If the common prefix itself ends with
    // whitespace, it is already at a natural boundary.
    size_t cut = prefix.size();
    while (cut > 0 && prefix[cut - 1] != ' ' && prefix[cut - 1] != '\t') --cut;

    if (cut > 0 && cut < original.size())
    {
        std::string result = original.substr(cut);

        // Remove separators left between the common prefix and the unique
        // part. Handle both ASCII separators and a UTF-8 em dash.
        for (;;)
        {
            if (!result.empty() &&
                (result[0] == ' ' || result[0] == '\t' || result[0] == '-' || result[0] == ':'))
            {
                result.erase(0, 1);
                continue;
            }
            if (result.size() >= 3 &&
                (unsigned char)result[0] == 0xE2 &&
                (unsigned char)result[1] == 0x80 &&
                (unsigned char)result[2] == 0x94)
            {
                result.erase(0, 3);
                continue;
            }
            break;
        }

        if (!result.empty()) return result;
    }

    return original;
}

void ClapAsVst2::queryPluginInfo()
{
    const clap_plugin_descriptor_t *d = _library->plugins[0];
    if (d) {
        _name = d->name ? d->name : "CLAP Plugin";
        _vendor = d->vendor ? d->vendor : "";
        _version = d->version ? d->version : "";
    }
}

bool ClapAsVst2::selectProgram(int32_t program)
{
    ScopeLock<Win32Lock> lock(&_pluginMutex);
    if (!_library || program < 0 || (size_t)program >= _library->plugins.size()) return false;
    if (program == _currentProgram) return true;

    const bool wasProcessing = _processing;
    const bool wasActive = _active;

    if (wasProcessing && _plugin) {
        _plugin->stop_processing();
        _processing = false;
    }
    if (wasActive && _plugin) {
        _plugin->deactivate();
        _active = false;
    }
    if (_plugin) {
        delete _plugin;
        _plugin = NULL;
    }

    // Requests from the previous descriptor must not reach the new instance.
    InterlockedExchange(&_mainCallbackPending, 0);
    _plugin = Clap::Plugin::createInstance(*_library, (size_t)program, &_clapHost);
    if (!_plugin || !_plugin->initialize()) {
        delete _plugin;
        _plugin = NULL;
        return false;
    }

    _plugin->setSampleRate(_lastSampleRate);
    queryPluginInfo();
    updateFlags();
    buildAudioLayout();
    buildParameters();
    buildNoteDialect();
    _currentProgram = program;

    if (wasActive) {
        for (uint32_t i = 0; i < _inPorts.size(); ++i) _plugin->setBusActivation(true, i, true);
        for (uint32_t i = 0; i < _outPorts.size(); ++i) _plugin->setBusActivation(false, i, true);
        if (!activatePlugin()) return false;
    }
    if (wasProcessing) {
        if (!_plugin->start_processing()) return false;
        _processing = true;
    }
    return true;
}

VstInt32 ClapAsVst2::stableId32(const char *s)
{
    uint32_t h = 2166136261u;
    if (s) {
        while (*s) {
            h ^= (uint8_t)*s++;
            h *= 16777619u;
        }
    }
    if (h == 0 || h == (uint32_t)kEffectMagic) h ^= 0xA5A5A5A5u;
    return (VstInt32)h;
}

int64_t ClapAsVst2::roundToInt64(double v)
{
    return v >= 0.0 ? (int64_t)(v + 0.5) : (int64_t)(v - 0.5);
}

bool ClapAsVst2::isInstrumentLike(const clap_plugin_audio_ports_t *audioports,
                                   const clap_plugin_note_ports_t *noteports,
                                   const clap_plugin_t *plugin) const
{
    if (!plugin || !noteports || noteports->count(plugin, true) == 0) return false;
    uint32_t ins = audioports ? audioports->count(plugin, true) : 0;
    uint32_t outs = audioports ? audioports->count(plugin, false) : 0;
    // A CLAP with note input and audio output is usable as a VST2 instrument.
    // The no-audio-input case is the common VSTi shape.
    bool instrumentFeature = false;
    const char *const *features = _library->plugins[0]->features;
    if (features) {
        for (size_t i = 0; features[i] != NULL; ++i) {
            if (!strcmp(features[i], CLAP_PLUGIN_FEATURE_INSTRUMENT) ||
                !strcmp(features[i], CLAP_PLUGIN_FEATURE_SYNTHESIZER)) {
                instrumentFeature = true;
                break;
            }
        }
    }
    return outs > 0 && (ins == 0 || instrumentFeature);
}

void ClapAsVst2::updateFlags()
{
    int32_t flags = effFlagsCanReplacing;
    if (isInstrumentLike(_plugin->_ext._audioports, _plugin->_ext._noteports, _plugin->_plugin)) flags |= effFlagsIsSynth;
    if (_plugin->_ext._gui) flags |= effFlagsHasEditor;
    if (_plugin->_ext._state) flags |= effFlagsProgramChunks;
    _effect.flags = flags;
}

void ClapAsVst2::populateEffectStruct()
{
    _effect.magic = kEffectMagic;
    _effect.dispatcher = dispatcherProc;
    _effect.processReplacing = processReplacingProc;
    _effect.DECLARE_VST_DEPRECATED(process) = NULL;
    _effect.setParameter = setParameterProc;
    _effect.getParameter = getParameterProc;
    _effect.numPrograms = _library ? (VstInt32)_library->plugins.size() : 1;
    _effect.numParams = (VstInt32)_params.size();
    _effect.numInputs = (VstInt32)_inputChannels;
    _effect.numOutputs = (VstInt32)_outputChannels;
    _effect.uniqueID = stableId32(_library->plugins[0]->id);
    _effect.version = kVstVersion;
    _effect.object = this;
    _effect.user = NULL;
    _effect.initialDelay = 0;
    _effect.future[0] = 0;
}

void ClapAsVst2::buildAudioLayout()
{
    _inputChannels = 0;
    _outputChannels = 0;
    if (!_plugin || !_plugin->_ext._audioports) return;

    const clap_plugin_audio_ports_t *ext = _plugin->_ext._audioports;
    uint32_t ni = ext->count(_plugin->_plugin, true);
    uint32_t no = ext->count(_plugin->_plugin, false);
    _inPorts.resize(ni);
    _outPorts.resize(no);
    if (!_inPorts.empty()) memset(&_inPorts[0], 0, _inPorts.size() * sizeof(clap_audio_buffer_t));
    if (!_outPorts.empty()) memset(&_outPorts[0], 0, _outPorts.size() * sizeof(clap_audio_buffer_t));

    for (uint32_t i = 0; i < ni; ++i) {
        clap_audio_port_info_t info;
        memset(&info, 0, sizeof(info));
        if (ext->get(_plugin->_plugin, i, true, &info)) {
            _inPorts[i].channel_count = info.channel_count;
            _inputChannels += info.channel_count;
        }
    }
    for (uint32_t i = 0; i < no; ++i) {
        clap_audio_port_info_t info;
        memset(&info, 0, sizeof(info));
        if (ext->get(_plugin->_plugin, i, false, &info)) {
            _outPorts[i].channel_count = info.channel_count;
            _outputChannels += info.channel_count;
        }
    }
}

void ClapAsVst2::buildParameters()
{
    _params.clear();
    _pendingParams.clear();
    _lastSentParams.clear();
    if (!_plugin || !_plugin->_ext._params) return;
    const clap_plugin_params_t *ext = _plugin->_ext._params;
    uint32_t count = ext->count(_plugin->_plugin);
    _params.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        clap_param_info_t info;
        memset(&info, 0, sizeof(info));
        if (!ext->get_info(_plugin->_plugin, i, &info)) continue;
        if (info.flags & CLAP_PARAM_IS_HIDDEN) continue;
        Param p;
        p.id = info.id;
        p.flags = info.flags;
        p.minValue = info.min_value;
        p.maxValue = info.max_value;
        p.def = info.default_value;
        p.name = info.name;
        p.module = info.module;
        char txt[128];
        memset(txt, 0, sizeof(txt));
        if (ext->value_to_text) ext->value_to_text(_plugin->_plugin, p.id, p.def, txt, sizeof(txt) - 1);
        p.display = txt;
        _params.push_back(p);
    }
    _pendingParams.resize(_params.size());
    _lastSentParams.resize(_params.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t i = 0; i < _params.size(); ++i) {
        double v = _params[i].def;
        if (_plugin->_ext._params->get_value)
            _plugin->_ext._params->get_value(_plugin->_plugin, _params[i].id, &v);
        _pendingParams[i] = v;
    }
}

void ClapAsVst2::buildNoteDialect()
{
    _usingClapNotes = false;
    _noteDialect = CLAP_NOTE_DIALECT_MIDI;
    _notePortIndex = 0;
    if (!_plugin || !_plugin->_ext._noteports || _plugin->_ext._noteports->count(_plugin->_plugin, true) == 0) return;
    clap_note_port_info_t info;
    memset(&info, 0, sizeof(info));
    if (_plugin->_ext._noteports->get(_plugin->_plugin, 0, true, &info)) {
        _notePortIndex = 0;
        if (info.preferred_dialect & CLAP_NOTE_DIALECT_CLAP) _noteDialect = CLAP_NOTE_DIALECT_CLAP;
        else if (info.preferred_dialect & CLAP_NOTE_DIALECT_MIDI) _noteDialect = CLAP_NOTE_DIALECT_MIDI;
        else if (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP) _noteDialect = CLAP_NOTE_DIALECT_CLAP;
        _usingClapNotes = (_noteDialect == CLAP_NOTE_DIALECT_CLAP);
    }
}

void ClapAsVst2::processEvents(const VstEvents *events)
{
    ScopeLock<Win32Lock> lock(&_pluginMutex);
    if (!events) return;
    for (VstInt32 i = 0; i < events->numEvents; ++i) {
        VstEvent *e = events->events[i];
        if (!e) continue;
        if (e->type == kVstMidiType) {
            const VstMidiEvent *m = reinterpret_cast<const VstMidiEvent *>(e);

            // Ignore MIDI System Real-Time messages (0xF8..0xFF).
            // They are not useful to the wrapped CLAP instrument and can be
            // misinterpreted by plugins which consume VST MIDI events.
            if ((uint8_t)m->midiData[0] >= 0xF8) continue;

            PendingMidi pm;
            pm.delta = m->deltaFrames;
            pm.data[0] = (uint8_t)m->midiData[0];
            pm.data[1] = (uint8_t)m->midiData[1];
            pm.data[2] = (uint8_t)m->midiData[2];
            _pendingMidi.push_back(pm);
        } else if (e->type == kVstSysExType) {
            const VstMidiSysexEvent *s = reinterpret_cast<const VstMidiSysexEvent *>(e);
            if (s->sysexDump && s->dumpBytes > 0) {
                PendingSysEx ps;
                ps.delta = s->deltaFrames;
                ps.data.assign((const uint8_t *)s->sysexDump, (const uint8_t *)s->sysexDump + s->dumpBytes);
                _pendingSysEx.push_back(ps);
            }
        }
    }
}

VstIntPtr ClapAsVst2::dispatcher(VstInt32 opcode, VstInt32 index, VstIntPtr value, void *ptr, float opt)
{
    switch (opcode) {
    case effOpen:
        if (_master)
            _master(&_effect, DECLARE_VST_DEPRECATED (audioMasterNeedIdle), 0, 0, NULL, 0);
        return 1;
    case DECLARE_VST_DEPRECATED (effIdle):
        serviceMainThreadCallback();
        return 1;
    case effClose:
        {
            ClapAsVst2 *self = asVst2(&_effect);
            delete self;
            return 1;
        }
    case effCanDo:
        if (!ptr) return 0;
        {
            const char *canDo = reinterpret_cast<const char *>(ptr);
            if (!std::strcmp(canDo, "CLAP2VST2VariableBlocks")) return 1;
            // The wrapper accepts VST2 MIDI events and translates them to CLAP
            // MIDI/note events, so report the standard VST2 MIDI receive
            // capabilities. The plural form is used by some hosts, including
            // VST hosts which do not use the singular spelling.
            if (!std::strcmp(canDo, "receiveVstMidiEvents") ||
                !std::strcmp(canDo, "receiveVstMidiEvent") ||
                !std::strcmp(canDo, "receiveVstEvents"))
                return 1;
            return 0;
        }
    case effGetProgramName:
        if (!ptr) return 0;
        {
            VstInt32 prog = _currentProgram;
            if (prog < 0 || !_library || (size_t)prog >= _library->plugins.size()) return 0;
            std::string n = getProgramDisplayName((size_t)prog);
            std::string ansi = utf8ToAnsi(n);
            vst_strncpy((char *)ptr, ansi.c_str(), kVstMaxProgNameLen);
            return 1;
        }
    case effGetProgramNameIndexed:
        if (!ptr || !_library || index < 0 || (size_t)index >= _library->plugins.size()) return 0;
        {
            std::string n = getProgramDisplayName((size_t)index);
            std::string ansi = utf8ToAnsi(n);
            vst_strncpy((char *)ptr, ansi.c_str(), kVstMaxProgNameLen);
            return 1;
        }
    case effSetProgramName:
        return 1;
    case effSetSampleRate:
        _lastSampleRate = opt > 0 ? opt : 44100.0f;
        if (_plugin) _plugin->setSampleRate(_lastSampleRate);
        return 1;
    case effSetBlockSize:
        if (value > 0) _configuredBlockSize = (uint32_t)clamp_i32(value, 1, INT32_MAX);
        return 1;
    case effStartProcess:
        if (!_plugin) return 0;
        if (!_active && !activatePlugin()) return 0;
        if (!_processing) {
            if (!_plugin->start_processing()) return 0;
            _processing = true;
        }
        return 1;
    case effStopProcess:
        if (_processing) { _plugin->stop_processing(); _processing = false; }
        if (_plugin) _plugin->reset();
        return 1;
    case effMainsChanged:
        if (!_plugin) return 1;
        if (value) {
            if (!_active) {
                for (uint32_t i = 0; i < _inPorts.size(); ++i) _plugin->setBusActivation(true, i, true);
                for (uint32_t i = 0; i < _outPorts.size(); ++i) _plugin->setBusActivation(false, i, true);
                if (!activatePlugin()) return 0;
                if (!_processing) {
                    if (!_plugin->start_processing()) return 0;
                    _processing = true;
                }
            }
        } else {
            if (_processing) { _plugin->stop_processing(); _processing = false; }
            if (_active) { _plugin->deactivate(); _active = false; }
        }
        return 1;
    case effProcessEvents:
        processEvents(reinterpret_cast<VstEvents *>(ptr));
        return 1;
    case effGetChunk: {
        if (!_plugin || !_plugin->_ext._state || !ptr) return 0;
        _state.clear();
        Vst2MemoryWriter writer(_state);
        clap_ostream_t out;
        out.ctx = &writer;
        out.write = Vst2MemoryWriter::write;
        if (!_plugin->save(&out)) return 0;
        *reinterpret_cast<void **>(ptr) = _state.empty() ? NULL : &_state[0];
        return (VstIntPtr)_state.size();
    }
    case effSetChunk: {
        if (!_plugin || !_plugin->_ext._state || !ptr || value <= 0) return 0;
        Vst2MemoryReader reader(ptr, (size_t)value);
        clap_istream_t in;
        in.ctx = &reader;
        in.read = Vst2MemoryReader::read;
        return _plugin->load(&in) ? 1 : 0;
    }
    case effGetParamName: {
        if (index < 0 || (size_t)index >= _params.size() || !ptr) return 0;
        std::string ansi = utf8ToAnsi(_params[index].name);
        vst_strncpy((char *)ptr, ansi.c_str(), kVstMaxParamStrLen);
        return 1;
    }
    case effGetParamDisplay: {
        if (index < 0 || (size_t)index >= _params.size() || !ptr) return 0;
        char txt[128];
        memset(txt, 0, sizeof(txt));
        double val = getParameter(index);
        double plain = _params[index].minValue + (double)val * (_params[index].maxValue - _params[index].minValue);
        if (_plugin->_ext._params && _plugin->_ext._params->value_to_text)
            _plugin->_ext._params->value_to_text(_plugin->_plugin, _params[index].id, plain, txt, sizeof(txt)-1);
        std::string ansi = utf8ToAnsi(txt);
        vst_strncpy((char *)ptr, ansi.c_str(), kVstMaxParamStrLen);
        return 1;
    }
    case effGetParamLabel:
        if (index < 0 || (size_t)index >= _params.size() || !ptr) return 0;
        ((char *)ptr)[0] = 0;
        return 1;
    case effGetEffectName: {
        if (!ptr) return 0;
        std::string ansi = utf8ToAnsi(_name);
        vst_strncpy((char *)ptr, ansi.c_str(), kVstMaxEffectNameLen);
        return 1;
    }
    case effGetVendorString: {
        if (!ptr) return 0;
        std::string ansi = utf8ToAnsi(_vendor);
        vst_strncpy((char *)ptr, ansi.c_str(), kVstMaxVendorStrLen);
        return 1;
    }
    case effGetProductString: {
        if (!ptr) return 0;
        std::string ansi = utf8ToAnsi(_name);
        vst_strncpy((char *)ptr, ansi.c_str(), kVstMaxProductStrLen);
        return 1;
    }
    case effGetVendorVersion:
        return 1;
    case effGetProgram:
        return _currentProgram;
    case effSetProgram:
        return selectProgram((int32_t)value) ? 1 : 0;
    case effEditGetRect:
        return editGetRect(reinterpret_cast<ERect **>(ptr)) ? 1 : 0;
    case effEditOpen:
        editOpen(ptr);
        return 1;
    case effEditClose:
        editClose();
        return 1;
    case effEditIdle:
        editIdle();
        return 1;
    case effCanBeAutomated:
        if (index < 0 || (size_t)index >= _params.size()) return 0;
        return (_params[index].flags & CLAP_PARAM_IS_AUTOMATABLE) ? 1 : 0;
    case effGetTailSize:
        if (_plugin && _plugin->_ext._tail && _active) return (VstIntPtr)std::min<uint32_t>(_plugin->_ext._tail->get(_plugin->_plugin), INT32_MAX);
        return 0;
    case effGetInputProperties:
    case effGetOutputProperties:
        return 0;
    case effGetPlugCategory:
        return (_effect.flags & effFlagsIsSynth) ? kPlugCategSynth : kPlugCategEffect;
    default:
        break;
    }
    return 0;
}

void ClapAsVst2::setParameter(VstInt32 index, float value)
{
    ScopeLock<Win32Lock> lock(&_pluginMutex);
    if (_pendingParams.empty() || index < 0 || (size_t)index >= _params.size()) return;
    if (_params[index].flags & CLAP_PARAM_IS_READONLY) return;
    double v = _params[index].minValue + std::max(0.0, std::min(1.0, (double)value)) * (_params[index].maxValue - _params[index].minValue);
    _pendingParams[index] = v;
    _dirty = true;
}

float ClapAsVst2::getParameter(VstInt32 index)
{
    ScopeLock<Win32Lock> lock(&_pluginMutex);
    if (index < 0 || (size_t)index >= _params.size()) return 0.0f;

    double v = _pendingParams[index];
    if (_params[index].maxValue == _params[index].minValue) return 0.0f;
    return (float)((v - _params[index].minValue) / (_params[index].maxValue - _params[index].minValue));
}

void ClapAsVst2::buildTransport(uint32_t frames)
{
    (void)frames;
    memset(&_transport, 0, sizeof(_transport));
    _transport.header.size = sizeof(_transport);
    _transport.header.type = CLAP_EVENT_TRANSPORT;
    _transport.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    _transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
    _transport.tempo = _transportTempo;

    if (_master) {
        VstTimeInfo *ti = reinterpret_cast<VstTimeInfo *>(_master(&_effect, audioMasterGetTime, 0,
                                                                  kVstPpqPosValid | kVstTempoValid | kVstBarsValid |
                                                                      kVstCyclePosValid | kVstTimeSigValid,
                                                                  NULL, 0));
        if (ti) {
            if (ti->flags & kVstTempoValid) _transport.tempo = ti->tempo;
            if (ti->flags & kVstPpqPosValid) {
                _transport.flags |= CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
                _transport.song_pos_beats = (clap_beattime)roundToInt64(ti->ppqPos * CLAP_BEATTIME_FACTOR);
            }
            {
                _transport.flags |= CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
                _transport.song_pos_seconds = (clap_sectime)roundToInt64((ti->samplePos / _lastSampleRate) * CLAP_SECTIME_FACTOR);
            }
            if (ti->flags & kVstCyclePosValid) {
                _transport.flags |= CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
                _transport.loop_start_beats = (clap_beattime)roundToInt64(ti->cycleStartPos * CLAP_BEATTIME_FACTOR);
                _transport.loop_end_beats = (clap_beattime)roundToInt64(ti->cycleEndPos * CLAP_BEATTIME_FACTOR);
            }
            if (ti->flags & kVstTimeSigValid) {
                _transport.flags |= CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
                _transport.tsig_num = ti->timeSigNumerator;
                _transport.tsig_denom = ti->timeSigDenominator;
            }
            if (ti->flags & kVstTransportPlaying) _transport.flags |= CLAP_TRANSPORT_IS_PLAYING;
            if (ti->flags & kVstTransportRecording) _transport.flags |= CLAP_TRANSPORT_IS_RECORDING;
            if (ti->flags & kVstTransportCycleActive) _transport.flags |= CLAP_TRANSPORT_IS_LOOP_ACTIVE;
        }
    }
}

bool ClapAsVst2::activatePlugin()
{
    // The VST2 block size is a capacity hint. Give CLAP room for shorter
    // blocks and occasional spikes, within CLAP's signed 32-bit frame limit.
    const uint32_t maxFrames = std::min<uint32_t>(_configuredBlockSize * 2u, INT32_MAX);
    _plugin->setBlockSizes(1, maxFrames);
    if (!_plugin->activate()) return false;
    _activeMaxFrames = maxFrames;
    _active = true;
    return true;
}

void ClapAsVst2::convertVstEventsToClap(uint32_t frames)
{
    _midiEvents.clear();
    _noteEvents.clear();
    _sysexEvents.clear();
    _sysexStorage.clear();
    _eventPtrs.clear();

    // Move the pending batch into private processing storage without copying.
    // Keep the pending vectors available for a possible re-entrant
    // processEvents() call while the current CLAP block is being prepared.
    _processingMidi.clear();
    _processingSysEx.clear();
    _pendingMidi.swap(_processingMidi);
    _pendingSysEx.swap(_processingSysEx);

    _midiEvents.reserve(_midiEvents.size() + _processingMidi.size());
    _noteEvents.reserve(_noteEvents.size() + _processingMidi.size());
    _sysexEvents.reserve(_sysexEvents.size() + _processingSysEx.size());
    _sysexStorage.reserve(_sysexStorage.size() + _processingSysEx.size());

    for (size_t midiIndex = 0; midiIndex < _processingMidi.size(); ++midiIndex) {
        const PendingMidi &m = _processingMidi[midiIndex];
        uint8_t status = m.data[0] & 0xF0;
        uint8_t ch = m.data[0] & 0x0F;
        int32_t t = m.delta;
        if (t < 0) t = 0;
        if ((uint32_t)t >= frames) t = (int32_t)(frames - 1);

        if (_usingClapNotes && (status == 0x80 || status == 0x90)) {
            clap_event_note_t n;
            memset(&n, 0, sizeof(n));
            n.header.size = sizeof(n);
            n.header.time = t;
            n.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            n.header.type = (status == 0x90 && m.data[2] != 0) ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF;
            n.port_index = _notePortIndex;
            n.channel = ch;
            n.key = m.data[1];
            n.note_id = -1;
            n.velocity = m.data[2] / 127.0;
            _noteEvents.push_back(n);
            _eventPtrs.push_back(&_noteEvents.back().header);
        } else {
            clap_event_midi_t e;
            memset(&e, 0, sizeof(e));
            e.header.size = sizeof(e);
            e.header.time = t;
            e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.header.type = CLAP_EVENT_MIDI;
            e.port_index = 0;
            e.data[0] = m.data[0];
            e.data[1] = m.data[1];
            e.data[2] = m.data[2];
            _midiEvents.push_back(e);
            _eventPtrs.push_back(&_midiEvents.back().header);
        }
    }
    for (size_t sysexIndex = 0; sysexIndex < _processingSysEx.size(); ++sysexIndex) {
        const PendingSysEx &s = _processingSysEx[sysexIndex];
        _sysexStorage.push_back(s.data);
        clap_event_midi_sysex_t e;
        memset(&e, 0, sizeof(e));
        e.header.size = sizeof(e);
        e.header.time = std::max<int32_t>(0, s.delta);
        if (e.header.time >= frames) e.header.time = frames - 1;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = CLAP_EVENT_MIDI_SYSEX;
        e.port_index = 0;
        e.buffer = _sysexStorage.back().empty() ? NULL : &_sysexStorage.back()[0];
        e.size = _sysexStorage.back().size();
        _sysexEvents.push_back(e);
        _eventPtrs.push_back(&_sysexEvents.back().header);
    }

    _processingMidi.clear();
    _processingSysEx.clear();
}

void ClapAsVst2::sendClapProcess(uint32_t frames, float **inputs, float **outputs)
{
    if (!_plugin || !_active) return;
    _clapOutputEvents.clear();

    buildTransport(frames);
    convertVstEventsToClap(frames);

    // Parameter events are emitted from a dedicated local vector to avoid heap allocation in common cases.
    std::vector<clap_event_param_value_t> paramEvents;
    paramEvents.reserve(_params.size());
    for (size_t i = 0; i < _params.size(); ++i) {
        double v = _pendingParams[i];
        if (v == _lastSentParams[i]) continue;
        clap_event_param_value_t e;
        memset(&e, 0, sizeof(e));
        e.header.size = sizeof(e);
        e.header.time = 0;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = CLAP_EVENT_PARAM_VALUE;
        e.param_id = _params[i].id;
        e.note_id = -1;
        e.port_index = -1;
        e.channel = -1;
        e.key = -1;
        e.value = v;
        paramEvents.push_back(e);
        _lastSentParams[i] = v;
    }
    for (size_t paramIndex = 0; paramIndex < paramEvents.size(); ++paramIndex)
        _eventPtrs.push_back(&paramEvents[paramIndex].header);

    std::stable_sort(_eventPtrs.begin(), _eventPtrs.end(), eventTimeLess);

    InputEventSlice eventSlice;
    _inputEvents.ctx = &eventSlice;
    _inputEvents.size = inputEventsSize;
    _inputEvents.get = inputEventsGet;

    _clapOutEvents.ctx = this;
    _clapOutEvents.try_push = outputEventsTryPush;

    uint32_t cIn = (uint32_t)_inPorts.size();
    uint32_t cOut = (uint32_t)_outPorts.size();
    _inPtrs.resize(_inputChannels);
    _outPtrs.resize(_outputChannels);
    _process.steady_time = -1;
    _process.audio_inputs = cIn ? &_inPorts[0] : NULL;
    _process.audio_outputs = cOut ? &_outPorts[0] : NULL;
    _process.audio_inputs_count = cIn;
    _process.audio_outputs_count = cOut;
    _process.in_events = &_inputEvents;
    _process.out_events = &_clapOutEvents;

    const clap_event_transport_t transport = _transport;
    size_t nextEvent = 0;
    for (uint32_t offset = 0; offset < frames;) {
        const uint32_t count = std::min(frames - offset, _activeMaxFrames);
        const uint32_t end = offset + count;

        // Input times are relative to this CLAP block. Output times are
        // converted back to offsets within the original VST2 call below.
        const size_t firstEvent = nextEvent;
        while (nextEvent < _eventPtrs.size() && _eventPtrs[nextEvent]->time < end)
            _eventPtrs[nextEvent++]->time -= offset;
        eventSlice.count = (uint32_t)(nextEvent - firstEvent);
        eventSlice.events = eventSlice.count ? &_eventPtrs[firstEvent] : NULL;

        uint32_t k = 0;
        for (uint32_t b = 0; b < cIn; ++b) {
            _inPorts[b].data32 = _inPtrs.empty() ? NULL : &_inPtrs[k];
            for (uint32_t c = 0; c < _inPorts[b].channel_count; ++c, ++k)
                _inPtrs[k] = inputs && inputs[k] ? inputs[k] + offset : NULL;
            _inPorts[b].data64 = NULL;
            _inPorts[b].constant_mask = 0;
        }
        k = 0;
        for (uint32_t b = 0; b < cOut; ++b) {
            _outPorts[b].data32 = _outPtrs.empty() ? NULL : &_outPtrs[k];
            for (uint32_t c = 0; c < _outPorts[b].channel_count; ++c, ++k)
                _outPtrs[k] = outputs && outputs[k] ? outputs[k] + offset : NULL;
            _outPorts[b].data64 = NULL;
            _outPorts[b].constant_mask = 0;
        }

        if (offset && (transport.flags & CLAP_TRANSPORT_IS_PLAYING)) {
            const double seconds = offset / (double)_lastSampleRate;
            _transport.song_pos_seconds = transport.song_pos_seconds
                + roundToInt64(seconds * CLAP_SECTIME_FACTOR);
            if (transport.flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE)
                _transport.song_pos_beats = transport.song_pos_beats
                    + roundToInt64(seconds * transport.tempo * CLAP_BEATTIME_FACTOR / 60.0);
        }

        _process.frames_count = count;
        _process.transport = &_transport;
        const size_t firstOutput = _clapOutputEvents.size();
        _plugin->_plugin->process(_plugin->_plugin, &_process);
        for (size_t i = firstOutput; i < _clapOutputEvents.size(); ++i)
            _clapOutputEvents[i].time += offset;
        offset = end;
    }
    _inputEvents.ctx = NULL;
}

void ClapAsVst2::processReplacing(float **inputs, float **outputs, VstInt32 sampleFrames)
{
    ScopeLock<Win32Lock> lock(&_pluginMutex);
    if (!_opened || !_plugin || sampleFrames <= 0) return;
    if (!_active && !activatePlugin()) return;
    if (!_processing) {
        if (!_plugin->start_processing()) return;
        _processing = true;
    }

    sendClapProcess((uint32_t)sampleFrames, inputs, outputs);
    sendOutputEventsToHost();
}

void ClapAsVst2::collectOutputEvent(const clap_event_header_t *event)
{
    if (!event) return;
    if (event->type == CLAP_EVENT_MIDI) {
        const clap_event_midi_t *m = reinterpret_cast<const clap_event_midi_t *>(event);
        OutputEvent o;
        o.time = event->time;
        o.kind = 1;
        o.data[0] = m->data[0]; o.data[1] = m->data[1]; o.data[2] = m->data[2];
        _clapOutputEvents.push_back(o);
    } else if (event->type == CLAP_EVENT_MIDI_SYSEX) {
        const clap_event_midi_sysex_t *s = reinterpret_cast<const clap_event_midi_sysex_t *>(event);
        OutputEvent o;
        o.time = event->time;
        o.kind = 2;
        o.sysex.assign(s->buffer, s->buffer + s->size);
        _clapOutputEvents.push_back(o);
    } else if (event->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ||
               event->type == CLAP_EVENT_PARAM_GESTURE_END ||
               event->type == CLAP_EVENT_PARAM_VALUE) {
        const clap_id *param_id = NULL;
        if (event->type == CLAP_EVENT_PARAM_GESTURE_BEGIN) {
            const clap_event_param_gesture_t *g = reinterpret_cast<const clap_event_param_gesture_t *>(event);
            param_id = &g->param_id;
        } else if (event->type == CLAP_EVENT_PARAM_GESTURE_END) {
            const clap_event_param_gesture_t *g = reinterpret_cast<const clap_event_param_gesture_t *>(event);
            param_id = &g->param_id;
        } else {
            const clap_event_param_value_t *p = reinterpret_cast<const clap_event_param_value_t *>(event);
            param_id = &p->param_id;
        }

        for (size_t i = 0; i < _params.size(); ++i) {
            if (_params[i].id == *param_id) {
                if (event->type == CLAP_EVENT_PARAM_GESTURE_BEGIN) {
                    if (_master) _master(&_effect, audioMasterBeginEdit, (VstInt32)i, 0, NULL, 0.0f);
                } else if (event->type == CLAP_EVENT_PARAM_GESTURE_END) {
                    if (_master) _master(&_effect, audioMasterEndEdit, (VstInt32)i, 0, NULL, 0.0f);
                } else {
                    const clap_event_param_value_t *p = reinterpret_cast<const clap_event_param_value_t *>(event);
                    _pendingParams[i] = p->value;
                    _lastSentParams[i] = p->value;
                    if (_master) _master(&_effect, audioMasterAutomate, (VstInt32)i, 0, NULL, getParameter((VstInt32)i));
                }
                break;
            }
        }
    } else if (event->type == CLAP_EVENT_NOTE_ON || event->type == CLAP_EVENT_NOTE_OFF) {
        const clap_event_note_t *n = reinterpret_cast<const clap_event_note_t *>(event);
        OutputEvent o;
        o.time = event->time;
        o.kind = 1;
        uint8_t status = event->type == CLAP_EVENT_NOTE_ON ? 0x90 : 0x80;
        o.data[0] = status | (uint8_t)(n->channel & 0x0F);
        o.data[1] = (uint8_t)std::max(0, std::min(127, (int)n->key));
        o.data[2] = (uint8_t)std::max(0, std::min(127, (int)roundToInt64(n->velocity * 127.0)));
        _clapOutputEvents.push_back(o);
    }
}

void ClapAsVst2::sendOutputEventsToHost()
{
    if (_clapOutputEvents.empty() || !_master) return;

    std::vector<VstEvent *> ptrs;
    std::vector<VstMidiEvent> midi;
    std::vector<VstMidiSysexEvent> sysex;
    std::vector<std::vector<char> > sysexBuffers;

    const size_t maximumEvents = _clapOutputEvents.size();
    ptrs.reserve(maximumEvents);
    midi.reserve(maximumEvents);
    sysex.reserve(maximumEvents);
    sysexBuffers.reserve(maximumEvents);

    for (size_t eventIndex = 0; eventIndex < _clapOutputEvents.size(); ++eventIndex)
    {
        const OutputEvent &o = _clapOutputEvents[eventIndex];
        if (o.kind == 1)
        {
            VstMidiEvent e;
            memset(&e, 0, sizeof(e));
            e.type = kVstMidiType;
            e.byteSize = sizeof(VstMidiEvent);
            e.deltaFrames = o.time;
            e.midiData[0] = (char)o.data[0];
            e.midiData[1] = (char)o.data[1];
            e.midiData[2] = (char)o.data[2];
            e.midiData[3] = 0;
            midi.push_back(e);
            ptrs.push_back(reinterpret_cast<VstEvent *>(&midi.back()));
        }
        else if (o.kind == 2 && !o.sysex.empty())
        {
            std::vector<char> buffer(o.sysex.size());
            memcpy(&buffer[0], &o.sysex[0], o.sysex.size());
            sysexBuffers.push_back(buffer);

            VstMidiSysexEvent e;
            memset(&e, 0, sizeof(e));
            e.type = kVstSysExType;
            e.byteSize = sizeof(VstMidiSysexEvent);
            e.deltaFrames = o.time;
            e.dumpBytes = (VstInt32)sysexBuffers.back().size();
            e.sysexDump = sysexBuffers.back().empty() ? NULL : &sysexBuffers.back()[0];
            sysex.push_back(e);
            ptrs.push_back(reinterpret_cast<VstEvent *>(&sysex.back()));
        }
    }

    if (ptrs.empty())
    {
        _clapOutputEvents.clear();
        return;
    }

    const size_t storageSize = sizeof(VstEvents) +
        (ptrs.size() > 2 ? (ptrs.size() - 2) * sizeof(VstEvent *) : 0);
    std::vector<uint8_t> storage(storageSize);
    VstEvents *events = reinterpret_cast<VstEvents *>(&storage[0]);
    events->numEvents = (VstInt32)ptrs.size();
    events->reserved = 0;
    for (size_t i = 0; i < ptrs.size(); ++i) events->events[i] = ptrs[i];

    _master(&_effect, audioMasterProcessEvents, 0, 0, events, 0);
    _clapOutputEvents.clear();
}

void ClapAsVst2::editOpen(void *parent)
{
    if (!_plugin || !_plugin->_ext._gui || _editorOpen) return;
    _editorParent = reinterpret_cast<HWND>(parent);
    const clap_plugin_gui_t *gui = _plugin->_ext._gui;
    if (!gui->is_api_supported(_plugin->_plugin, CLAP_WINDOW_API_WIN32, false)) return;
    if (!gui->create(_plugin->_plugin, CLAP_WINDOW_API_WIN32, false)) return;
    clap_window_t win;
    memset(&win, 0, sizeof(win));
    win.api = CLAP_WINDOW_API_WIN32;
    win.ptr = _editorParent;
    if (!gui->set_parent(_plugin->_plugin, &win)) {
        gui->destroy(_plugin->_plugin);
        return;
    }
    if (gui->get_size) {
        uint32_t w = 0, h = 0;
        if (gui->get_size(_plugin->_plugin, &w, &h)) {
            _rect.left = 0; _rect.top = 0; _rect.right = (VstInt16)std::min<uint32_t>(w, 32767); _rect.bottom = (VstInt16)std::min<uint32_t>(h, 32767);
        }
    }
    if (gui->show) gui->show(_plugin->_plugin);
    _editorOpen = true;
}

void ClapAsVst2::editClose()
{
    if (_plugin && _plugin->_ext._gui && _editorOpen) {
        if (_plugin->_ext._gui->hide) _plugin->_ext._gui->hide(_plugin->_plugin);
        if (_plugin->_ext._gui->destroy) _plugin->_ext._gui->destroy(_plugin->_plugin);
    }
    _editorOpen = false;
    _editorParent = NULL;
}

bool ClapAsVst2::editGetRect(ERect **rect)
{
    if (!rect) return false;
    *rect = &_rect;
    return true;
}

void ClapAsVst2::editIdle()
{
    serviceMainThreadCallback();
    // Use the VST2 editor idle callback as an additional main-thread parameter
    // synchronization point. This is deliberately independent of CLAP output
    // parameter events: some VST2 hosts do not immediately reflect an
    // audioMasterAutomate callback originating from their processing thread.
    syncClapParameters();
}

void ClapAsVst2::syncClapParameters()
{
    ScopeLock<Win32Lock> lock(&_pluginMutex);
    if (!_plugin || !_plugin->_ext._params || !_plugin->_ext._params->get_value ||
        _pendingParams.empty()) return;

    for (size_t i = 0; i < _params.size(); ++i) {
        double pending = _pendingParams[i];
        const double sent = _lastSentParams[i];

        // A host-originated VST2 change is waiting for the next process block.
        // Do not overwrite it with the older CLAP value.
        if (!isNan(sent) && pending != sent) continue;

        double live = pending;
        if (!_plugin->_ext._params->get_value(_plugin->_plugin, _params[i].id, &live))
            continue;

        if (live == pending) continue;

        _pendingParams[i] = live;
        _lastSentParams[i] = live;
        if (_master) {
            const double range = _params[i].maxValue - _params[i].minValue;
            const float normalized = range == 0.0
                ? 0.0f
                : (float)((live - _params[i].minValue) / range);
            _master(&_effect, audioMasterAutomate, (VstInt32)i, 0, NULL, normalized);
        }
    }
}

bool ClapAsVst2::gui_can_resize()
{
    return _plugin && _plugin->_ext._gui && _plugin->_ext._gui->can_resize &&
           _plugin->_ext._gui->can_resize(_plugin->_plugin);
}

bool ClapAsVst2::gui_request_resize(uint32_t width, uint32_t height)
{
    _rect.right = (VstInt16)std::min<uint32_t>(width, 32767);
    _rect.bottom = (VstInt16)std::min<uint32_t>(height, 32767);
    if (_master)
        _master(&_effect, audioMasterSizeWindow, (VstInt32)width,
                (VstIntPtr)height, NULL, 0);
    return true;
}

bool ClapAsVst2::gui_request_show()
{
    return _editorOpen;
}

bool ClapAsVst2::gui_request_hide()
{
    return _editorOpen;
}

void ClapAsVst2::mark_dirty()
{
    _dirty = true;
}

void ClapAsVst2::restartPlugin()
{
    if (_master) _master(&_effect, audioMasterIOChanged, 0, 0, NULL, 0);
}

void ClapAsVst2::request_callback()
{
    InterlockedExchange(&_mainCallbackPending, 1);
}

void ClapAsVst2::serviceMainThreadCallback()
{
    if (GetCurrentThreadId() != _mainThreadId || _servicingMainCallback ||
        InterlockedCompareExchange(&_mainCallbackPending, 0, 0) == 0)
        return;

    ScopeLock<Win32Lock> lock(&_pluginMutex);
    if (!_opened || !_plugin || !_plugin->_plugin->on_main_thread) return;
    if (InterlockedExchange(&_mainCallbackPending, 0) != 0) {
        _servicingMainCallback = true;
        _plugin->_plugin->on_main_thread(_plugin->_plugin);
        _servicingMainCallback = false;
    }
}

void ClapAsVst2::param_rescan(clap_param_rescan_flags)
{
    ScopeLock<Win32Lock> lock(&_pluginMutex);
    buildParameters();
    _effect.numParams = (VstInt32)_params.size();
}

void ClapAsVst2::param_clear(clap_id, clap_param_clear_flags)
{
}

void ClapAsVst2::param_request_flush()
{
    // Intentional no-op. When processing is active, parameter input/output is
    // handled by process(); editIdle() separately synchronizes current CLAP
    // parameter values. This preserves the tested baseline and does not revive
    // the discarded deferred-flush experiment.
}

void ClapAsVst2::latency_changed()
{
    if (_plugin && _plugin->_ext._latency && _master)
        _master(&_effect, audioMasterIOChanged, 0, 0, NULL, 0);
}

void ClapAsVst2::tail_changed()
{
    if (_master) _master(&_effect, audioMasterUpdateDisplay, 0, 0, NULL, 0);
}

const void *CLAP_ABI ClapAsVst2::hostGetExtension(const clap_host_t *host,
                                                   const char *extensionId)
{
    if (!host || !host->host_data || !extensionId) return NULL;
    ClapAsVst2 *self = static_cast<ClapAsVst2 *>(host->host_data);

    if (!strcmp(extensionId, CLAP_EXT_PARAMS)) return &self->_hostParams;
    if (!strcmp(extensionId, CLAP_EXT_STATE)) return &self->_hostState;
    if (!strcmp(extensionId, CLAP_EXT_GUI)) return &self->_hostGui;
    if (!strcmp(extensionId, CLAP_EXT_LATENCY)) return &self->_hostLatency;
    if (!strcmp(extensionId, CLAP_EXT_TAIL)) return &self->_hostTail;

    // In particular, do not expose CLAP_EXT_THREAD_CHECK: the tested wrapper
    // no longer depends on GetCurrentThreadId/main-thread bookkeeping.
    return NULL;
}

void CLAP_ABI ClapAsVst2::hostRequestRestart(const clap_host_t *host)
{
    if (!host || !host->host_data) return;
    static_cast<ClapAsVst2 *>(host->host_data)->restartPlugin();
}

void CLAP_ABI ClapAsVst2::hostRequestProcess(const clap_host_t *)
{
    // VST2 has no direct equivalent which safely forces an arbitrary host to
    // process immediately. This matches the old proxy behavior.
}

void CLAP_ABI ClapAsVst2::hostRequestCallback(const clap_host_t *host)
{
    if (!host || !host->host_data) return;
    static_cast<ClapAsVst2 *>(host->host_data)->request_callback();
}

void CLAP_ABI ClapAsVst2::hostParamsRescan(const clap_host_t *host,
                                            clap_param_rescan_flags flags)
{
    if (!host || !host->host_data) return;
    static_cast<ClapAsVst2 *>(host->host_data)->param_rescan(flags);
}

void CLAP_ABI ClapAsVst2::hostParamsClear(const clap_host_t *host, clap_id paramId,
                                           clap_param_clear_flags flags)
{
    if (!host || !host->host_data) return;
    static_cast<ClapAsVst2 *>(host->host_data)->param_clear(paramId, flags);
}

void CLAP_ABI ClapAsVst2::hostParamsRequestFlush(const clap_host_t *host)
{
    if (!host || !host->host_data) return;
    static_cast<ClapAsVst2 *>(host->host_data)->param_request_flush();
}

void CLAP_ABI ClapAsVst2::hostStateMarkDirty(const clap_host_t *host)
{
    if (!host || !host->host_data) return;
    static_cast<ClapAsVst2 *>(host->host_data)->mark_dirty();
}

void CLAP_ABI ClapAsVst2::hostGuiResizeHintsChanged(const clap_host_t *)
{
    // VST2 has no matching resize-hints notification.
}

bool CLAP_ABI ClapAsVst2::hostGuiRequestResize(const clap_host_t *host,
                                                uint32_t width, uint32_t height)
{
    if (!host || !host->host_data) return false;
    return static_cast<ClapAsVst2 *>(host->host_data)->gui_request_resize(width, height);
}

bool CLAP_ABI ClapAsVst2::hostGuiRequestShow(const clap_host_t *host)
{
    if (!host || !host->host_data) return false;
    return static_cast<ClapAsVst2 *>(host->host_data)->gui_request_show();
}

bool CLAP_ABI ClapAsVst2::hostGuiRequestHide(const clap_host_t *host)
{
    if (!host || !host->host_data) return false;
    return static_cast<ClapAsVst2 *>(host->host_data)->gui_request_hide();
}

void CLAP_ABI ClapAsVst2::hostGuiClosed(const clap_host_t *host, bool)
{
    if (!host || !host->host_data) return;
    ClapAsVst2 *self = static_cast<ClapAsVst2 *>(host->host_data);
    self->_editorOpen = false;
    self->_editorParent = NULL;
}

void CLAP_ABI ClapAsVst2::hostLatencyChanged(const clap_host_t *host)
{
    if (!host || !host->host_data) return;
    static_cast<ClapAsVst2 *>(host->host_data)->latency_changed();
}

void CLAP_ABI ClapAsVst2::hostTailChanged(const clap_host_t *host)
{
    if (!host || !host->host_data) return;
    static_cast<ClapAsVst2 *>(host->host_data)->tail_changed();
}

uint32_t CLAP_ABI ClapAsVst2::inputEventsSize(const clap_input_events *list)
{
    if (!list || !list->ctx) return 0;
    return static_cast<const InputEventSlice *>(list->ctx)->count;
}

const clap_event_header_t *CLAP_ABI ClapAsVst2::inputEventsGet(
    const clap_input_events *list, uint32_t index)
{
    if (!list || !list->ctx) return NULL;
    const InputEventSlice *slice = static_cast<const InputEventSlice *>(list->ctx);
    return index < slice->count ? slice->events[index] : NULL;
}

bool CLAP_ABI ClapAsVst2::outputEventsTryPush(const clap_output_events *list,
                                               const clap_event_header_t *event)
{
    if (!list || !list->ctx || !event) return false;
    static_cast<ClapAsVst2 *>(list->ctx)->collectOutputEvent(event);
    return true;
}

int64_t Vst2MemoryReader::read(const clap_istream_t *stream, void *buffer, uint64_t size)
{
    if (!stream || !stream->ctx || !buffer) return -1;
    Vst2MemoryReader *self = static_cast<Vst2MemoryReader *>(stream->ctx);
    size_t remain = self->_size - self->_pos;
    size_t n = (size_t)std::min<uint64_t>(size, remain);
    if (n) memcpy(buffer, self->_data + self->_pos, n);
    self->_pos += n;
    return (int64_t)n;
}

int64_t Vst2MemoryWriter::write(const clap_ostream_t *stream, const void *buffer, uint64_t size)
{
    if (!stream || !stream->ctx || (!buffer && size)) return -1;
    Vst2MemoryWriter *self = static_cast<Vst2MemoryWriter *>(stream->ctx);
    const size_t old = self->_data.size();
    self->_data.resize(old + (size_t)size);
    if (size) memcpy(&self->_data[old], buffer, (size_t)size);
    return (int64_t)size;
}

int64_t CLAP_ABI ClapAsVst2::stateRead(const clap_istream_t *stream, void *buffer, uint64_t size)
{
    return Vst2MemoryReader::read(stream, buffer, size);
}

int64_t CLAP_ABI ClapAsVst2::stateWrite(const clap_ostream_t *stream, const void *buffer, uint64_t size)
{
    return Vst2MemoryWriter::write(stream, buffer, size);
}
