/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <ytkmm/box.h>
#include <ytkmm/button.h>
#include <ytkmm/checkbutton.h>
#include <ytkmm/menu.h>
#include <ytkmm/togglebutton.h>
#include <ytkmm/comboboxtext.h>
#include <ytkmm/scrolledwindow.h>
#include <ytkmm/table.h>
#include <ytkmm/treeview.h>
#include <ytkmm/liststore.h>
#include <ytkmm/entry.h>
#include <ytkmm/spinbutton.h>
#include "pbd/signals.h"
#include "ardour/audio_rack.h"
#include "ardour_window.h"
#include "plugin_interest.h"
#include "processor_box.h"

namespace ARDOUR { class Route; }
class PluginSelector;
class PluginUIWindow;
class AutomationController;

/** A descriptor-aware control, also used for learned plugin parameters. */
class RackParameter : public Gtk::VBox, public PBD::ScopedConnectionList {
public:
	RackParameter (std::shared_ptr<ARDOUR::AutomationControl>, std::string const&, std::string const& kind = "", ARDOUR::ScalePoints const* choices = nullptr, bool compact = false);
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

class AudioRackUI : public Gtk::VBox, public PluginInterestedObject, public PBD::ScopedConnectionList, public ProcessorEntryHost {
public:
	explicit AudioRackUI (std::shared_ptr<ARDOUR::AudioRack>);
	~AudioRackUI ();
	bool use_plugins (SelectedPlugins const&);
	static void drag_data (Gtk::SelectionData&, std::shared_ptr<ARDOUR::PluginInsert>, std::shared_ptr<ARDOUR::AudioRack> = {});
	static bool drop_plugin (ARDOUR::Session&, Gtk::SelectionData const&, ARDOUR::Route&,
	                         std::shared_ptr<ARDOUR::AudioRack>, size_t chain, size_t position,
	                         std::shared_ptr<ARDOUR::Processor> before = {});
	std::shared_ptr<ARDOUR::AudioRack> rack () const { return _rack; }
	bool focus_chains ();
	bool list_key_press (GdkEventKey*);
	sigc::signal<bool> ParentFocusRequested;
private:
	bool focus_list (Gtk::TreeView&);
	bool focus_plugins ();
	void chain_focus_changed (bool);
	void on_map () override;
	void on_unmap () override;
	void update_focus_highlight (Gtk::Widget*);
	void all_visible_processors_active (bool) override;
	void setup_routing_feeds () override;
	void update_gui_object_state (ProcessorEntry*, bool) override;
	void edit_processor (std::shared_ptr<ARDOUR::Processor>) override;
	void generic_edit_processor (std::shared_ptr<ARDOUR::Processor>) override;
	Gtk::Widget* entry_toplevel () override { return get_toplevel (); }
	bool entry_drag_data (Gtk::SelectionData&, std::shared_ptr<ARDOUR::Processor>) override;
	bool plugin_button_press (GdkEventButton*, ProcessorEntry*);
	bool plugin_button_release (GdkEventButton*, ProcessorEntry*);
	void plugin_menu (ProcessorEntry*, guint);
	void rename_plugin (std::shared_ptr<ARDOUR::PluginInsert>);
	size_t selected_plugin_index () const;
	void scroll_to_plugin (ProcessorEntry*);
	void plugin_drag_begin (Glib::RefPtr<Gdk::DragContext> const&);
	void plugin_drag_end (Glib::RefPtr<Gdk::DragContext> const&);
	bool drag_motion (Glib::RefPtr<Gdk::DragContext> const&, int, int, guint, bool chains);
	void drag_received (Glib::RefPtr<Gdk::DragContext> const&, int, int, Gtk::SelectionData const&, guint, guint, bool chains);
	bool _dragging = false, _rebuild_after_drag = false;
	Gtk::Window& parent_window ();
	void schedule_rebuild ();
	void rebuild_plugins ();
	void chain_selected ();
	void plugin_selected ();
	void update_end_cursor (bool focused);
	void update_chain ();
	void add_plugin ();
	void remove_chain ();
	void move_plugin (int);
	void remove_plugin ();
	std::shared_ptr<ARDOUR::PluginInsert> selected_plugin () const;
	size_t selected_chain_index () const;
	void rebuild ();
	void open_plugin (std::shared_ptr<ARDOUR::PluginInsert>, bool native);
	void touched (std::weak_ptr<PBD::Controllable>);
	void mapping_dialog (size_t);
	void learn_parameter (std::shared_ptr<ARDOUR::PluginInsert>, uint32_t);
	PBD::ScopedConnectionList _native_learn_connections;
	bool macro_context (GdkEventButton*, size_t);
	void failed (std::string const&);
	std::shared_ptr<ARDOUR::AudioRack> _rack;
	struct ListColumns : Gtk::TreeModel::ColumnRecord {
		ListColumns () { add (index); add (name); add (detail); }
		Gtk::TreeModelColumn<unsigned> index;
		Gtk::TreeModelColumn<Glib::ustring> name, detail;
	} _list_columns;
	Gtk::VBox _root, _macro_section, _chain_section, _plugin_section;
	Gtk::HBox _body, _toolbar, _chain_toolbar, _chain_controls, _plugin_toolbar;
	Gtk::Table _macros;
	Gtk::TreeView _chain_view;
	Gtkmm2ext::DnDVBox<ProcessorEntry> _plugin_view;
	Glib::RefPtr<Gtk::ListStore> _chain_model;
	Gtk::ScrolledWindow _chain_scroll, _plugin_scroll;
	Gtk::Button _add_chain, _remove_chain, _add_plugin, _open_plugin, _parameters, _up, _down, _remove_plugin;
	Gtk::ComboBoxText _learn_macro;
	Gtk::ToggleButton _learn;
	Gtk::Entry _chain_name;
	Gtk::SpinButton _chain_gain;
	Gtk::CheckButton _chain_mute, _chain_solo;
	Gtk::Label _status;
	std::weak_ptr<ARDOUR::AudioRack::Chain> _selected_chain;
	std::weak_ptr<ARDOUR::PluginInsert> _selected_plugin;
	std::unique_ptr<PluginSelector> _selector;
	std::vector<std::unique_ptr<PluginUIWindow>> _windows;
	std::unique_ptr<Gtk::Menu> _menu;
	std::unique_ptr<Gtk::Menu> _plugin_menu;
	std::weak_ptr<ARDOUR::AudioRack::Chain> _adding_chain;
	bool _rebuilding = false;
	sigc::connection _idle, _colors, _focus_connection;
};

/** Optional floating editor uses exactly the same controls as the device panel. */
class AudioRackWindow : public ArdourWindow {
public:
	explicit AudioRackWindow (std::shared_ptr<ARDOUR::AudioRack>);
private:
	AudioRackUI _ui;
};
