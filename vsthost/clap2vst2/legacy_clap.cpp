#include "legacy_clap.h"

#include <cstring>

namespace Clap
{
PluginExtensions::PluginExtensions()
    : _state(NULL),
      _params(NULL),
      _audioports(NULL),
      _audio_ports_activation(NULL),
      _noteports(NULL),
      _gui(NULL),
      _latency(NULL),
      _tail(NULL)
{
}

Library::Library()
    : _pluginFactory(NULL), _module(NULL), _entry(NULL), _entryInitialized(false)
{
}

Library::~Library()
{
    unload();
}

void Library::unload()
{
    plugins.clear();
    _pluginFactory = NULL;

    if (_entry && _entryInitialized && _entry->deinit)
        _entry->deinit();

    _entryInitialized = false;
    _entry = NULL;

    if (_module)
    {
        FreeLibrary(_module);
        _module = NULL;
    }
}

bool Library::load(const TCHAR *path)
{
    if (!path || !*path) return false;
    unload();

    _module = LoadLibrary(path);
    return _module && initializeLoadedModule(LegacyWin32::pathToUtf8(tstring(path)));
}

bool Library::initializeLoadedModule(const std::string &utf8Path)
{
    _entry = reinterpret_cast<const clap_plugin_entry_t *>(
        GetProcAddress(_module, "clap_entry"));
    if (!_entry || !clap_version_is_compatible(_entry->clap_version) ||
        !_entry->init || !_entry->get_factory)
    {
        unload();
        return false;
    }

    if (utf8Path.empty() || !_entry->init(utf8Path.c_str()))
    {
        // CLAP specifies that deinit() must not be called after a failed init().
        _entry = NULL;
        FreeLibrary(_module);
        _module = NULL;
        return false;
    }
    _entryInitialized = true;

    _pluginFactory = reinterpret_cast<const clap_plugin_factory_t *>(
        _entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!_pluginFactory || !_pluginFactory->get_plugin_count ||
        !_pluginFactory->get_plugin_descriptor || !_pluginFactory->create_plugin)
    {
        unload();
        return false;
    }

    const uint32_t count = _pluginFactory->get_plugin_count(_pluginFactory);
    for (uint32_t i = 0; i < count; ++i)
    {
        const clap_plugin_descriptor_t *descriptor =
            _pluginFactory->get_plugin_descriptor(_pluginFactory, i);
        if (!descriptor || !descriptor->id || !*descriptor->id) continue;
        if (!clap_version_is_compatible(descriptor->clap_version)) continue;
        plugins.push_back(descriptor);
    }

    if (plugins.empty())
    {
        unload();
        return false;
    }
    return true;
}

Plugin::Plugin()
    : _plugin(NULL), _sampleRate(44100.0), _minFrames(0), _maxFrames(0)
{
}

Plugin::~Plugin()
{
    terminate();
}

Plugin *Plugin::createInstance(Library &library, size_t index, const clap_host_t *host)
{
    if (!library._pluginFactory || index >= library.plugins.size() || !host)
        return NULL;

    const clap_plugin_descriptor_t *descriptor = library.plugins[index];
    if (!descriptor || !descriptor->id) return NULL;

    const clap_plugin_t *raw = library._pluginFactory->create_plugin(
        library._pluginFactory, host, descriptor->id);
    if (!raw) return NULL;

    Plugin *result = new Plugin();
    if (!result)
    {
        if (raw->destroy) raw->destroy(raw);
        return NULL;
    }

    result->_plugin = raw;
    if (!raw->init || !raw->init(raw))
    {
        result->terminate();
        delete result;
        return NULL;
    }

    result->cacheExtensions();
    return result;
}

void Plugin::cacheExtensions()
{
    if (!_plugin || !_plugin->get_extension) return;

    _ext._state = reinterpret_cast<const clap_plugin_state_t *>(
        _plugin->get_extension(_plugin, CLAP_EXT_STATE));
    _ext._params = reinterpret_cast<const clap_plugin_params_t *>(
        _plugin->get_extension(_plugin, CLAP_EXT_PARAMS));
    _ext._audioports = reinterpret_cast<const clap_plugin_audio_ports_t *>(
        _plugin->get_extension(_plugin, CLAP_EXT_AUDIO_PORTS));
    _ext._audio_ports_activation = reinterpret_cast<const clap_plugin_audio_ports_activation_t *>(
        _plugin->get_extension(_plugin, CLAP_EXT_AUDIO_PORTS_ACTIVATION));
    if (!_ext._audio_ports_activation)
        _ext._audio_ports_activation = reinterpret_cast<const clap_plugin_audio_ports_activation_t *>(
            _plugin->get_extension(_plugin, CLAP_EXT_AUDIO_PORTS_ACTIVATION_COMPAT));
    _ext._noteports = reinterpret_cast<const clap_plugin_note_ports_t *>(
        _plugin->get_extension(_plugin, CLAP_EXT_NOTE_PORTS));
    _ext._gui = reinterpret_cast<const clap_plugin_gui_t *>(
        _plugin->get_extension(_plugin, CLAP_EXT_GUI));
    if (_ext._gui && _ext._gui->is_api_supported &&
        !_ext._gui->is_api_supported(_plugin, CLAP_WINDOW_API_WIN32, false))
        _ext._gui = NULL;
    _ext._latency = reinterpret_cast<const clap_plugin_latency_t *>(
        _plugin->get_extension(_plugin, CLAP_EXT_LATENCY));
    _ext._tail = reinterpret_cast<const clap_plugin_tail_t *>(
        _plugin->get_extension(_plugin, CLAP_EXT_TAIL));
}

void Plugin::terminate()
{
    if (_plugin)
    {
        if (_plugin->destroy) _plugin->destroy(_plugin);
        _plugin = NULL;
    }
    _ext = PluginExtensions();
}

void Plugin::setBusActivation(bool isInput, uint32_t portIndex, bool active)
{
    if (_plugin && _ext._audio_ports_activation && _ext._audio_ports_activation->set_active)
        _ext._audio_ports_activation->set_active(_plugin, isInput, portIndex, active, 32);
}

bool Plugin::activate()
{
    return _plugin && _plugin->activate &&
           _plugin->activate(_plugin, _sampleRate, _minFrames, _maxFrames);
}

void Plugin::deactivate()
{
    if (_plugin && _plugin->deactivate) _plugin->deactivate(_plugin);
}

bool Plugin::start_processing()
{
    return _plugin && _plugin->start_processing && _plugin->start_processing(_plugin);
}

void Plugin::stop_processing()
{
    if (_plugin && _plugin->stop_processing) _plugin->stop_processing(_plugin);
}

void Plugin::reset()
{
    if (_plugin && _plugin->reset) _plugin->reset(_plugin);
}

bool Plugin::save(const clap_ostream_t *stream) const
{
    return _plugin && _ext._state && _ext._state->save &&
           _ext._state->save(_plugin, stream);
}

bool Plugin::load(const clap_istream_t *stream) const
{
    return _plugin && _ext._state && _ext._state->load &&
           _ext._state->load(_plugin, stream);
}
}
