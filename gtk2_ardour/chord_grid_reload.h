/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Chord Grid development reload. Called on Ardour's GUI thread, stopped only. */
#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <glib.h>
#include "pbd/xml++.h"
#include "ardour/audioengine.h"
#include "ardour/plugin_insert.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/vst3_module.h"
#include "ardour/vst3_plugin.h"

namespace ChordGridReload {
inline bool supported (std::shared_ptr<ARDOUR::PluginInsert> const& insert)
{
	return insert && insert->get_count () == 1 && insert->plugin ()->unique_id () == "ABCDEF019182FAEB4A61797343676D69"
	       && insert->plugin ()->get_info ()->type == ARDOUR::VST3;
}

/* A new pathname avoids both Ardour's module cache and macOS's loaded-bundle
 * cache. Retain all bundle resources for as long as any instance uses it. The
 * plugin info keeps the installed path, so subsequent reloads read new builds. */
inline ARDOUR::PluginPtr fresh_plugin (ARDOUR::PluginPtr const& old)
{
	namespace fs = std::filesystem;
	auto original = std::dynamic_pointer_cast<ARDOUR::VST3PluginInfo> (old->get_info ());
	if (!original) { throw std::runtime_error ("Reload requires a VST3 plugin."); }
	gchar* temp = g_dir_make_tmp ("ardour-chord-reload-XXXXXX", nullptr);
	if (!temp) { throw std::runtime_error ("Cannot create the plugin reload directory."); }
	const fs::path dir (temp); g_free (temp);
	try {
		const auto bundle = dir / fs::path (original->path).filename ();
		fs::copy (original->path, bundle, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
		auto info = std::make_shared<ARDOUR::VST3PluginInfo> (*original);
		auto module = ARDOUR::VST3PluginModule::load (bundle.string ());
		info->m = std::shared_ptr<ARDOUR::VST3PluginModule> (module.get (), [module, dir] (ARDOUR::VST3PluginModule*) mutable {
			module.reset (); std::error_code ec; fs::remove_all (dir, ec);
		});
		auto fresh = info->load (old->session ());
		if (!fresh) { throw std::runtime_error ("The new Chord Grid build could not be loaded."); }
		return fresh;
	} catch (...) {
		std::error_code ec; fs::remove_all (dir, ec); throw;
	}
}

inline std::string state_chunk (XMLNode const& plugin_state)
{
	auto chunks = plugin_state.children ("chunk");
	if (chunks.size () != 1 || chunks.front ()->children ().empty ()) {
		throw std::runtime_error ("Chord Grid did not provide a complete state snapshot.");
	}
	return chunks.front ()->children ().front ()->content ();
}

inline std::shared_ptr<ARDOUR::PluginInsert> replace (std::shared_ptr<ARDOUR::PluginInsert> const& old)
{
	using namespace ARDOUR;
	if (!supported (old)) { throw std::runtime_error ("Reload supports Chord Grid MIDI VST3."); }
	auto& session = old->plugin ()->session ();
	auto* route = dynamic_cast<Route*> (old->owner ());
	if (!route || !session.engine ().running ()) { throw std::runtime_error ("The plugin is no longer on an active track."); }
	if (session.transport_rolling () || session.actively_recording ()) { throw std::runtime_error ("Stop playback before reloading Chord Grid."); }
	std::unique_ptr<XMLNode> state (&old->get_state ());
	auto fresh = fresh_plugin (old->plugin ());
	auto replacement = std::make_shared<PluginInsert> (session, *route, fresh);
	if (replacement->set_state (*state, PBD::Stateful::current_state_version)) {
		throw std::runtime_error ("Could not restore Chord Grid's saved state. The original plugin is still in place.");
	}
	/* The VST3 adapter logs some state-load errors without returning them.
	 * Chord Grid's stable state format lets us verify restoration before swap. */
	auto saved = state->children (fresh->state_node_name ());
	std::unique_ptr<XMLNode> restored (&fresh->get_state ());
	if (saved.size () != 1 || state_chunk (*saved.front ()) != state_chunk (*restored)) {
		throw std::runtime_error ("The new build did not restore the complete chord state. The original plugin is still in place.");
	}
	if (session.transport_rolling () || session.actively_recording ()) {
		throw std::runtime_error ("Playback started during reload. Stop playback and try again.");
	}
	/* Ardour's normal replacement transaction preserves chain position and
	 * rolls back routing if the new plugin cannot support the existing I/O. */
	if (route->replace_processor (old, replacement)) {
		throw std::runtime_error ("Could not reconnect Chord Grid. The original plugin is still in place.");
	}
	session.set_dirty ();
	return replacement;
}
}
