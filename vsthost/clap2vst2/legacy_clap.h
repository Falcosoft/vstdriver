#pragma once

#include "legacy_win32.h"

// The CLAP module supplies clap_entry, which we resolve with GetProcAddress.
#define CLAP_EXPORT
#include "clap/entry.h"
#include "clap/factory/plugin-factory.h"
#include "clap/plugin-features.h"
#include "clap/ext/audio-ports.h"
#include "clap/ext/audio-ports-activation.h"
#include "clap/ext/gui.h"
#include "clap/ext/latency.h"
#include "clap/ext/note-ports.h"
#include "clap/ext/params.h"
#include "clap/ext/state.h"
#include "clap/ext/tail.h"

#include <vector>

namespace Clap
{
struct PluginExtensions
{
    const clap_plugin_state_t *_state;
    const clap_plugin_params_t *_params;
    const clap_plugin_audio_ports_t *_audioports;
    const clap_plugin_audio_ports_activation_t *_audio_ports_activation;
    const clap_plugin_note_ports_t *_noteports;
    const clap_plugin_gui_t *_gui;
    const clap_plugin_latency_t *_latency;
    const clap_plugin_tail_t *_tail;

    PluginExtensions();
};

class Library
{
public:
    Library();
    ~Library();

    bool load(const TCHAR *path);
    void unload();
    bool hasEntryPoint() const { return _entry != NULL; }

    const clap_plugin_factory_t *_pluginFactory;
    std::vector<const clap_plugin_descriptor_t *> plugins;

private:
    bool initializeLoadedModule(const std::string &utf8Path);

    HMODULE _module;
    const clap_plugin_entry_t *_entry;
    bool _entryInitialized;

    Library(const Library &);
    Library &operator=(const Library &);
};

class Plugin
{
public:
    Plugin();
    ~Plugin();

    static Plugin *createInstance(Library &library, size_t index, const clap_host_t *host);

    // The modern clap_proxy initialize() method only triggered wrapper-side
    // setup callbacks. The legacy wrapper performs those setup steps directly.
    bool initialize() const { return _plugin != NULL; }

    void terminate();
    void destroy() { terminate(); }

    void setSampleRate(double sampleRate) { _sampleRate = sampleRate; }
    void setBlockSizes(uint32_t minFrames, uint32_t maxFrames)
    {
        _minFrames = minFrames;
        _maxFrames = maxFrames;
    }

    void setBusActivation(bool isInput, uint32_t portIndex, bool active);
    bool activate();
    void deactivate();
    bool start_processing();
    void stop_processing();
    void reset();

    bool save(const clap_ostream_t *stream) const;
    bool load(const clap_istream_t *stream) const;

    const clap_plugin_t *_plugin;
    PluginExtensions _ext;

private:
    void cacheExtensions();

    double _sampleRate;
    uint32_t _minFrames;
    uint32_t _maxFrames;

    Plugin(const Plugin &);
    Plugin &operator=(const Plugin &);
};
}
