/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <algorithm>
#include <cmath>
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
#include "audio_rack_ui.h"
#include "automation_controller.h"
#include "gui_thread.h"
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
	: _rack (rack), _macros (2, 4, true), _add_chain (_("Add chain")), _remove_chain (_("Remove chain")),
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
	_plugin_view.set_name ("AudioRackPluginList"); _plugin_model = ListStore::create (_list_columns); _plugin_view.set_model (_plugin_model);
	_plugin_view.append_column (_("Plugins in selected chain"), _list_columns.name);
	_plugin_view.get_selection ()->signal_changed ().connect (sigc::mem_fun (*this, &AudioRackUI::plugin_selected));
	_plugin_view.signal_row_activated ().connect ([this] (TreeModel::Path const&, TreeViewColumn*) { if (auto p = selected_plugin ()) open_plugin (p, true); });
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
AudioRackUI::~AudioRackUI () { _idle.disconnect (); }
Window& AudioRackUI::parent_window () { return *dynamic_cast<Window*> (get_toplevel ()); }
void AudioRackUI::failed (std::string const& text) { _status.set_text (text); _status.set_visible (!text.empty ()); }
void AudioRackUI::schedule_rebuild () { if (!_rebuilding && !_idle.connected ()) _idle = Glib::signal_idle ().connect ([this] () { rebuild (); return false; }); }
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
	_rebuilding = true; _plugin_model->clear ();
	auto c = _selected_chain.lock ();
	if (c) {
		_chain_name.set_text (c->name); _chain_gain.set_value (c->gain > 0 ? 20 * std::log10 (c->gain) : -60);
		_chain_mute.set_active (c->mute); _chain_solo.set_active (c->solo);
		auto selected = _selected_plugin.lock ();
		if (std::find (c->plugins.begin (), c->plugins.end (), selected) == c->plugins.end ()) selected = c->plugins.empty () ? nullptr : c->plugins.front ();
		for (size_t i = 0; i < c->plugins.size (); ++i) {
			auto row = _plugin_model->append (); (*row)[_list_columns.index] = i; (*row)[_list_columns.name] = c->plugins[i]->name ();
			if (c->plugins[i] == selected) _plugin_view.get_selection ()->select (row);
		}
	}
	_rebuilding = false; plugin_selected ();
}
void AudioRackUI::plugin_selected () {
	if (_rebuilding) return;
	auto row = _plugin_view.get_selection ()->get_selected (); auto c = _selected_chain.lock ();
	unsigned i = row ? row->get_value (_list_columns.index) : 0;
	bool valid = row && c && i < c->plugins.size ();
	_selected_plugin = valid ? c->plugins[i] : std::shared_ptr<PluginInsert> ();
	_open_plugin.set_sensitive (valid); _parameters.set_sensitive (valid); _remove_plugin.set_sensitive (valid);
	_up.set_sensitive (valid && i > 0); _down.set_sensitive (valid && i + 1 < c->plugins.size ());
}
void AudioRackUI::update_chain () {
	if (_rebuilding) return;
	_rebuilding = true;
	bool ok = _rack->set_chain (selected_chain_index (), _chain_name.get_text (), std::pow (10., _chain_gain.get_value () / 20.), _chain_mute.get_active (), _chain_solo.get_active ());
	_rebuilding = false;
	if (!ok) { failed (_("Cannot edit a rack while recording.")); schedule_rebuild (); }
	else if (auto row = _chain_view.get_selection ()->get_selected ()) (*row)[_list_columns.name] = _chain_name.get_text ();
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
	auto row = _plugin_view.get_selection ()->get_selected (); if (row) _rack->move_plugin (selected_chain_index (), (*row)[_list_columns.index], direction);
}
void AudioRackUI::remove_plugin () {
	auto row = _plugin_view.get_selection ()->get_selected (); if (!row) return;
	_windows.clear (); _rack->remove_plugin (selected_chain_index (), (*row)[_list_columns.index]);
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
