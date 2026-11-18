/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <ytkmm/box.h>
#include <ytkmm/button.h>
#include <ytkmm/checkbutton.h>
#include <ytkmm/menu.h>
#include <ytkmm/togglebutton.h>
#include <ytkmm/comboboxtext.h>
#include <ytkmm/scrolledwindow.h>
#include "pbd/signals.h"
#include "ardour/audio_rack.h"
#include "ardour_window.h"
#include "plugin_interest.h"

class PluginSelector;
class PluginUIWindow;
class AutomationController;

/** A descriptor-aware control, also used for learned plugin parameters. */
class RackParameter : public Gtk::VBox, public PBD::ScopedConnectionList {
public:
	RackParameter (std::shared_ptr<ARDOUR::AutomationControl>, std::string const&, std::string const& kind = "", ARDOUR::ScalePoints const* choices = nullptr);
	~RackParameter ();
	void refresh ();
	sigc::signal<bool, GdkEventButton*> ContextRequested;
private:
	std::shared_ptr<ARDOUR::AutomationControl> _control;
	std::shared_ptr<AutomationController> _controller;
	Gtk::ComboBoxText* _choices = nullptr;
	Gtk::CheckButton* _toggle = nullptr;
	std::vector<double> _values;
	Gtk::Label _value;
	bool _updating = false;
};

class LearnedPluginWindow : public ArdourWindow, public PBD::ScopedConnectionList {
public:
	explicit LearnedPluginWindow (std::shared_ptr<ARDOUR::PluginInsert>);
	~LearnedPluginWindow ();
private:
	void rebuild ();
	void learn (uint32_t);
	void touched (std::weak_ptr<PBD::Controllable>);
	void save ();
	bool context (GdkEventButton*, uint32_t);
	std::shared_ptr<ARDOUR::PluginInsert> _plugin;
	Gtk::VBox _root, _parameters;
	Gtk::HBox _toolbar;
	Gtk::ToggleButton _learn;
	Gtk::ComboBoxText _chooser;
	Gtk::Button _add;
	Gtk::ScrolledWindow _scroll;
	std::vector<uint32_t> _available, _learned;
	std::unique_ptr<Gtk::Menu> _menu;
	sigc::connection _idle;
};

class AudioRackUI : public ArdourWindow, public PluginInterestedObject, public PBD::ScopedConnectionList {
public:
	explicit AudioRackUI (std::shared_ptr<ARDOUR::AudioRack>);
	~AudioRackUI ();
	bool use_plugins (SelectedPlugins const&);
private:
	void schedule_rebuild ();
	void rebuild ();
	void open_plugin (std::shared_ptr<ARDOUR::PluginInsert>, bool native);
	void touched (std::weak_ptr<PBD::Controllable>);
	void mapping_dialog (size_t);
	void learn_parameter (std::shared_ptr<ARDOUR::PluginInsert>, uint32_t);
	PBD::ScopedConnectionList _native_learn_connections;
	bool macro_context (GdkEventButton*, size_t);
	void failed (std::string const&);
	std::shared_ptr<ARDOUR::AudioRack> _rack;
	Gtk::VBox _root, _chains;
	Gtk::HBox _macros, _toolbar;
	Gtk::Button _add_chain;
	Gtk::ComboBoxText _learn_macro;
	Gtk::ToggleButton _learn;
	Gtk::Label _status;
	Gtk::ScrolledWindow _scroll;
	std::unique_ptr<PluginSelector> _selector;
	std::vector<std::unique_ptr<PluginUIWindow>> _windows;
	std::unique_ptr<Gtk::Menu> _menu;
	std::weak_ptr<ARDOUR::AudioRack::Chain> _adding_chain;
	bool _rebuilding = false;
	sigc::connection _idle;
};
