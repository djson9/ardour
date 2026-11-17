/*
 * Copyright (C) 2006-2012 David Robillard <d@drobilla.net>
 * Copyright (C) 2006-2015 Tim Mayberry <mojofunk@gmail.com>
 * Copyright (C) 2006-2016 Paul Davis <paul@linuxaudiosystems.com>
 * Copyright (C) 2009-2011 Carl Hetherington <carl@carlh.net>
 * Copyright (C) 2014-2016 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <cmath>

#include <gtkmm2ext/utils.h>

#include "pbd/memento_command.h"
#include "pbd/pthread_utils.h"
#include "pbd/stateful_diff_command.h"

#include "temporal/tempo.h"

#include "ardour/audioregion.h"
#include "ardour/dB.h"
#include "ardour/region_fx_plugin.h"
#include "ardour/session_event.h"

#include "audio_region_editor.h"
#include "audio_region_view.h"
#include "editor.h"
#include "gui_thread.h"
#include "public_editor.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace PBD;
using namespace std;
using namespace Gtkmm2ext;

static void*
_peak_amplitude_thread (void* arg)
{
	static_cast<AudioRegionEditor*> (arg)->peak_amplitude_thread ();
	return 0;
}

AudioRegionEditor::AudioRegionEditor (Session* s, AudioRegionView* arv)
	: RegionEditor (s, arv->region())
	, _arv (arv)
	, _audio_region (arv->audio_region ())
	, _gain_adjustment (accurate_coefficient_to_dB (fabsf (_audio_region->scale_amplitude ())), -40.0, +40.0, 0.1, 1.0, 0)
	, _polarity_toggle (_("Polarity Invert"))
	, _fade_before_fx_toggle (_("Fade before Fx"))
	, _show_on_touch (_("Show on Touch"))
	, _peak_channel (false)
{
	_gain_label.set_name ("AudioRegionEditorLabel");
	_gain_label.set_text (_("Region gain"));
	_gain_label.set_alignment (0, 0.5);

	_peak_amplitude_label.set_name ("AudioRegionEditorLabel");
	_peak_amplitude_label.set_text (_("Peak Amplitude"));
	_peak_amplitude_label.set_alignment (1, 0.5);

	_region_line_label.set_name ("AudioRegionEditorLabel");
	_region_line_label.set_text (_("Region Line:"));
	_region_line_label.set_alignment (1, 0.5);

	_region_line.set_text_ellipsize (Pango::ELLIPSIZE_MIDDLE);
	_region_line.set_layout_ellipsize_width (PANGO_SCALE * 240);

	Gtk::Table* gain_table = manage (new Gtk::Table ());
	gain_table->set_homogeneous ();
	gain_table->set_col_spacings (12);
	gain_table->set_row_spacings (6);
	gain_table->set_border_width (0);

	_gain_entry.configure (_gain_adjustment, 0, 1);

	int row = 0;
	gain_table->attach (_gain_label, 0, 2, row, row + 1, Gtk::FILL, Gtk::FILL);
	gain_table->attach (_peak_amplitude_label, 2, 4, row, row + 1, Gtk::FILL, Gtk::FILL);
	++row;

	gain_table->show_all ();

	Gtk::HBox* b = Gtk::manage (new Gtk::HBox);
	b->set_spacing (6);
	b->pack_start (_gain_entry);
	b->pack_start (*Gtk::manage (new Gtk::Label (_("dB"))), false, false);
	gain_table->attach (*b, 0, 2, row, row + 1, Gtk::FILL | Gtk::EXPAND, Gtk::FILL);

	b = Gtk::manage (new Gtk::HBox);
	b->set_spacing (6);
	b->pack_start (_peak_amplitude);
	b->pack_start (*Gtk::manage (new Gtk::Label (_("dBFS"))), false, false);
	gain_table->attach (*b, 2, 4, row, row + 1, Gtk::FILL | Gtk::EXPAND, Gtk::FILL);
	++row;

	gain_table->attach (_polarity_toggle, 0, 2, row, row + 1, Gtk::FILL, Gtk::SHRINK);
	gain_table->attach (_fade_before_fx_toggle, 2, 4, row, row + 1, Gtk::FILL, Gtk::SHRINK);
	++row;

	/* Add to main layout */

	row = 4; /* see RegionEditor table packing for why this is 4 */
	_table_main.attach (*gain_table,        1, 2, row, row+1, Gtk::FILL, Gtk::SHRINK);
	row++;
	_table_main.attach (_region_line_label, 0, 1, row, row+1, Gtk::FILL, Gtk::SHRINK);
	_table_main.attach (_region_line,       1, 3, row, row+1, Gtk::EXPAND | Gtk::FILL, Gtk::SHRINK);
	_table_main.attach (_show_on_touch,     3, 4, row, row+1, Gtk::FILL, Gtk::SHRINK);
	row++;
	_table_main.attach (*manage (new ArdourWidgets::ArdourHSpacer (0)), 0, 3, row, row + 1, Gtk::FILL, Gtk::FILL | Gtk::EXPAND);

	UI::instance()->set_tip (_polarity_toggle, _("Invert the signal polarity (180deg phase shift)"));
	UI::instance()->set_tip (_fade_before_fx_toggle, _("Apply region effects after the region fade.\nThis is useful if the effect(s) have tail, which would otherwise be faded out by the region fade (e.g. reverb, delay)"));
	UI::instance()->set_tip (_show_on_touch, _("When touching a control in a region effect plugin UI, the corresponding region-automation line is shown the editor, and edit mode is set to 'draw'."));

	setup_warp_controls ();
	gain_changed ();
	fade_before_fx_changed ();
	refill_region_line ();
	_table_main.show_all ();
	warp_controls_changed ();
	_warp_updating = false;

	_gain_adjustment.signal_value_changed ().connect (sigc::mem_fun (*this, &AudioRegionEditor::gain_adjustment_changed));
	_polarity_toggle.signal_toggled ().connect (sigc::mem_fun (*this, &AudioRegionEditor::gain_adjustment_changed));
	_fade_before_fx_toggle.signal_toggled ().connect (sigc::mem_fun (*this, &AudioRegionEditor::fade_before_fx_toggle_changed));
	_show_on_touch.signal_toggled ().connect (sigc::mem_fun (*this, &AudioRegionEditor::show_on_touch_changed));

	_arv->region_line_changed.connect ((sigc::mem_fun (*this, &AudioRegionEditor::refill_region_line)));

	_peak_amplitude.property_editable () = false;
	_peak_amplitude.set_width_chars (9);
	_peak_amplitude.set_text (_("Analyzing"));

	PeakAmplitudeFound.connect (_peak_amplitude_connection, invalidator (*this), std::bind (&AudioRegionEditor::peak_amplitude_found, this, _1), gui_context ());

	char name[64];
	snprintf (name, 64, "peak amplitude-%p", this);
	pthread_create_and_store (name, &_peak_amplitude_thread_handle, _peak_amplitude_thread, this);
	signal_peak_thread ();
}

AudioRegionEditor::~AudioRegionEditor ()
{
	void* v;
	_warp_pending.disconnect ();
	_peak_amplitude_connection.disconnect ();
	_peak_channel.deliver ('t');
	Progress::cancel ();
	pthread_join (_peak_amplitude_thread_handle, &v);
}

void
AudioRegionEditor::region_changed (const PBD::PropertyChange& what_changed)
{
	RegionEditor::region_changed (what_changed);

	if (what_changed.contains (ARDOUR::Properties::scale_amplitude)) {
		gain_changed ();
	}

	if (what_changed.contains (ARDOUR::Properties::fade_before_fx)) {
		fade_before_fx_changed ();
	}

	if (what_changed.contains (ARDOUR::Properties::start) || what_changed.contains (ARDOUR::Properties::length)) {
		/* ask the peak thread to run again */
		signal_peak_thread ();
	}
	if (what_changed.contains (ARDOUR::Properties::region_fx_changed)) {
		refill_region_line ();
	}
}

void
AudioRegionEditor::gain_changed ()
{
	const gain_t scale_amplitude = _audio_region->scale_amplitude ();

	float const region_gain_dB = accurate_coefficient_to_dB (fabsf (scale_amplitude));
	if (region_gain_dB != _gain_adjustment.get_value ()) {
		_gain_adjustment.set_value (region_gain_dB);
	}
	_polarity_toggle.set_active (scale_amplitude < 0);
}

void
AudioRegionEditor::gain_adjustment_changed ()
{
	float gain = dB_to_coefficient (_gain_adjustment.get_value ());
	if (_polarity_toggle.get_active ()) {
		gain *= -1;
	}
	if (_audio_region->scale_amplitude () != gain) {
		_audio_region->set_scale_amplitude (gain);
	}
}

void
AudioRegionEditor::fade_before_fx_changed ()
{
	_fade_before_fx_toggle.set_active (_audio_region->fade_before_fx ());
}

void
AudioRegionEditor::fade_before_fx_toggle_changed ()
{
	_audio_region->set_fade_before_fx (_fade_before_fx_toggle.get_active ());
}

void
AudioRegionEditor::signal_peak_thread ()
{
	_peak_channel.deliver ('c');
}

void
AudioRegionEditor::peak_amplitude_thread ()
{
	while (1) {
		char msg;
		/* await instructions to run */
		_peak_channel.receive (msg);

		if (msg == 't') {
			break;
		}

		/* update thread-local tempo map */

		Temporal::TempoMap::fetch ();

		/* compute peak amplitude and signal the fact */
		PeakAmplitudeFound (accurate_coefficient_to_dB (_audio_region->maximum_amplitude (this))); /* EMIT SIGNAL */
	}
}

void
AudioRegionEditor::peak_amplitude_found (double p)
{
	stringstream s;
	s.precision (2);
	s.setf (ios::fixed, ios::floatfield);
	s << p;
	_peak_amplitude.set_text (s.str ());
}

static void
switch_tool_to_show_rfx_line ()
{
	switch (PublicEditor::instance ().current_mouse_mode ()) {
		case Editing::MouseObject:
		case Editing::MouseTimeFX:
		case Editing::MouseGrid:
		case Editing::MouseCut:
			PublicEditor::instance ().set_mouse_mode (Editing::MouseDraw, false);
			break;
		default:
			break;
	}
}

void
AudioRegionEditor::show_touched_automation (std::weak_ptr<PBD::Controllable> wac)
{
	if (!_arv->set_region_fx_line (wac)) {
		return;
	}

	switch_tool_to_show_rfx_line ();
}

void
AudioRegionEditor::show_on_touch_changed ()
{
	if (!_show_on_touch.get_active ()) {
		_ctrl_touched_connection.disconnect ();
		return;
	}
	Controllable::ControlTouched.connect (_ctrl_touched_connection, invalidator (*this), std::bind (&AudioRegionEditor::show_touched_automation, this, _1), gui_context ());
}

void
AudioRegionEditor::refill_region_line ()
{
	using namespace Gtk::Menu_Helpers;

	_region_line.clear_items ();

	MenuList& rm_items (_region_line.items ());

	int      nth = 0;
	PBD::ID  rfx_id (0);
	uint32_t param_id    = 0;
	string   active_text = _("Gain Envelope");

	_arv->get_region_fx_line (rfx_id, param_id);
	_arv->set_ignore_line_change (true);

	Gtk::RadioMenuItem::Group grp;
	AudioRegionView*          arv = _arv;

	rm_items.push_back (RadioMenuElem (grp, _("Gain Envelope")));
	Gtk::CheckMenuItem* cmi = static_cast<Gtk::CheckMenuItem*> (&rm_items.back ());
	cmi->set_active (rfx_id == 0 || param_id == UINT32_MAX);
	cmi->signal_activate ().connect ([cmi, arv] () { if (cmi->get_active ()) { arv->set_region_gain_line (); switch_tool_to_show_rfx_line ();} });

	_audio_region->foreach_plugin ([&rm_items, arv, &nth, &grp, &active_text, rfx_id, param_id] (std::weak_ptr<RegionFxPlugin> wfx) {
		std::shared_ptr<RegionFxPlugin> fx (wfx.lock ());
		if (!fx) {
			return;
		}
		std::shared_ptr<Plugin> plugin = fx->plugin ();

		if (!plugin) {
			return;
		}

		Gtk::Menu* acm = manage (new Gtk::Menu);
		MenuList&  acm_items (acm->items ());

		for (size_t i = 0; i < plugin->parameter_count (); ++i) {
			if (!plugin->parameter_is_control (i) || !plugin->parameter_is_input (i)) {
				continue;
			}
			const Evoral::Parameter param (PluginAutomation, 0, i);
			std::string             label = plugin->describe_parameter (param);
			if (label == X_("latency") || label == X_("hidden")) {
				continue;
			}
			std::shared_ptr<ARDOUR::AutomationControl> c (std::dynamic_pointer_cast<ARDOUR::AutomationControl> (fx->control (param)));
			if (c && c->flags () & (Controllable::HiddenControl | Controllable::NotAutomatable)) {
				continue;
			}
			bool active = fx->id () == rfx_id && param_id == i;

			acm_items.push_back (RadioMenuElem (grp, label));
			Gtk::CheckMenuItem* cmi = static_cast<Gtk::CheckMenuItem*> (&acm_items.back ());
			cmi->set_active (active);
			cmi->signal_activate ().connect ([cmi, arv, nth, i] () { if (cmi->get_active ()) { arv->set_region_fx_line (nth, i); switch_tool_to_show_rfx_line (); } });
			if (active) {
				active_text = fx->name () + ": " + label;
			}
		}

		if (!acm_items.empty ()) {
			rm_items.push_back (MenuElem (fx->name (), *acm));
		} else {
			delete acm;
		}
		++nth;
	});

	if (rm_items.size () > 1) {
		_show_on_touch.set_sensitive (true);
	} else {
		_show_on_touch.set_active (false);
		_show_on_touch.set_sensitive (false);
	}

	_region_line.set_text (active_text);
	_arv->set_ignore_line_change (false);
}

void
AudioRegionEditor::on_unmap ()
{
	_show_on_touch.set_active (false);
}

void
AudioRegionEditor::setup_warp_controls ()
{
 _warp_enabled.set_name ("warp-enabled");
 _warp_reverse.set_name ("warp-reverse");
 _warp_mode.set_name ("warp-mode");
 _warp_bpm.set_name ("warp-bpm");
 _warp_pitch.set_name ("warp-pitch");
 _warp_cents.set_name ("warp-cents");
 _warp_grain.set_name ("warp-grain");
 _warp_flux.set_name ("warp-flux");
 _warp_formants.set_name ("warp-formants");
 _warp_envelope.set_name ("warp-envelope");
 _warp_transient.set_name ("warp-transient-envelope");
 _warp_preserve.set_name ("warp-preserve");
 _warp_loop.set_name ("warp-loop");
 const double tempo = Temporal::TempoMap::use ()->tempo_at (_audio_region->position ()).quarter_notes_per_minute ();
 _warp_settings = ClipWarpSettings::read (*_audio_region, tempo);
 _warp_box.set_spacing (6); _warp_box.set_border_width (8);
 _warp_enabled.set_label (_("Warp")); _warp_enabled.set_active (_warp_settings.enabled);
 _warp_reverse.set_label (_("Reverse")); _warp_reverse.set_active (_warp_settings.reverse);
 const char* modes[] = {"beats", "tones", "texture", "repitch", "complex", "complex-pro"};
 const char* labels[] = {"Beats", "Tones", "Texture", "Re-Pitch", "Complex", "Complex Pro"};
 int active = 5;
 for (int i = 0; i < 6; ++i) { _warp_mode.append_text (labels[i]); if (_warp_settings.mode == modes[i]) active = i; }
 _warp_mode.set_active (active);
 auto add = [] (Gtk::HBox& box, const char* label, Gtk::Widget& control) {
  box.set_spacing (5); box.pack_start (*Gtk::manage (new Gtk::Label (label)), false, false); box.pack_start (control, false, false);
 };
 auto spin = [] (Gtk::SpinButton& button, double lo, double hi, double step, int digits, double value) {
  button.set_range (lo, hi); button.set_increments (step, step * 10); button.set_digits (digits); button.set_numeric (true); button.set_width_chars (5); button.set_value (value);
 };
 spin (_warp_bpm, 1., 999., .1, 2, _warp_settings.bpm);
 const double semitones = std::trunc (_warp_settings.pitch);
 spin (_warp_pitch, -24., 24., 1., 0, semitones);
 spin (_warp_cents, -99., 99., 1., 0, (_warp_settings.pitch - semitones) * 100.);
 spin (_warp_grain, 2., 500., 1., 1, _warp_settings.grain);
 spin (_warp_flux, 0., 100., 1., 0, _warp_settings.flux);
 spin (_warp_formants, 0., 100., 1., 0, _warp_settings.formants);
 spin (_warp_envelope, 16., 512., 1., 0, _warp_settings.envelope);
 spin (_warp_transient, 0., 100., 1., 0, _warp_settings.transient_envelope);
 Gtk::HBox* row = Gtk::manage (new Gtk::HBox);
 row->set_spacing (6); row->pack_start (_warp_enabled, false, false); row->pack_start (_warp_mode, false, false); row->pack_start (_warp_reverse, false, false);
 _warp_box.pack_start (*row, false, false);
 row = Gtk::manage (new Gtk::HBox); add (*row, _("Source BPM"), _warp_bpm);
 Gtk::Button* half = Gtk::manage (new Gtk::Button ("÷2")); Gtk::Button* twice = Gtk::manage (new Gtk::Button ("×2"));
 row->pack_start (*half, false, false); row->pack_start (*twice, false, false); _warp_box.pack_start (*row, false, false);
 half->signal_clicked ().connect ([this] () { _warp_bpm.set_value (_warp_bpm.get_value () / 2.); });
 twice->signal_clicked ().connect ([this] () { _warp_bpm.set_value (_warp_bpm.get_value () * 2.); });
 row = Gtk::manage (new Gtk::HBox); add (*row, _("Pitch"), _warp_pitch); add (*row, _("cents"), _warp_cents); _warp_box.pack_start (*row, false, false);
 add (_warp_grain_row, _("Grain ms"), _warp_grain); add (_warp_grain_row, _("Flux %"), _warp_flux); _warp_box.pack_start (_warp_grain_row, false, false);
 add (_warp_pro_row, _("Formants %"), _warp_formants); add (_warp_pro_row, _("Envelope"), _warp_envelope); _warp_box.pack_start (_warp_pro_row, false, false);
 for (const char* p : {"Transients", "1/4", "1/8", "1/16", "1/32"}) _warp_preserve.append_text (p);
 _warp_preserve.set_active (_warp_settings.preserve);
 for (const char* p : {"Off", "Forward", "Back-and-forth"}) _warp_loop.append_text (p);
 _warp_loop.set_active (_warp_settings.loop);
 add (_warp_beats_row, _("Preserve"), _warp_preserve); add (_warp_beats_row, _("Loop"), _warp_loop); add (_warp_beats_row, _("Envelope %"), _warp_transient);
 _warp_box.pack_start (_warp_beats_row, false, false);
 _warp_status.set_alignment (0, .5); _warp_status.set_line_wrap (true); _warp_status.set_size_request (260, -1); _warp_box.pack_start (_warp_status, false, false);
 _table_main.attach (_warp_box, 4, 5, 0, 7, Gtk::FILL, Gtk::FILL);
 _warp_enabled.signal_toggled ().connect (sigc::mem_fun (*this, &AudioRegionEditor::warp_controls_changed));
 _warp_reverse.signal_toggled ().connect (sigc::mem_fun (*this, &AudioRegionEditor::warp_controls_changed));
 for (auto* c : {&_warp_mode, &_warp_preserve, &_warp_loop}) c->signal_changed ().connect (sigc::mem_fun (*this, &AudioRegionEditor::warp_controls_changed));
 for (auto* c : {&_warp_bpm, &_warp_pitch, &_warp_cents, &_warp_grain, &_warp_flux, &_warp_formants, &_warp_envelope, &_warp_transient}) c->signal_value_changed ().connect (sigc::mem_fun (*this, &AudioRegionEditor::warp_controls_changed));
 Glib::signal_idle ().connect (sigc::mem_fun (*this, &AudioRegionEditor::finish_warp_setup));
}

bool
AudioRegionEditor::finish_warp_setup ()
{
 const bool was_updating = _warp_updating; _warp_updating = true;
 warp_controls_changed (); _warp_updating = was_updating; return false;
}

void
AudioRegionEditor::warp_controls_changed ()
{
 const char* modes[] = {"beats", "tones", "texture", "repitch", "complex", "complex-pro"};
 const int mode = _warp_mode.get_active_row_number ();
 if (mode < 0 || mode > 5) return;
 _warp_settings.mode = modes[mode]; _warp_settings.enabled = _warp_enabled.get_active (); _warp_settings.reverse = _warp_reverse.get_active ();
 _warp_settings.bpm = _warp_bpm.get_value (); _warp_settings.pitch = std::max (-24., std::min (24., _warp_pitch.get_value () + _warp_cents.get_value () / 100.));
 _warp_settings.grain = _warp_grain.get_value (); _warp_settings.flux = _warp_flux.get_value ();
 _warp_settings.formants = _warp_formants.get_value (); _warp_settings.envelope = _warp_envelope.get_value_as_int ();
 _warp_settings.preserve = _warp_preserve.get_active_row_number (); _warp_settings.loop = _warp_loop.get_active_row_number (); _warp_settings.transient_envelope = _warp_transient.get_value ();
 _warp_beats_row.set_visible (mode == 0); _warp_grain_row.set_visible (mode == 1 || mode == 2); _warp_flux.set_sensitive (mode == 2); _warp_pro_row.set_visible (mode == 5);
 _warp_bpm.set_sensitive (_warp_settings.enabled);
 // Re-Pitch derives pitch from playback speed; transposition is unavailable.
 _warp_pitch.set_sensitive (mode != 3); _warp_cents.set_sensitive (mode != 3);
 _warp_status.set_text (_warp_settings.available () ? _("Original engine · changes render automatically") : _("Original engine not installed for this mode"));
 if (_warp_updating) return;
 _warp_pending.disconnect ();
 const auto region = _audio_region; const auto options = _warp_settings;
 Editor* editor = dynamic_cast<Editor*> (&PublicEditor::instance ());
 _warp_pending = Glib::signal_timeout ().connect ([region, options, editor] () {
  if (editor && region->playlist ()) {
   const double tempo = Temporal::TempoMap::use ()->tempo_at (region->position ()).quarter_notes_per_minute ();
   const double stretch = options.enabled ? options.bpm / tempo : 1.;
   editor->apply_clip_warp (region, options, stretch / region->stretch ());
  }
  return false;
 }, 350);
}
