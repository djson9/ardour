/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <algorithm>
#include <cmath>
#include <sstream>
#include "ardour/route.h"
#include <ytkmm/checkbutton.h>
#include <ytkmm/dialog.h>
#include <ytkmm/entry.h>
#include <ytkmm/eventbox.h>
#include <ytkmm/frame.h>
#include <ytkmm/messagedialog.h>
#include <ytkmm/spinbutton.h>
#include "pbd/compose.h"
#include "pbd/xml++.h"
#include "ardour/plugin_manager.h"
#include "ardour/session.h"
#include "gtkmm2ext/doi.h"
#include "gtkmm2ext/colors.h"
#include "ui_config.h"
#include "audio_rack_ui.h"
#include "automation_controller.h"
#include "gui_thread.h"
#include "keyboard.h"
#include "plugin_selector.h"
#include "plugin_ui.h"
#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace PBD;
using namespace Gtk;

namespace {
void clear_box (Box& b) { for (auto* w : b.get_children ()) { b.remove (*w); delete w; } }
Button* button (Box& box, std::string const& text, sigc::slot<void> action) {
	auto b = manage (new Button (text)); box.pack_start (*b, false, false); b->signal_clicked ().connect (action); return b;
}
std::vector<uint32_t> parameters (std::shared_ptr<PluginInsert> p, ComboBoxText& chooser) {
	std::vector<uint32_t> result;
	chooser.clear_items ();
	for (uint32_t i = 0; i < p->plugin ()->parameter_count (); ++i) {
		if (!p->plugin ()->parameter_is_control (i) || !p->plugin ()->parameter_is_input (i)) continue;
		auto c = p->automation_control (Evoral::Parameter (PluginAutomation, 0, i));
		if (!c || c->flags () & Controllable::HiddenControl || c->desc ().upper <= c->desc ().lower) continue;
		result.push_back (i); chooser.append_text (c->desc ().label);
	}
	if (!result.empty ()) chooser.set_active (0);
	return result;
}
}

RackParameter::RackParameter (std::shared_ptr<AutomationControl> c, std::string const& name, std::string const& kind, ScalePoints const* points, bool compact) : _control (c)
{
	set_spacing (compact ? 0 : 3); set_border_width (compact ? 1 : 4); set_size_request (-1, compact ? 58 : 104);
	auto title = manage (new EventBox ()); title->add (*manage (new Label (name)));
	title->signal_button_press_event ().connect ([this] (GdkEventButton* ev) { return ev->button == 3 && ContextRequested (ev); }, false);
	if (!name.empty ()) pack_start (*title, false, false); else delete title;
	auto const& d = c->desc ();
	if (kind == "toggle") {
		_toggle = manage (new CheckButton (_("On"))); pack_start (*_toggle, false, false);
		_toggle->signal_toggled ().connect ([this] () {
			if (!_updating) _control->set_value (_toggle->get_active () ? 1 : 0, Controllable::NoGroup);
		});
		refresh ();
	} else if (kind == "choices" || (d.enumeration && d.scale_points && !d.scale_points->empty ())) {
		_choices = manage (new ComboBoxText ());
		std::vector<std::pair<double, std::string>> choices;
		if (points && !points->empty ()) { for (auto const& point : *points) choices.emplace_back (point.second, point.first); }
		else if (d.scale_points && !d.scale_points->empty ()) { for (auto const& point : *d.scale_points) choices.emplace_back (point.second, point.first); }
		else { choices = {{0, _("1")}, {1. / 3, _("2")}, {2. / 3, _("3")}, {1, _("4")}}; }
		std::sort (choices.begin (), choices.end ());
		for (auto const& point : choices) { _values.push_back (point.first); _choices->append_text (point.second); }
		pack_start (*_choices, false, false);
		_choices->signal_changed ().connect ([this] () {
			int i = _choices->get_active_row_number ();
			if (!_updating && i >= 0) {
				_control->start_touch (Temporal::timepos_t (_control->session ().audible_sample ()));
				_control->set_value (_values[i], Controllable::NoGroup);
				_control->stop_touch (Temporal::timepos_t (_control->session ().audible_sample ()));
			}
		});
		refresh ();
	} else {
		_controller = AutomationController::create (c->parameter (), d, c, !d.toggled);
		_controller->widget ()->set_size_request (compact ? 36 : 56, d.toggled ? 28 : (compact ? 36 : 56));
		pack_start (*_controller, false, false);
	}
	pack_end (_value, false, false);
	c->Changed.connect (*this, invalidator (*this), std::bind (&RackParameter::refresh, this), gui_context ()); refresh ();
	Widget* target = _controller ? _controller->widget () : (_choices ? static_cast<Widget*> (_choices) : static_cast<Widget*> (_toggle));
	if (target) target->signal_button_press_event ().connect ([this] (GdkEventButton* ev) { return ev->button == 3 && ContextRequested (ev); }, false);
}
RackParameter::~RackParameter () { if (_controller && _controller->get_parent () == this) remove (*_controller); }
void RackParameter::refresh () {
	_value.set_text (_control->get_user_string ());
	if (_toggle) { _updating = true; _toggle->set_active (_control->get_value () >= .5); _updating = false; return; }
	if (!_choices) return;
	_updating = true;
	double distance = HUGE_VAL; int nearest = 0;
	for (size_t i = 0; i < _values.size (); ++i) if (std::abs (_values[i] - _control->get_value ()) < distance) { distance = std::abs (_values[i] - _control->get_value ()); nearest = i; }
	_choices->set_active (nearest); _updating = false;
}

LearnedPluginWindow::LearnedPluginWindow (std::shared_ptr<PluginInsert> p)
	: ArdourWindow (p->name () + _(" — Learned controls")), _plugin (p), _learn (_("Learn")), _add (_("Add parameter"))
{
	_root.set_border_width (8); _root.set_spacing (6); _toolbar.set_spacing (5);
	add (_root); _root.pack_start (_toolbar, false, false); _root.pack_start (_scroll);
	_toolbar.pack_start (_learn, false, false); _toolbar.pack_start (_chooser); _toolbar.pack_start (_add, false, false);
	_root.pack_end (*manage (new Label (_("Enable Learn, then touch a control in the plugin editor. Right-click a learned control to remove it."))), false, false);
	_scroll.set_policy (POLICY_AUTOMATIC, POLICY_AUTOMATIC); _scroll.add (_parameters);
	_available = parameters (p, _chooser);
	if (auto node = p->extra_xml ("LearnedParameters")) for (auto const* child : node->children ()) {
		uint32_t id;
		if (child->get_property ("id", id) && std::find (_available.begin (), _available.end (), id) != _available.end () && std::find (_learned.begin (), _learned.end (), id) == _learned.end ()) _learned.push_back (id);
	}
	_add.signal_clicked ().connect ([this] () { int i = _chooser.get_active_row_number (); if (i >= 0) learn (_available[i]); });
	Controllable::ControlTouched.connect (*this, invalidator (*this), std::bind (&LearnedPluginWindow::touched, this, std::placeholders::_1), gui_context ());
	p->plugin ()->ParameterChangedExternally.connect (*this, invalidator (*this), [this] (uint32_t id, float) {
		if (_learn.get_active () && get_visible ()) learn (id);
	}, gui_context ());
	p->DropReferences.connect (*this, invalidator (*this), [this] () { _learn.set_active (false); hide (); set_sensitive (false); }, gui_context ());
	signal_hide ().connect ([this] () { _learn.set_active (false); });
	rebuild (); set_default_size (640, 320); show_all_children ();
}
LearnedPluginWindow::~LearnedPluginWindow () { _idle.disconnect (); }
void LearnedPluginWindow::save () {
	auto node = new XMLNode ("LearnedParameters");
	for (auto id : _learned) node->add_child ("Parameter")->set_property ("id", id);
	_plugin->add_extra_xml (*node); _plugin->session ().set_dirty ();
}
void LearnedPluginWindow::learn (uint32_t id) {
	if (std::find (_available.begin (), _available.end (), id) == _available.end () || std::find (_learned.begin (), _learned.end (), id) != _learned.end ()) return;
	_learned.push_back (id); save ();
	if (!_idle.connected ()) _idle = Glib::signal_idle ().connect ([this] () { rebuild (); return false; });
}
void LearnedPluginWindow::touched (std::weak_ptr<Controllable> weak) {
	if (!_learn.get_active () || !get_visible ()) return;
	auto c = weak.lock ();
	for (auto id : _available) if (_plugin->automation_control (Evoral::Parameter (PluginAutomation, 0, id)) == c) { learn (id); break; }
}
void LearnedPluginWindow::rebuild () {
	clear_box (_parameters);
	for (auto id : _learned) {
		auto c = _plugin->automation_control (Evoral::Parameter (PluginAutomation, 0, id));
		auto box = manage (new EventBox ()); auto control = manage (new RackParameter (c, c->desc ().label));
		control->ContextRequested.connect (sigc::bind (sigc::mem_fun (*this, &LearnedPluginWindow::context), id));
		box->add (*control); box->signal_button_press_event ().connect (sigc::bind (sigc::mem_fun (*this, &LearnedPluginWindow::context), id), false);
		_parameters.pack_start (*box, false, false);
	}
	_parameters.show_all ();
}
bool LearnedPluginWindow::context (GdkEventButton* ev, uint32_t id) {
	if (ev->button != 3) return false;
	_menu.reset (new Menu ());
	_menu->items ().push_back (Menu_Helpers::MenuElem (_("Remove learned control"), [this, id] () {
		_learned.erase (std::remove (_learned.begin (), _learned.end (), id), _learned.end ()); save ();
		if (!_idle.connected ()) _idle = Glib::signal_idle ().connect ([this] () { rebuild (); return false; });
	}));
	_menu->popup (ev->button, ev->time); return true;
}

AudioRackWindow::AudioRackWindow (std::shared_ptr<AudioRack> rack)
	: ArdourWindow (_("Audio Rack")), _ui (rack)
{
	add (_ui); set_default_size (1080, 280); show_all_children ();
}

AudioRackUI::AudioRackUI (std::shared_ptr<AudioRack> rack)
	: _rack (rack), _macros (2, 4, true), _plugin_view ({TargetEntry ("x-ardour/rack-plugin", TARGET_SAME_APP)}, Gdk::ACTION_COPY | Gdk::ACTION_MOVE), _add_chain (_("Add chain")), _remove_chain (_("Remove chain")),
	  _add_plugin (_("Add plugin…")), _open_plugin (_("Open")), _parameters (_("Parameters")),
	  _up (_("Up")), _down (_("Down")), _remove_plugin (_("Remove")), _learn (_("Learn")),
	  _chain_mute (_("Mute")), _chain_solo (_("Solo"))
{
	set_name ("AudioRackControls");
	pack_start (_root); _root.set_border_width (4); _root.set_spacing (3); _body.set_spacing (8);
	_root.pack_start (_body); _root.pack_end (_status, false, false);
	_status.set_alignment (0, .5); _status.set_no_show_all (); _status.hide ();
	_body.pack_start (_macro_section, false, false);
	_body.pack_start (_chain_section, false, false);
	_body.pack_start (_plugin_section, true, true);
	_macro_section.pack_start (_toolbar, false, false); _macro_section.pack_start (_macros);
	_toolbar.set_spacing (3); _toolbar.pack_start (*manage (new Label (_("Macros"))), false, false);
	_toolbar.pack_start (_learn_macro, true, true); _toolbar.pack_start (_learn, false, false);
	_macros.set_col_spacings (2); _macros.set_row_spacings (2);
	_chain_section.set_spacing (3); _plugin_section.set_spacing (3);
	_chain_section.pack_start (*manage (new Label (_("Chains"))), false, false);
	_chain_section.pack_start (_chain_scroll); _chain_section.pack_end (_chain_toolbar, false, false);
	_chain_toolbar.set_spacing (3); _chain_toolbar.pack_start (_add_chain); _chain_toolbar.pack_start (_remove_chain);
	_chain_scroll.set_policy (POLICY_NEVER, POLICY_AUTOMATIC); _chain_scroll.set_size_request (185, 90); _chain_scroll.add (_chain_view);
	_chain_view.set_name ("AudioRackChainList"); _chain_view.set_headers_visible (false);
	_chain_view.set_data ("audio-rack-ui", this);
	_colors = UIConfiguration::instance ().ColorsChanged.connect ([this] () { chain_focus_changed (_chain_view.is_focus ()); });
	chain_focus_changed (false);
	_chain_model = ListStore::create (_list_columns); _chain_view.set_model (_chain_model);
	_chain_view.append_column (_("Chain"), _list_columns.name); _chain_view.append_column (_("Plugins"), _list_columns.detail);
	_chain_view.get_selection ()->signal_changed ().connect (sigc::mem_fun (*this, &AudioRackUI::chain_selected));
	_plugin_section.pack_start (_chain_controls, false, false); _plugin_section.pack_start (_plugin_scroll); _plugin_section.pack_end (_plugin_toolbar, false, false);
	_chain_controls.set_spacing (3); _chain_name.set_width_chars (12); _chain_name.set_name ("AudioRackChainName");
	_chain_gain.set_range (-60, 12); _chain_gain.set_increments (.5, 3); _chain_gain.set_digits (1); _chain_gain.set_name ("AudioRackChainGain");
	_chain_controls.pack_start (_chain_name); _chain_controls.pack_start (_chain_gain, false, false);
	_chain_controls.pack_start (*manage (new Label (_("dB"))), false, false);
	_chain_controls.pack_start (_chain_mute, false, false); _chain_controls.pack_start (_chain_solo, false, false);
	_chain_name.signal_activate ().connect (sigc::mem_fun (*this, &AudioRackUI::update_chain));
	_chain_gain.signal_value_changed ().connect (sigc::mem_fun (*this, &AudioRackUI::update_chain));
	_chain_mute.signal_toggled ().connect (sigc::mem_fun (*this, &AudioRackUI::update_chain));
	_chain_solo.signal_toggled ().connect (sigc::mem_fun (*this, &AudioRackUI::update_chain));
	_plugin_scroll.set_policy (POLICY_NEVER, POLICY_AUTOMATIC); _plugin_scroll.set_size_request (380, 90); _plugin_scroll.add (_plugin_view);
	_plugin_view.set_name ("AudioRackPluginList"); _plugin_view.set_can_focus ();
	_plugin_view.set_no_show_all (true); _plugin_view.show ();
	_plugin_view.set_data ("audio-rack-ui", this);
	_plugin_view.SelectionChanged.connect (sigc::mem_fun (*this, &AudioRackUI::plugin_selected));
	_plugin_view.ButtonPress.connect (sigc::mem_fun (*this, &AudioRackUI::plugin_button_press));
	_plugin_view.ButtonRelease.connect (sigc::mem_fun (*this, &AudioRackUI::plugin_button_release));
	std::vector<TargetEntry> targets {TargetEntry ("x-ardour/rack-plugin", TARGET_SAME_APP)};
	for (bool chains : {false, true}) {
		Widget& view = chains ? static_cast<Widget&> (_chain_view) : static_cast<Widget&> (_plugin_view);
		view.drag_dest_set (targets, DEST_DEFAULT_DROP, Gdk::ACTION_COPY | Gdk::ACTION_MOVE);
		view.signal_drag_motion ().connect (sigc::bind (sigc::mem_fun (*this, &AudioRackUI::drag_motion), chains), false);
		// Handle the ID payload before DnDVBox's native pointer-based reorder handler.
		view.signal_drag_data_received ().connect (sigc::bind (sigc::mem_fun (*this, &AudioRackUI::drag_received), chains), false);
		view.signal_drag_leave ().connect ([this, chains] (Glib::RefPtr<Gdk::DragContext> const&, guint) {
			if (chains) gtk_tree_view_set_drag_dest_row (_chain_view.gobj (), nullptr, GTK_TREE_VIEW_DROP_BEFORE);
			else _plugin_view.remove_placeholder ();
		});
	}
	_plugin_view.set_tooltip_text (_("Drag plugins to reorder them, into another chain, or back to the track's plugin list."));
	_chain_view.set_tooltip_text (_("Drop an audio effect on a chain to move it there."));
	_plugin_toolbar.set_spacing (3);
	for (auto b : {&_add_plugin, &_open_plugin, &_parameters, &_up, &_down, &_remove_plugin}) _plugin_toolbar.pack_start (*b, false, false);
	_add_plugin.signal_clicked ().connect (sigc::mem_fun (*this, &AudioRackUI::add_plugin));
	_open_plugin.signal_clicked ().connect ([this] () { if (auto p = selected_plugin ()) open_plugin (p, true); });
	_parameters.signal_clicked ().connect ([this] () { if (auto p = selected_plugin ()) open_plugin (p, false); });
	_up.signal_clicked ().connect ([this] () { move_plugin (-1); }); _down.signal_clicked ().connect ([this] () { move_plugin (1); });
	_remove_plugin.signal_clicked ().connect (sigc::mem_fun (*this, &AudioRackUI::remove_plugin));
	_remove_chain.signal_clicked ().connect (sigc::mem_fun (*this, &AudioRackUI::remove_chain));
	_add_chain.signal_clicked ().connect ([this] () {
		if (_rack->add_chain (string_compose (_("Chain %1"), _rack->chains ().size () + 1))) _selected_chain = _rack->chains ().back ();
		else failed (_("Cannot edit a rack while recording."));
	});
	_selector.reset (new PluginSelector (PluginManager::instance ()));
	_selector->set_session (&rack->session ());
	for (size_t i = 0; i < 8; ++i) _learn_macro.append_text (string_compose (_("Macro %1"), i + 1));
	_learn_macro.set_active (0);
	rack->Changed.connect (*this, invalidator (*this), std::bind (&AudioRackUI::schedule_rebuild, this), gui_context ());
	Controllable::ControlTouched.connect (*this, invalidator (*this), std::bind (&AudioRackUI::touched, this, std::placeholders::_1), gui_context ());
	rack->DropReferences.connect (*this, invalidator (*this), [this] () { hide (); _selector->hide (); _windows.clear (); set_sensitive (false); }, gui_context ());
	signal_unmap ().connect ([this] () { _learn.set_active (false); _selector->hide (); });
	rebuild (); show_all_children ();
}
AudioRackUI::~AudioRackUI () { _idle.disconnect (); _colors.disconnect (); _focus_connection.disconnect (); _plugin_menu.reset (); _plugin_view.clear (); }
bool AudioRackUI::focus_list (TreeView& view) {
	if (!view.get_mapped () || !view.is_sensitive ()) return false;
	auto row = view.get_selection ()->get_selected ();
	if (!row) row = view.get_model ()->children ().begin ();
	if (row) view.set_cursor (view.get_model ()->get_path (row));
	view.grab_focus ();
	return view.is_focus ();
}
void AudioRackUI::on_map () {
	Gtk::VBox::on_map (); _focus_connection.disconnect ();
	if (auto window = dynamic_cast<Window*> (get_toplevel ())) {
		_focus_connection = window->signal_set_focus ().connect (sigc::mem_fun (*this, &AudioRackUI::update_focus_highlight));
		update_focus_highlight (window->get_focus ());
	}
}
void AudioRackUI::on_unmap () {
	_focus_connection.disconnect (); update_focus_highlight (nullptr); Gtk::VBox::on_unmap ();
}
void AudioRackUI::update_focus_highlight (Widget* focus) {
	chain_focus_changed (focus == &_chain_view);
	while (focus && focus != &_plugin_view) focus = focus->get_parent ();
	for (auto e : _plugin_view.children ()) e->set_selection_focus (focus == &_plugin_view);
}
void AudioRackUI::chain_focus_changed (bool focused) {
	auto& config = UIConfiguration::instance ();
	auto background = Gtkmm2ext::gdk_color_from_rgba (config.color (focused ? "gtk_bg_selected" : "gtk_background"));
	auto foreground = Gtkmm2ext::gdk_color_from_rgba (config.color (focused ? "gtk_fg_selected" : "gtk_foreground"));
	for (auto state : {STATE_SELECTED, STATE_ACTIVE}) { _chain_view.modify_base (state, background); _chain_view.modify_text (state, foreground); }
}
bool AudioRackUI::focus_chains () { return focus_list (_chain_view); }
bool AudioRackUI::focus_plugins () {
	if (!_plugin_view.get_mapped () || !_plugin_view.is_sensitive ()) return false;
	if (_plugin_view.selection ().empty () && !_plugin_view.children ().empty ()) _plugin_view.select (_plugin_view.children ().front ());
	_plugin_view.grab_focus ();
	return _plugin_view.is_focus ();
}
bool AudioRackUI::list_key_press (GdkEventKey* event) {
	if (!Gtkmm2ext::Keyboard::modifier_state_equals (event->state, 0)) return false;
	bool chains = _chain_view.is_focus ();
	if (!chains && !_plugin_view.is_focus ()) return false;
	switch (event->keyval) {
	case GDK_Return: case GDK_KP_Enter:
		if (chains) focus_plugins ();
		else if (auto p = selected_plugin ()) open_plugin (p, true);
		return true;
	case GDK_Delete: case GDK_BackSpace:
		if (!chains) { remove_plugin (); return true; }
		return false;
	case GDK_Escape:
		if (!chains) return focus_chains ();
		return !ParentFocusRequested.empty () && ParentFocusRequested ();
	default: break;
	}
	bool previous;
	switch (event->keyval) {
	case GDK_Up: case GDK_KP_Up: case GDK_Left: case GDK_KP_Left:
		previous = true; break;
	case GDK_Down: case GDK_KP_Down: case GDK_Right: case GDK_KP_Right:
		previous = false; break;
	default: return false;
	}
	if (!chains) {
		auto rows = _plugin_view.children ();
		if (rows.empty ()) return true;
		auto selected = _plugin_view.selection ();
		auto row = selected.empty () ? rows.end () : std::find (rows.begin (), rows.end (), selected.back ());
		if (row == rows.end ()) row = previous ? std::prev (rows.end ()) : rows.begin ();
		else if (previous && row != rows.begin ()) --row;
		else if (!previous && std::next (row) != rows.end ()) ++row;
		_plugin_view.select (*row); scroll_to_plugin (*row);
		return true;
	}
	auto& view = _chain_view;
	auto model = view.get_model ();
	auto rows = model->children ();
	if (rows.empty ()) return true;
	auto row = view.get_selection ()->get_selected ();
	if (!row) row = previous ? std::prev (rows.end ()) : rows.begin ();
	else if (previous && row != rows.begin ()) --row;
	else if (!previous && std::next (row) != rows.end ()) ++row;
	auto path = model->get_path (row);
	view.set_cursor (path);
	view.scroll_to_row (path);
	return true;
}
Window& AudioRackUI::parent_window () { return *dynamic_cast<Window*> (get_toplevel ()); }
void AudioRackUI::failed (std::string const& text) { _status.set_text (text); _status.set_visible (!text.empty ()); }
void AudioRackUI::schedule_rebuild () { if (_dragging) { _rebuild_after_drag = true; return; } if (!_rebuilding && !_idle.connected ()) _idle = Glib::signal_idle ().connect ([this] () { rebuild (); return false; }); }
size_t AudioRackUI::selected_chain_index () const {
	auto c = _selected_chain.lock ();
	return std::distance (_rack->chains ().begin (), std::find (_rack->chains ().begin (), _rack->chains ().end (), c));
}
std::shared_ptr<PluginInsert> AudioRackUI::selected_plugin () const { return _selected_plugin.lock (); }
void AudioRackUI::rebuild () {
	_rebuilding = true; _native_learn_connections.drop_connections ();
	for (auto w : _macros.get_children ()) { _macros.remove (*w); delete w; }
	for (size_t i = 0; i < 8; ++i) {
		auto const& m = _rack->macros ()[i];
		auto frame = manage (new EventBox ()); auto column = manage (new VBox ()); frame->add (*column);
		auto name = manage (new Entry ()); name->set_width_chars (8); name->set_text (m.name);
		name->signal_activate ().connect ([this, name, i] () { _rack->rename_macro (i, name->get_text ()); });
		column->pack_start (*name, false, false);
		ScalePoints choices;
		if (m.presentation == "choices" && !m.mappings.empty ()) {
			auto const& t = m.mappings.front (); auto const& d = t.control->desc ();
			if (d.scale_points && t.upper != t.lower) for (auto const& point : *d.scale_points) {
				float v = (d.to_interface (point.second) - t.lower) / (t.upper - t.lower);
				if (v >= 0 && v <= 1) choices.insert ({point.first, v});
			}
		}
		auto control = manage (new RackParameter (m.control, "", m.presentation, &choices, true));
		control->ContextRequested.connect (sigc::bind (sigc::mem_fun (*this, &AudioRackUI::macro_context), i));
		column->pack_start (*control, false, false);
		button (*column, string_compose (_("Map (%1)…"), m.mappings.size ()), [this, i] () { mapping_dialog (i); });
		frame->signal_button_press_event ().connect (sigc::bind (sigc::mem_fun (*this, &AudioRackUI::macro_context), i), false);
		_macros.attach (*frame, i % 4, i % 4 + 1, i / 4, i / 4 + 1, FILL | EXPAND, FILL);
	}
	if (selected_chain_index () >= _rack->chains ().size ()) _selected_chain = _rack->chains ().front ();
	_chain_model->clear ();
	for (size_t i = 0; i < _rack->chains ().size (); ++i) {
		auto c = _rack->chains ()[i]; auto row = _chain_model->append ();
		(*row)[_list_columns.index] = i; (*row)[_list_columns.name] = c->name;
		(*row)[_list_columns.detail] = string_compose ("%1", c->plugins.size ());
		if (c == _selected_chain.lock ()) _chain_view.get_selection ()->select (row);
		for (auto p : c->plugins) {
			// Do not keep the signal's owner alive from inside its own callback.
			// Removing the last plugin reference while disconnecting would destroy
			// that signal under its own mutex.
			std::weak_ptr<PluginInsert> weak = p;
			p->plugin ()->ParameterChangedExternally.connect (_native_learn_connections, invalidator (*this), [this, weak] (uint32_t id, float) {
				if (auto insert = weak.lock ()) learn_parameter (insert, id);
			}, gui_context ());
		}
	}
	_remove_chain.set_sensitive (_rack->chains ().size () > 1);
	_macros.show_all (); _rebuilding = false; rebuild_plugins ();
}
void AudioRackUI::chain_selected () {
	if (_rebuilding) return;
	auto row = _chain_view.get_selection ()->get_selected (); if (!row) return;
	unsigned i = (*row)[_list_columns.index]; if (i >= _rack->chains ().size ()) return;
	if (_selected_chain.lock () == _rack->chains ()[i]) return;
	_selected_chain = _rack->chains ()[i]; _selected_plugin.reset (); rebuild_plugins ();
}
void AudioRackUI::rebuild_plugins () {
	_rebuilding = true; _plugin_menu.reset (); _plugin_view.clear ();
	auto c = _selected_chain.lock ();
	if (c) {
		_chain_name.set_text (c->name); _chain_gain.set_value (c->gain > 0 ? 20 * std::log10 (c->gain) : -60);
		_chain_mute.set_active (c->mute); _chain_solo.set_active (c->solo);
		auto selected = _selected_plugin.lock ();
		if (std::find (c->plugins.begin (), c->plugins.end (), selected) == c->plugins.end ()) selected = c->plugins.empty () ? nullptr : c->plugins.front ();
		for (size_t i = 0; i < c->plugins.size (); ++i) {
			auto e = new PluginInsertProcessorEntry (this, c->plugins[i], Wide);
			e->set_position (ProcessorEntry::PreFader, i);
			e->set_selection_focus (_plugin_view.is_focus ());
			_plugin_view.add_child (e, {TargetEntry ("x-ardour/rack-plugin", TARGET_SAME_APP)});
			e->action_widget ().signal_drag_begin ().connect (sigc::mem_fun (*this, &AudioRackUI::plugin_drag_begin));
			e->action_widget ().signal_drag_end ().connect (sigc::mem_fun (*this, &AudioRackUI::plugin_drag_end));
			e->widget ().show_all (); e->set_control_state (c->plugins[i]->extra_xml ("RackProcessorRow")); e->hide_things ();
			if (c->plugins[i] == selected) _plugin_view.select (e);
		}
		setup_routing_feeds ();
	}
	_rebuilding = false; plugin_selected ();
}
size_t AudioRackUI::selected_plugin_index () const {
	auto c = _selected_chain.lock ();
	return c ? std::distance (c->plugins.begin (), std::find (c->plugins.begin (), c->plugins.end (), selected_plugin ())) : 0;
}
void AudioRackUI::plugin_selected () {
	if (_rebuilding) return;
	auto rows = _plugin_view.selection ();
	_selected_plugin = rows.empty () ? nullptr : std::dynamic_pointer_cast<PluginInsert> (rows.back ()->processor ());
	auto c = _selected_chain.lock (); auto i = selected_plugin_index ();
	bool valid = c && i < c->plugins.size ();
	_open_plugin.set_sensitive (valid); _parameters.set_sensitive (valid); _remove_plugin.set_sensitive (valid);
	_up.set_sensitive (valid && i > 0); _down.set_sensitive (valid && i + 1 < c->plugins.size ());
}
void AudioRackUI::scroll_to_plugin (ProcessorEntry* e) {
	int x, y;
	if (e->widget ().translate_coordinates (_plugin_view, 0, 0, x, y)) _plugin_scroll.get_vadjustment ()->clamp_page (y, y + e->widget ().get_height ());
}
void AudioRackUI::all_visible_processors_active (bool active) {
	if (auto c = _selected_chain.lock ()) for (auto p : c->plugins) p->enable (active);
}
void AudioRackUI::setup_routing_feeds () { ProcessorEntry::setup_routing_feeds (_plugin_view.children ()); }
void AudioRackUI::update_gui_object_state (ProcessorEntry* e, bool) {
	auto state = new XMLNode ("RackProcessorRow"); e->add_control_state (state);
	e->processor ()->add_extra_xml (*state); _rack->session ().set_dirty ();
}
void AudioRackUI::edit_processor (std::shared_ptr<Processor> p) { if (auto plugin = std::dynamic_pointer_cast<PluginInsert> (p)) open_plugin (plugin, true); }
void AudioRackUI::generic_edit_processor (std::shared_ptr<Processor> p) { if (auto plugin = std::dynamic_pointer_cast<PluginInsert> (p)) open_plugin (plugin, false); }
bool AudioRackUI::entry_drag_data (SelectionData& data, std::shared_ptr<Processor> p) {
	if (data.get_target () != "x-ardour/rack-plugin") return false;
	drag_data (data, std::dynamic_pointer_cast<PluginInsert> (p), _rack); return true;
}
bool AudioRackUI::plugin_button_press (GdkEventButton* ev, ProcessorEntry* e) {
	_plugin_view.grab_focus ();
	if (Gtkmm2ext::Keyboard::is_context_menu_event (ev)) { plugin_menu (e, ev->time); return true; }
	if (ev->button == 1 && ev->type == GDK_2BUTTON_PRESS) {
		if (!e) add_plugin ();
		else if (Gtkmm2ext::Keyboard::modifier_state_equals (ev->state, Gtkmm2ext::Keyboard::TertiaryModifier)) e->toggle_inline_display_visibility ();
		else if (Gtkmm2ext::Keyboard::modifier_state_equals (ev->state, Gtkmm2ext::Keyboard::SecondaryModifier)) generic_edit_processor (e->processor ());
		else edit_processor (e->processor ());
		return true;
	}
	return false;
}
bool AudioRackUI::plugin_button_release (GdkEventButton* ev, ProcessorEntry* e) {
	if (e && Gtkmm2ext::Keyboard::is_button2_event (ev)) e->processor ()->enable (!e->processor ()->enabled ());
	return false;
}
void AudioRackUI::plugin_menu (ProcessorEntry* e, guint time) {
	using namespace Menu_Helpers;
	_plugin_menu.reset (new Menu ()); auto& items = _plugin_menu->items ();
	items.push_back (MenuElem (_("Add plugin…"), sigc::mem_fun (*this, &AudioRackUI::add_plugin)));
	if (e) {
		auto p = std::dynamic_pointer_cast<PluginInsert> (e->processor ());
		items.push_back (MenuElem (_("Edit…"), [this, p] () { open_plugin (p, true); }));
		items.push_back (MenuElem (_("Edit with generic controls…"), [this, p] () { open_plugin (p, false); }));
		items.push_back (MenuElem (p->enabled () ? _("Bypass") : _("Enable"), [p] () { p->enable (!p->enabled ()); }));
		if (auto menu = e->build_controls_menu ()) items.push_back (MenuElem (_("Controls"), *menu));
		if (auto menu = e->build_presets_menu ()) items.push_back (MenuElem (_("Presets"), *menu));
		items.push_back (MenuElem (_("Rename…"), [this, p] () { rename_plugin (p); }));
		items.push_back (MenuElem (_("Delete"), sigc::mem_fun (*this, &AudioRackUI::remove_plugin)));
	}
	_plugin_menu->popup (3, time);
}
void AudioRackUI::rename_plugin (std::shared_ptr<PluginInsert> p) {
	Dialog dialog (_("Rename Plugin"), parent_window (), true); Entry name; name.set_text (p->name ());
	dialog.get_vbox ()->pack_start (name); dialog.add_button (_("Cancel"), RESPONSE_CANCEL); dialog.add_button (_("Rename"), RESPONSE_OK);
	name.signal_activate ().connect ([&dialog] () { dialog.response (RESPONSE_OK); }); dialog.show_all (); name.grab_focus ();
	if (dialog.run () == RESPONSE_OK && !name.get_text ().empty () && _rack->contains (p)) { p->set_name (name.get_text ()); _rack->session ().set_dirty (); }
}
void AudioRackUI::update_chain () {
	if (_rebuilding) return;
	_rebuilding = true;
	bool ok = _rack->set_chain (selected_chain_index (), _chain_name.get_text (), std::pow (10., _chain_gain.get_value () / 20.), _chain_mute.get_active (), _chain_solo.get_active ());
	_rebuilding = false;
	if (!ok) { failed (_("Cannot edit a rack while recording.")); schedule_rebuild (); }
	else if (auto row = _chain_view.get_selection ()->get_selected ()) (*row)[_list_columns.name] = _chain_name.get_text ();
}
void AudioRackUI::drag_data (SelectionData& data, std::shared_ptr<PluginInsert> plugin, std::shared_ptr<AudioRack> rack)
{
	auto route = plugin ? dynamic_cast<Route*> (plugin->owner ()) : nullptr;
	if (!route) return;
	data.set (data.get_target (), route->id ().to_s () + " " + (rack ? rack->id ().to_s () : "-") + " " + plugin->id ().to_s ());
}
bool AudioRackUI::drop_plugin (Session& session, SelectionData const& data, Route& route,
                              std::shared_ptr<AudioRack> rack, size_t chain, size_t position, std::shared_ptr<Processor> before)
{
	if (data.get_target () != "x-ardour/rack-plugin" || data.get_length () <= 0) return false;
	std::istringstream payload (data.get_data_as_string ()); std::string route_id, rack_id, plugin_id, extra;
	if (!(payload >> route_id >> rack_id >> plugin_id) || payload >> extra) return false;
	auto source_route = session.route_by_id (PBD::ID (route_id)); if (!source_route) return false;
	std::shared_ptr<AudioRack> source;
	std::shared_ptr<PluginInsert> plugin;
	if (rack_id != "-") {
		source = std::dynamic_pointer_cast<AudioRack> (source_route->processor_by_id (PBD::ID (rack_id)));
		if (!source) return false;
		for (auto const& c : source->chains ()) for (auto const& p : c->plugins) if (p->id ().to_s () == plugin_id) plugin = p;
	} else plugin = std::dynamic_pointer_cast<PluginInsert> (source_route->processor_by_id (PBD::ID (plugin_id)));
	if (!plugin || (rack && chain >= rack->chains ().size ())) return false;
	auto destination = session.route_by_id (route.id ());
	auto target_chain = rack ? rack->chains ()[chain] : nullptr;
	// Route notifications can destroy both the drag source and this panel.
	// GTK queues its drag-end cleanup at DEFAULT_IDLE after drop delivery.
	// Run after that cleanup so DnDVBox can release its source row first.
	Glib::signal_idle ().connect_once ([plugin, source, source_route, rack, destination, target_chain, position, before] () {
		size_t index = rack ? std::distance (rack->chains ().begin (), std::find (rack->chains ().begin (), rack->chains ().end (), target_chain)) : 0;
		if (source_route->session ().route_by_id (source_route->id ()) != source_route ||
		    destination->session ().route_by_id (destination->id ()) != destination ||
		    !destination->move_rack_plugin (plugin, source, rack, index, position, before)) {
			MessageDialog dialog (_("Cannot move this plugin here. Racks accept compatible audio effects without MIDI or external sidechains; moves are disabled while recording."));
			dialog.run ();
		}
	}, Glib::PRIORITY_LOW);
	return true;
}
void AudioRackUI::plugin_drag_begin (Glib::RefPtr<Gdk::DragContext> const&) { _dragging = true; }
void AudioRackUI::plugin_drag_end (Glib::RefPtr<Gdk::DragContext> const&) {
	_dragging = false;
	if (_rebuild_after_drag) { _rebuild_after_drag = false; schedule_rebuild (); }
}
bool AudioRackUI::drag_motion (Glib::RefPtr<Gdk::DragContext> const& context, int x, int y, guint time, bool chains)
{
	Widget& view = chains ? static_cast<Widget&> (_chain_view) : static_cast<Widget&> (_plugin_view);
	if (view.drag_dest_find_target (context, view.drag_dest_get_target_list ()) != "x-ardour/rack-plugin") return false;
	if (chains) {
		TreeModel::Path path; TreeViewDropPosition position;
		if (_chain_view.get_dest_row_at_pos (x, y, path, position)) _chain_view.set_drag_dest_row (path, TREE_VIEW_DROP_INTO_OR_BEFORE);
		else gtk_tree_view_set_drag_dest_row (_chain_view.gobj (), nullptr, GTK_TREE_VIEW_DROP_BEFORE);
	} else _plugin_view.add_placeholder (y);
	context->drag_status (Gdk::ACTION_COPY, time); // The receiver moves it; GTK must never delete the source.
	return true;
}
void AudioRackUI::drag_received (Glib::RefPtr<Gdk::DragContext> const& context, int x, int y, SelectionData const& data, guint, guint time, bool chains)
{
	if (!chains) g_signal_stop_emission_by_name (_plugin_view.gobj (), "drag-data-received");
	size_t chain = selected_chain_index (), index = 0;
	if (chains) {
		TreeModel::Path path; TreeViewDropPosition position;
		if (!_chain_view.get_dest_row_at_pos (x, y, path, position)) { context->drag_finish (false, false, time); return; }
		chain = path[0];
	}
	bool ok = false;
	if (chain < _rack->chains ().size ()) {
		index = _rack->chains ()[chain]->plugins.size ();
		if (!chains) { double position = _plugin_view.get_child_at_position (y).second; if (position >= 0) index = size_t (position + .5); }
		if (auto route = dynamic_cast<Route*> (_rack->owner ())) ok = drop_plugin (_rack->session (), data, *route, _rack, chain, index);
		if (ok) { _selected_chain = _rack->chains ()[chain]; failed (""); }
	}
	if (!ok) failed (_("Cannot move this plugin here. Racks accept compatible audio effects without MIDI or external sidechains; moves are disabled while recording."));
	if (chains) gtk_tree_view_set_drag_dest_row (_chain_view.gobj (), nullptr, GTK_TREE_VIEW_DROP_BEFORE);
	else _plugin_view.remove_placeholder ();
	context->drag_finish (ok, false, time);
}
void AudioRackUI::add_plugin () {
	_adding_chain = _selected_chain; failed ("");
	_selector->set_transient_for (parent_window ()); _selector->set_interested_object (*this);
	_selector->show_manager ();
}
void AudioRackUI::remove_chain () {
	auto c = _selected_chain.lock (); if (!c) return;
	if (!c->plugins.empty ()) { MessageDialog confirm (parent_window (), _("Remove this chain and its plugins?"), false, MESSAGE_QUESTION, BUTTONS_OK_CANCEL); if (confirm.run () != RESPONSE_OK) return; }
	_rack->remove_chain (selected_chain_index ());
}
void AudioRackUI::move_plugin (int direction) {
	if (selected_plugin ()) _rack->move_plugin (selected_chain_index (), selected_plugin_index (), direction);
}
void AudioRackUI::remove_plugin () {
	if (!selected_plugin ()) return;
	_windows.clear (); _rack->remove_plugin (selected_chain_index (), selected_plugin_index ());
}
bool AudioRackUI::use_plugins (SelectedPlugins const& plugins) {
	auto chain = _adding_chain.lock ();
	auto found = std::find (_rack->chains ().begin (), _rack->chains ().end (), chain);
	if (found == _rack->chains ().end ()) { failed (_("The selected chain has been removed.")); return true; }
	size_t index = std::distance (_rack->chains ().begin (), found);
	for (auto p : plugins) {
		auto insert = std::make_shared<PluginInsert> (_rack->session (), *_rack, p);
		if (!_rack->add_plugin (index, insert)) { failed (_("This rack accepts audio effects with matching input/output channels, without MIDI or external sidechains. Edits are disabled while recording.")); return true; }
	}
	return false;
}
void AudioRackUI::open_plugin (std::shared_ptr<PluginInsert> p, bool native) {
	auto window = std::unique_ptr<PluginUIWindow> (new PluginUIWindow (p, false, native));
	window->set_title (p->name ()); window->set_transient_for (parent_window ()); window->show_all (); window->present (); _windows.push_back (std::move (window));
}
void AudioRackUI::touched (std::weak_ptr<Controllable> weak) {
	if (!_learn.get_active () || !get_mapped ()) return;
	auto c = std::dynamic_pointer_cast<AutomationControl> (weak.lock ());
	if (!c || c->parameter ().type () != PluginAutomation) return;
	for (auto const& chain : _rack->chains ()) for (auto const& p : chain->plugins) {
		if (p->automation_control (c->parameter ()) != c) continue;
		learn_parameter (p, c->parameter ().id ()); return;
	}
}
void AudioRackUI::learn_parameter (std::shared_ptr<PluginInsert> p, uint32_t parameter) {
	if (!_learn.get_active () || !get_mapped ()) return;
	int index = _learn_macro.get_active_row_number ();
	if (index >= 0 && _rack->map_parameter (index, p, parameter)) {
		auto c = p->automation_control (Evoral::Parameter (PluginAutomation, 0, parameter));
		failed (string_compose (_("Mapped %1 to %2"), c->desc ().label, _rack->macros ()[index].name)); _learn.set_active (false);
	}
}
bool AudioRackUI::macro_context (GdkEventButton* ev, size_t i) {
	if (ev->button != 3) return false;
	_menu.reset (new Menu ());
	_menu->items ().push_back (Menu_Helpers::MenuElem (_("Edit mappings…"), [this, i] () { mapping_dialog (i); }));
	_menu->items ().push_back (Menu_Helpers::MenuElem (_("Control type: Knob"), [this, i] () { _rack->set_macro_presentation (i, "knob"); }));
	_menu->items ().push_back (Menu_Helpers::MenuElem (_("Control type: Toggle"), [this, i] () { _rack->set_macro_presentation (i, "toggle"); }));
	_menu->items ().push_back (Menu_Helpers::MenuElem (_("Control type: Choices"), [this, i] () { _rack->set_macro_presentation (i, "choices"); }));
	_menu->items ().push_back (Menu_Helpers::MenuElem (_("Remove all macro mappings"), [this, i] () { _rack->clear_macro (i); }));
	_menu->popup (ev->button, ev->time); return true;
}
void AudioRackUI::mapping_dialog (size_t macro) {
	Dialog dialog (_rack->macros ()[macro].name + _(" mappings"), parent_window (), true);
	dialog.add_button (_("Close"), RESPONSE_CLOSE); dialog.set_default_size (600, 300);
	auto box = dialog.get_vbox (); ComboBoxText plugins, params; SpinButton lower, upper;
	lower.set_range (0, 100); upper.set_range (0, 100); lower.set_increments (1, 10); upper.set_increments (1, 10); upper.set_value (100);
	std::vector<std::shared_ptr<PluginInsert>> inserts; std::vector<uint32_t> ids;
	for (auto const& c : _rack->chains ()) for (auto const& p : c->plugins) { inserts.push_back (p); plugins.append_text (c->name + " / " + p->name ()); }
	plugins.signal_changed ().connect ([&] () { int i = plugins.get_active_row_number (); if (i >= 0) ids = parameters (inserts[i], params); });
	box->pack_start (plugins, false, false); box->pack_start (params, false, false);
	box->pack_start (*manage (new Label (_("Target range: start / end (%) — reverse by making start larger"))), false, false);
	box->pack_start (lower, false, false); box->pack_start (upper, false, false);
	if (!inserts.empty ()) plugins.set_active (0);
	button (*box, _("Add / update mapping"), [&] () {
		int p = plugins.get_active_row_number (), id = params.get_active_row_number ();
		if (p >= 0 && id >= 0 && _rack->map_parameter (macro, inserts[p], ids[id], lower.get_value () / 100, upper.get_value () / 100)) dialog.response (RESPONSE_OK);
	});
	// Snapshot indexes remain stable while this modal dialog is open; Learn is disarmed.
	_learn.set_active (false);
	for (size_t i = 0; i < _rack->macros ()[macro].mappings.size (); ++i) {
		auto const& t = _rack->macros ()[macro].mappings[i];
		button (*box, string_compose (_("Remove: %1 (%2 → %3%)"), t.control->desc ().label, t.lower * 100, t.upper * 100), [&, i] () { _rack->unmap_parameter (macro, i); dialog.response (RESPONSE_OK); });
	}
	dialog.show_all (); dialog.run ();
}
