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

RackParameter::RackParameter (std::shared_ptr<AutomationControl> c, std::string const& name, std::string const& kind, ScalePoints const* points) : _control (c)
{
	set_spacing (3); set_border_width (4); set_size_request (-1, 104);
	auto title = manage (new EventBox ()); title->add (*manage (new Label (name)));
	title->signal_button_press_event ().connect ([this] (GdkEventButton* ev) { return ev->button == 3 && ContextRequested (ev); }, false);
	pack_start (*title, false, false);
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
		_controller->widget ()->set_size_request (56, d.toggled ? 28 : 56);
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

AudioRackUI::AudioRackUI (std::shared_ptr<AudioRack> rack)
	: ArdourWindow (_("Audio Rack")), _rack (rack), _add_chain (_("Add chain")), _learn (_("Learn"))
{
	add (_root); _root.set_border_width (8); _root.set_spacing (8); _toolbar.set_spacing (5);
	_root.pack_start (_macros, false, false); _root.pack_start (_toolbar, false, false); _root.pack_start (_scroll); _root.pack_end (_status, false, false);
	_toolbar.pack_start (_add_chain, false, false); _toolbar.pack_start (_learn_macro, false, false); _toolbar.pack_start (_learn, false, false);
	_toolbar.pack_start (*manage (new Label (_("Learn: touch a parameter in a plugin editor"))), false, false);
	_scroll.set_policy (POLICY_AUTOMATIC, POLICY_AUTOMATIC); _scroll.add (_chains);
	_selector.reset (new PluginSelector (PluginManager::instance ()));
	_selector->set_session (&rack->session ());
	_add_chain.signal_clicked ().connect ([this] () { if (!_rack->add_chain (string_compose (_("Chain %1"), _rack->chains ().size () + 1))) failed (_("Cannot edit a rack while recording.")); });
	for (size_t i = 0; i < 8; ++i) _learn_macro.append_text (string_compose (_("Macro %1"), i + 1));
	_learn_macro.set_active (0);
	rack->Changed.connect (*this, invalidator (*this), std::bind (&AudioRackUI::schedule_rebuild, this), gui_context ());
	Controllable::ControlTouched.connect (*this, invalidator (*this), std::bind (&AudioRackUI::touched, this, std::placeholders::_1), gui_context ());
	rack->DropReferences.connect (*this, invalidator (*this), [this] () { hide (); _selector->hide (); _windows.clear (); set_sensitive (false); }, gui_context ());
	signal_hide ().connect ([this] () { _learn.set_active (false); _selector->hide (); });
	rebuild (); set_default_size (1050, 560); show_all_children ();
}
AudioRackUI::~AudioRackUI () { _idle.disconnect (); }
void AudioRackUI::failed (std::string const& text) { _status.set_text (text); }
void AudioRackUI::schedule_rebuild () { if (!_rebuilding && !_idle.connected ()) _idle = Glib::signal_idle ().connect ([this] () { rebuild (); return false; }); }
void AudioRackUI::rebuild () {
	_rebuilding = true; _native_learn_connections.drop_connections (); clear_box (_macros); clear_box (_chains);
	for (size_t i = 0; i < 8; ++i) {
		auto const& m = _rack->macros ()[i];
		auto frame = manage (new EventBox ()); auto column = manage (new VBox ()); frame->add (*column);
		auto name = manage (new Entry ()); name->set_width_chars (10); name->set_text (m.name);
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
		auto control = manage (new RackParameter (m.control, "", m.presentation, &choices));
		control->ContextRequested.connect (sigc::bind (sigc::mem_fun (*this, &AudioRackUI::macro_context), i));
		column->pack_start (*control, false, false);
		button (*column, string_compose (_("Map (%1)…"), m.mappings.size ()), [this, i] () { mapping_dialog (i); });
		frame->signal_button_press_event ().connect (sigc::bind (sigc::mem_fun (*this, &AudioRackUI::macro_context), i), false);
		_macros.pack_start (*frame, true, true);
	}
	for (size_t i = 0; i < _rack->chains ().size (); ++i) {
		auto c = _rack->chains ()[i]; auto frame = manage (new Frame ()); auto column = manage (new VBox ()); auto row = manage (new HBox ());
		frame->add (*column); column->pack_start (*row, false, false); row->set_spacing (5); column->set_border_width (6);
		auto name = manage (new Entry ()); name->set_text (c->name); name->set_width_chars (16); row->pack_start (*name, false, false);
		auto gain = manage (new SpinButton ()); gain->set_range (-60, 12); gain->set_increments (.5, 3); gain->set_digits (1); gain->set_value (c->gain > 0 ? 20 * std::log10 (c->gain) : -60);
		row->pack_start (*gain, false, false); row->pack_start (*manage (new Label (_("dB"))), false, false);
		auto mute = manage (new CheckButton (_("Mute"))); auto solo = manage (new CheckButton (_("Solo")));
		mute->set_active (c->mute); solo->set_active (c->solo); row->pack_start (*mute, false, false); row->pack_start (*solo, false, false);
		auto update = [this, i, name, gain, mute, solo] () {
			if (_rebuilding) return;
			_rebuilding = true;
			bool ok = _rack->set_chain (i, name->get_text (), std::pow (10., gain->get_value () / 20.), mute->get_active (), solo->get_active ());
			_rebuilding = false;
			if (!ok) { failed (_("Cannot edit a rack while recording.")); schedule_rebuild (); }
		};
		name->signal_activate ().connect (update); gain->signal_value_changed ().connect (update); mute->signal_toggled ().connect (update); solo->signal_toggled ().connect (update);
		button (*row, _("Add plugin…"), [this, c] () { _adding_chain = c; _selector->set_interested_object (*this); _selector->show_all (); _selector->present (); });
		auto remove = button (*row, _("Remove chain"), [this, i, c] () {
			if (!c->plugins.empty ()) { MessageDialog confirm (*this, _("Remove this chain and its plugins?"), false, MESSAGE_QUESTION, BUTTONS_OK_CANCEL); if (confirm.run () != RESPONSE_OK) return; }
			_rack->remove_chain (i);
		}); remove->set_sensitive (_rack->chains ().size () > 1);
		if (c->plugins.empty ()) column->pack_start (*manage (new Label (_("Empty chain — dry audio"))), false, false);
		for (size_t j = 0; j < c->plugins.size (); ++j) {
			auto p = c->plugins[j]; auto pr = manage (new HBox ()); pr->set_spacing (5);
			p->plugin ()->ParameterChangedExternally.connect (_native_learn_connections, invalidator (*this), [this, p] (uint32_t id, float) { learn_parameter (p, id); }, gui_context ());
			pr->pack_start (*manage (new Label (p->name ())), true, true);
			button (*pr, _("Open"), [this, p] () { open_plugin (p, true); });
			button (*pr, _("Parameters"), [this, p] () { open_plugin (p, false); });
			button (*pr, _("Up"), [this, i, j] () { _rack->move_plugin (i, j, -1); })->set_sensitive (j > 0);
			button (*pr, _("Down"), [this, i, j] () { _rack->move_plugin (i, j, 1); })->set_sensitive (j + 1 < c->plugins.size ());
			button (*pr, _("Remove"), [this, i, j] () { _windows.clear (); _rack->remove_plugin (i, j); });
			column->pack_start (*pr, false, false);
		}
		_chains.pack_start (*frame, false, false, 4);
	}
	_macros.show_all (); _chains.show_all (); _rebuilding = false;
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
	window->set_title (p->name ()); window->set_transient_for (*this); window->show_all (); window->present (); _windows.push_back (std::move (window));
}
void AudioRackUI::touched (std::weak_ptr<Controllable> weak) {
	if (!_learn.get_active () || !get_visible ()) return;
	auto c = std::dynamic_pointer_cast<AutomationControl> (weak.lock ());
	if (!c || c->parameter ().type () != PluginAutomation) return;
	for (auto const& chain : _rack->chains ()) for (auto const& p : chain->plugins) {
		if (p->automation_control (c->parameter ()) != c) continue;
		learn_parameter (p, c->parameter ().id ()); return;
	}
}
void AudioRackUI::learn_parameter (std::shared_ptr<PluginInsert> p, uint32_t parameter) {
	if (!_learn.get_active () || !get_visible ()) return;
	int index = _learn_macro.get_active_row_number ();
	if (index >= 0 && _rack->map_parameter (index, p, parameter)) {
		auto c = p->automation_control (Evoral::Parameter (PluginAutomation, 0, parameter));
		_status.set_text (string_compose (_("Mapped %1 to %2"), c->desc ().label, _rack->macros ()[index].name)); _learn.set_active (false);
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
	Dialog dialog (_rack->macros ()[macro].name + _(" mappings"), *this, true);
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
