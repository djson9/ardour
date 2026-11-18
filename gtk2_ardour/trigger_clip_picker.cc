/*
 * Copyright (C) 2019-2021 Robin Gareus <robin@gareus.org>
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

#include <cassert>
#include <vector>

#include <ytkmm/stock.h>
#include <ytkmm/entry.h>

#include "pbd/basename.h"
#include "pbd/file_utils.h"
#include "pbd/natsort.h"
#include "pbd/openuri.h"
#include "pbd/pathexpand.h"
#include "pbd/search_path.h"
#include "pbd/unwind.h"

#include "ardour/audiofilesource.h"
#include "ardour/audioregion.h"
#include "ardour/auditioner.h"
#include "ardour/clip_library.h"
#include "ardour/directory_names.h"
#include "ardour/filesystem_paths.h"
#include "ardour/midi_region.h"
#include "ardour/plugin_insert.h"
#include "ardour/region_factory.h"
#include "ardour/smf_source.h"
#include "ardour/source_factory.h"
#include "ardour/srcfilesource.h"

#include "gtkmm2ext/gui_thread.h"
#include "gtkmm2ext/menu_elems.h"
#include "gtkmm2ext/utils.h"

#include "widgets/paths_dialog.h"
#include "widgets/tooltips.h"
#include "widgets/ardour_icon.h"

#include "ardour_ui.h"
#include "ardour_dialog.h"
#include "browser_file_filter.h"
#include "browser_file_search.h"
#include "browser_audio_preview.h"
#include "keyboard.h"
#include "plugin_ui.h"
#include "timers.h"
#include "trigger_clip_picker.h"
#include "ui_config.h"

#include "pbd/i18n.h"

using namespace Gtk;
using namespace PBD;
using namespace ARDOUR;

#define PX_SCALE(px) std::max((float)px, rintf((float)px * UIConfiguration::instance().get_ui_scale()))

TriggerClipPicker::TriggerClipPicker ()
	: _search (new BrowserFileSearch)
	, _fcd (_("Open Browser Folder"), FILE_CHOOSER_ACTION_SELECT_FOLDER)
	, _seek_slider (0, 1000, 1)
	, _autoplay_btn (_("Auto-play"))
	, _normalize_btn (_("Normalize"))
	, _audio_preview (new BrowserAudioPreview)
	, _auditioner_combo (InstrumentSelector::ForAuditioner)
	, _clip_library_listed (false)
	, _ignore_list_dir (false)
	, _seeking (false)
	, _audition_plugnui (0)
{
	set_name ("file-browser");
	if (!BrowserFileFilter::parse (UIConfiguration::instance ().get_browser_file_extensions (), _file_extensions)) {
		BrowserFileFilter::parse (BrowserFileFilter::defaults (), _file_extensions);
	}
	/* Setup Dropdown / File Browser */
#ifdef __APPLE__
	try {
		/* add_shortcut_folder throws an exception if the folder being added already has a shortcut */
		_fcd.add_shortcut_folder_uri ("file:///Library/GarageBand/Apple Loops");
		_fcd.add_shortcut_folder_uri ("file:///Library/Audio/Apple Loops");
		_fcd.add_shortcut_folder_uri ("file:///Library/Application Support/GarageBand/Instrument Library/Sampler/Sampler Files");
	}
	catch (Glib::Error & e) {}
#endif

	Gtkmm2ext::add_volume_shortcuts (_fcd);

	_fcd.add_button (Stock::CANCEL, RESPONSE_CANCEL);
	_fcd.add_button (Stock::ADD, RESPONSE_ACCEPT);
	_fcd.add_button (Stock::OPEN, RESPONSE_OK);

	refill_dropdown ();
	Glib::signal_idle ().connect (sigc::mem_fun (*this, &TriggerClipPicker::refill_dropdown));

	/* Audition */
	 _autoplay_btn.set_active (UIConfiguration::instance ().get_autoplay_clips ());
	_normalize_btn.set_name ("browser-normalize");
	_normalize_btn.set_active (UIConfiguration::instance ().get_normalize_browser_audio ());
	_normalize_btn.set_can_focus (false);
	_normalize_btn.set_sensitive (false);
	_normalize_btn.signal_toggled ().connect (sigc::mem_fun (*this, &TriggerClipPicker::normalize_toggled));
	_audio_preview->set_normalized (_normalize_btn.get_active ());
	_audio_preview->Ready.connect (sigc::mem_fun (*this, &TriggerClipPicker::preview_ready));
	_audio_preview->Seek.connect (sigc::mem_fun (*this, &TriggerClipPicker::preview_seek));
	ArdourWidgets::set_tooltip (_normalize_btn, _("Match preview loudness to -18 LUFS, capped at -1 dB true peak. Very short clips use peak normalization. Source files are unchanged."));
	ArdourWidgets::set_tooltip (*_audio_preview, _("Click or drag to seek in the audio preview"));
	_seek_slider.set_no_show_all ();

	_seek_slider.set_draw_value (false);

	_seek_slider.add_events (Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK);
	_seek_slider.signal_button_press_event ().connect (sigc::mem_fun (*this, &TriggerClipPicker::seek_button_press), false);
	_seek_slider.signal_button_release_event ().connect (sigc::mem_fun (*this, &TriggerClipPicker::seek_button_release), false);

	_play_btn.set_name ("generic button");
	_play_btn.set_icon (ArdourWidgets::ArdourIcon::TransportPlay);
	_play_btn.signal_clicked.connect (sigc::mem_fun (*this, &TriggerClipPicker::audition_selected));

	_stop_btn.set_name ("generic button");
	_stop_btn.set_icon (ArdourWidgets::ArdourIcon::TransportStop);
	_stop_btn.signal_clicked.connect (sigc::mem_fun (*this, &TriggerClipPicker::stop_audition));

	_gain_control.set_name("monitor section knob");
	_gain_control.set_size_request (PX_SCALE(24), PX_SCALE(24));

	_open_library_btn.set_name ("generic button");
	_open_library_btn.set_icon (ArdourWidgets::ArdourIcon::Folder);
	_open_library_btn.signal_clicked.connect (sigc::mem_fun (*this, &TriggerClipPicker::open_library));
	_open_library_btn.set_no_show_all ();

	_refresh_btn.set_name ("generic button");
	_refresh_btn.set_icon (ArdourWidgets::ArdourIcon::TransportLoop);
	_refresh_btn.signal_clicked.connect (sigc::mem_fun (*this, &TriggerClipPicker::refresh_library));
	_refresh_btn.set_name ("browser-refresh");
	_open_folder_btn.set_text (_("Open Folder…"));
	_open_folder_btn.set_name ("browser-open-folder");
	_open_folder_btn.signal_clicked.connect (sigc::mem_fun (*this, &TriggerClipPicker::open_dir));
	_file_types_btn.set_text (_("File Types…"));
	_file_types_btn.set_name ("browser-file-types");
	_file_types_btn.signal_clicked.connect (sigc::mem_fun (*this, &TriggerClipPicker::edit_file_types));
	_directory_label.set_name ("browser-directory");
	_directory_label.set_alignment (0, 0.5);
	_directory_label.set_ellipsize (Pango::ELLIPSIZE_START);
	_browser_status.set_name ("browser-status");
	_browser_status.set_alignment (0, 0.5);
	_browser_status.set_line_wrap (true);

	_show_plugin_btn.set_name ("generic button");
	_show_plugin_btn.set_icon (ArdourWidgets::ArdourIcon::PsetBrowse);
	_show_plugin_btn.signal_clicked.connect (sigc::mem_fun (*this, &TriggerClipPicker::audition_show_plugin_ui));
	_show_plugin_btn.set_sensitive (false);

	_play_btn.set_sensitive (false);
	_stop_btn.set_sensitive (false);

	_autoplay_btn.set_can_focus(false);
	_autoplay_btn.signal_toggled ().connect (sigc::mem_fun (*this, &TriggerClipPicker::autoplay_toggled));

	auditioner_combo_changed();
	_auditioner_combo.signal_changed().connect(sigc::mem_fun(*this, &TriggerClipPicker::auditioner_combo_changed) );

	ArdourWidgets::set_tooltip (_play_btn, _("Audition selected clip"));
	ArdourWidgets::set_tooltip (_stop_btn, _("Stop the audition"));
	ArdourWidgets::set_tooltip (_gain_control, _("Audition Volume"));
	ArdourWidgets::set_tooltip (_open_library_btn, _("Open clip library folder"));
	ArdourWidgets::set_tooltip (_refresh_btn, _("Refresh clip list"));
	ArdourWidgets::set_tooltip (_auditioner_combo, _("Select the Synth used for auditioning"));
	ArdourWidgets::set_tooltip (_show_plugin_btn, _("Show the GUI for the Auditioner Synth"));
	ArdourWidgets::set_tooltip (_clip_dir_menu, _("Click to select a clip folder and edit your available clip folders"));

	format_text.set_alignment(Gtk::ALIGN_START, Gtk::ALIGN_CENTER);
	channels_value.set_alignment(Gtk::ALIGN_START, Gtk::ALIGN_CENTER);
	_midi_prop_table.attach (format_text,       0, 1, 0, 1, EXPAND | FILL, SHRINK);
	_midi_prop_table.attach (channels_value,    0, 1, 1, 2, EXPAND | FILL, SHRINK);
	_midi_prop_table.attach (_auditioner_combo, 0, 3, 2, 3, EXPAND | FILL, SHRINK);
	_midi_prop_table.attach (_show_plugin_btn,  3, 4, 2, 3, SHRINK, SHRINK);
	_midi_prop_table.set_border_width (4);
	_midi_prop_table.set_spacings (4);

	/* Layout */
	int r = 0;
	_auditable.set_homogeneous(false);
	_auditable.attach (_play_btn,         0, 1, r,r+1, SHRINK, SHRINK);
	_auditable.attach (_stop_btn,         1, 2, r,r+1, SHRINK, SHRINK);
	_auditable.attach (_gain_control,     2, 3, r,r+1, SHRINK, SHRINK);
	_auditable.attach (_autoplay_btn,     3, 4, r,r+1, EXPAND | FILL, SHRINK);
	_auditable.attach (_normalize_btn,    4, 5, r,r+1, SHRINK, SHRINK); r++;
	_auditable.attach (*_audio_preview,   0, 5, r,r+1, EXPAND | FILL, SHRINK); r++;
	_auditable.attach (_seek_slider,      0, 5, r,r+1, EXPAND | FILL, SHRINK);  r++;
	_auditable.attach (_midi_prop_table,  0, 5, r,r+1, EXPAND | FILL, SHRINK);
	_auditable.set_border_width (4);
	_auditable.set_spacings (4);

	_scroller.set_policy (POLICY_AUTOMATIC, POLICY_AUTOMATIC);
	_scroller.add (_view);

	Gtk::Table *dir_table = manage(new Gtk::Table());
	dir_table->set_border_width(4);
	dir_table->set_spacings(4);
	dir_table->attach (_clip_dir_menu,    0, 1, 0, 1, EXPAND | FILL, SHRINK);
	dir_table->attach (_open_library_btn, 1, 2, 0, 1, SHRINK, SHRINK);
	dir_table->attach (_refresh_btn,      2, 3, 0, 1, SHRINK, SHRINK);
	dir_table->attach (_directory_label, 0, 3, 1, 2, FILL | EXPAND, SHRINK);
	Gtk::HBox* browser_actions = manage (new Gtk::HBox (false, 4));
	browser_actions->pack_start (_open_folder_btn, true, true);
	browser_actions->pack_start (_file_types_btn, false, false);
	dir_table->attach (*browser_actions, 0, 3, 2, 3, FILL | EXPAND, SHRINK);

	_search_entry.set_name ("browser-search");
	_search_entry.set_text ("");
	ArdourWidgets::set_tooltip (_search_entry, _("Search files in this folder and subfolders. Down selects results; Enter previews; Escape clears."));
	_search_entry.signal_changed ().connect (sigc::mem_fun (*this, &TriggerClipPicker::search_changed));
	_search_entry.signal_key_press_event ().connect (sigc::mem_fun (*this, &TriggerClipPicker::search_key), false);
	Gtk::HBox* search_row = manage (new Gtk::HBox (false, 4));
	search_row->pack_start (*manage (new Gtk::Label (_("Search"))), false, false);
	search_row->pack_start (_search_entry, true, true);
	dir_table->attach (*search_row, 0, 3, 3, 4, FILL | EXPAND, SHRINK);
	_search_poll = Glib::signal_timeout ().connect (sigc::mem_fun (*this, &TriggerClipPicker::search_poll), 40);

	pack_start (*dir_table, false, false);
	pack_start (_scroller);
	pack_start (_browser_status, false, false);
	pack_start (_auditable, false, false);

	/* TreeView */
	_model = TreeStore::create (_columns);
	_view.set_name ("browser-files");
	_view.signal_map ().connect ([this] () { _view.grab_focus (); });
	/* GTK's plain Left/Right scroll columns. In a file browser they navigate
	 * the folder hierarchy; Up/Down and Shift selection keep GTK semantics. */
	_view.signal_key_press_event ().connect ([this] (GdkEventKey* event) {
		if (!_search_entry.get_text ().empty () && event->keyval == GDK_Escape) { focus_search (); _search_entry.set_text (""); return true; }
		bool const right = event->keyval == GDK_Right || event->keyval == GDK_KP_Right;
		bool const left = event->keyval == GDK_Left || event->keyval == GDK_KP_Left;
		if ((!right && !left) || !Gtkmm2ext::Keyboard::modifier_state_equals (event->state, 0)) { return false; }
		Gtk::TreeModel::Path path;
		Gtk::TreeViewColumn* column = nullptr;
		_view.get_cursor (path, column);
		if (path.empty ()) { return true; }
		auto row = _model->get_iter (path);
		if (!row) { return true; }
		if (right && !(*row)[_columns.file]) {
			if (!_view.row_expanded (path)) {
				_view.expand_row (path, false);
			} else if (!row->children ().empty ()) {
				path.push_back (0);
				_view.set_cursor (path);
			}
		} else if (left) {
			if (_view.row_expanded (path)) {
				_view.collapse_row (path);
			} else if (path.size () > 1) {
				path.up ();
				_view.set_cursor (path);
			}
		}
		return true;
	}, false);
	_view.set_model (_model);
	_view.append_column (_("File Name"), _columns.name);
	_view.set_headers_visible (false);  //TODO: show headers when we have size/tags/etc
	_view.set_reorderable (false);
	_view.get_selection ()->set_mode (SELECTION_MULTIPLE);
	_view.signal_realize().connect (mem_fun (*this, &TriggerClipPicker::on_theme_changed));

	_view.ensure_style ();
	on_theme_changed ();

	Gtk::TreeViewColumn*   name_col = _view.get_column (0);
	Gtk::CellRendererText* renderer = dynamic_cast<Gtk::CellRendererText*> (_view.get_column_cell_renderer (0));
	name_col->add_attribute (renderer->property_foreground_gdk (), _columns.color);

	/* DnD source */
	std::vector<TargetEntry> dnd;
	dnd.push_back (TargetEntry ("text/uri-list"));
	_view.drag_source_set (dnd, Gdk::MODIFIER_MASK, Gdk::ACTION_COPY);
	_view.get_selection ()->signal_changed ().connect (sigc::mem_fun (*this, &TriggerClipPicker::row_selected));
	_view.signal_row_activated ().connect (sigc::mem_fun (*this, &TriggerClipPicker::row_activated));
	_view.signal_test_expand_row ().connect (sigc::mem_fun (*this, &TriggerClipPicker::test_expand));
	_view.signal_row_collapsed ().connect (sigc::mem_fun (*this, &TriggerClipPicker::row_collapsed));
	_view.signal_drag_data_get ().connect (sigc::mem_fun (*this, &TriggerClipPicker::drag_data_get));
	_view.signal_cursor_changed ().connect (sigc::mem_fun (*this, &TriggerClipPicker::cursor_changed));
	_view.signal_drag_end ().connect (sigc::mem_fun (*this, &TriggerClipPicker::drag_end));

	/* DnD target */
	std::vector<Gtk::TargetEntry> target_table;
	target_table.push_back (Gtk::TargetEntry ("x-ardour/region.pbdid", Gtk::TARGET_SAME_APP));
	target_table.push_back (TargetEntry ("text/uri-list"));
	_view.drag_dest_set (target_table, DEST_DEFAULT_ALL, Gdk::ACTION_COPY);
	_view.signal_drag_begin ().connect (sigc::mem_fun (*this, &TriggerClipPicker::drag_begin));
	_view.signal_drag_motion ().connect (sigc::mem_fun (*this, &TriggerClipPicker::drag_motion));
	_view.signal_drag_data_received ().connect (sigc::mem_fun (*this, &TriggerClipPicker::drag_data_received));

	UIConfiguration::instance ().ColorsChanged.connect (sigc::mem_fun (*this, &TriggerClipPicker::on_theme_changed));
	UIConfiguration::instance ().ParameterChanged.connect (sigc::mem_fun (*this, &TriggerClipPicker::parameter_changed));
	Config->ParameterChanged.connect (_config_connection, invalidator (*this), std::bind (&TriggerClipPicker::parameter_changed, this, _1), gui_context ());
	LibraryClipAdded.connect (_clip_added_connection, invalidator (*this), std::bind (&TriggerClipPicker::clip_added, this, _1, _2), gui_context ());

	/* cache value */
	_clip_library_dir = clip_library_dir ();

	/* show off */
	_scroller.show ();
	_view.show ();
	_clip_dir_menu.show ();
	_auditable.show ();

	/* fill treeview with data */
	std::string saved_directory = UIConfiguration::instance ().get_browser_directory ();
	if (!saved_directory.empty () && Glib::file_test (saved_directory, Glib::FILE_TEST_IS_DIR)) {
		list_dir (saved_directory);
	} else {
		_clip_dir_menu.items ().front ().activate ();
	}
}

TriggerClipPicker::~TriggerClipPicker ()
{
	_idle_connection.disconnect ();
	_search_debounce.disconnect ();
	_search_poll.disconnect ();
	_search.reset ();
}

void
TriggerClipPicker::focus_search ()
{
 _search_entry.grab_focus ();
 _search_entry.select_region (0, -1);
}

bool
TriggerClipPicker::search_key (GdkEventKey* event)
{
 if (event->keyval == GDK_Escape) {
  if (!_search_entry.get_text ().empty ()) _search_entry.set_text ("");
  else _view.grab_focus ();
  return true;
 }
 if (event->keyval == GDK_Down || event->keyval == GDK_KP_Down || event->keyval == GDK_Return || event->keyval == GDK_KP_Enter) {
  if (!_model->children ().empty ()) {
   auto row = _model->children ().begin ();
   if (event->keyval == GDK_Return || event->keyval == GDK_KP_Enter) row_activated (_model->get_path (row), nullptr);
   else { _view.grab_focus (); _view.set_cursor (_model->get_path (row)); }
  }
  return true;
 }
 return false;
}

void
TriggerClipPicker::search_changed ()
{
 if (_search_updating) return;
 _search_debounce.disconnect ();
 _search->cancel ();
 stop_audition ();
 if (_search_entry.get_text ().empty ()) { refresh_library (); return; }
 _model->clear ();
 _browser_status.set_text (_("Searching…"));
 _browser_status.show ();
 _search_debounce = Glib::signal_timeout ().connect ([this] {
  _search->request (_current_path, _search_entry.get_text (), _file_extensions);
  return false;
 }, 150);
}

bool
TriggerClipPicker::search_poll ()
{
 auto result = _search->take ();
 if (!result || _search_entry.get_text ().empty ()) return true;
 PBD::Unwinder<bool> updating (_search_updating, true);
 _model->clear ();
 for (auto const& file : result->files) {
  auto row = *(_model->append ());
  row[_columns.name] = file.relative;
  row[_columns.path] = Glib::build_filename (_current_path, file.relative);
  row[_columns.file] = true;
  row[_columns.read] = true;
  row[_columns.color] = _color_foreground;
 }
 _browser_status.set_text (result->limited ? _("Search limit reached. Refine your search.") :
  result->unreadable ? _("Some folders could not be read.") :
  result->files.empty () ? _("No matching files.") : string_compose (_("%1 matching files"), result->files.size ()));
 if (!_model->children ().empty ()) _view.set_cursor (_model->get_path (_model->children ().begin ()));
 return true;
}

void
TriggerClipPicker::auditioner_combo_changed()
{
	if (_session) {
		_session->the_auditioner()->set_audition_synth_info( _auditioner_combo.selected_instrument() );
	}
}


void
TriggerClipPicker::parameter_changed (std::string const& p)
{
	if (p == "sample-lib-path") {
		refill_dropdown ();
	} else if (p == "clip-library-dir") {
		_clip_library_dir = clip_library_dir ();
		refill_dropdown ();
	} else if (p == "highlight-auditioned-clips") {
		reset_audition_marks (true);
	} else if (p == "browser-file-extensions") {
		if (BrowserFileFilter::parse (UIConfiguration::instance ().get_browser_file_extensions (), _file_extensions)) {
			refresh_library ();
		}
	} else if (p == "normalize-browser-audio") {
		_normalize_btn.set_active (UIConfiguration::instance ().get_normalize_browser_audio ());
		_audio_preview->set_normalized (_normalize_btn.get_active ());
	}
}

void
TriggerClipPicker::edit_file_types ()
{
	ArdourDialog dialog (_("Browser File Types"), true);
	if (auto* parent = dynamic_cast<Gtk::Window*> (get_toplevel ())) { dialog.set_transient_for (*parent); }
	Gtk::Label help (_("Show files with these extensions (comma or space separated).\nFolders are always shown. An empty list hides all files.\nAdding an extension does not add support for its file format."));
	help.set_alignment (0, 0.5);
	Gtk::Entry entry;
	entry.set_name ("browser-extension-entry");
	entry.set_width_chars (58);
	entry.set_text (UIConfiguration::instance ().get_browser_file_extensions ());
	entry.set_activates_default (true);
	Gtk::Label error;
	error.set_name ("browser-filter-error");
	dialog.get_vbox ()->set_border_width (12);
	dialog.get_vbox ()->set_spacing (8);
	dialog.get_vbox ()->pack_start (help, false, false);
	dialog.get_vbox ()->pack_start (entry, false, false);
	dialog.get_vbox ()->pack_start (error, false, false);
	dialog.add_button (_("Reset to Audio + MIDI"), RESPONSE_APPLY);
	dialog.add_button (Stock::CANCEL, RESPONSE_CANCEL);
	dialog.add_button (Stock::APPLY, RESPONSE_OK);
	dialog.set_default_response (RESPONSE_OK);
	dialog.show_all ();
	entry.grab_focus ();
	for (;;) {
		int response = dialog.run ();
		if (response == RESPONSE_APPLY) {
			entry.set_text (BrowserFileFilter::defaults ());
			error.set_text ("");
			continue;
		}
		if (response != RESPONSE_OK) { break; }
		std::set<std::string> extensions;
		if (!BrowserFileFilter::parse (entry.get_text (), extensions)) {
			error.set_text (_("Use extensions such as wav, .flac or *.mid; no paths or wildcards."));
			continue;
		}
		UIConfiguration::instance ().set_browser_file_extensions (BrowserFileFilter::serialize (extensions));
		UIConfiguration::instance ().save_state ();
		break;
	}
	dialog.hide ();
	_view.grab_focus ();
}

void
TriggerClipPicker::clip_added (std::string const& path, void* src)
{
	if (!_clip_library_listed) {
		_clip_library_dir = clip_library_dir ();
		refill_dropdown ();
	}
	if (src == this) {
		list_dir (clip_library_dir ());
	} else {
		list_dir (Glib::path_get_dirname (path));
	}
}

/* ****************************************************************************
 * Paths Dropdown Callbacks
 */

void
TriggerClipPicker::edit_path ()
{
	Gtk::Window* tlw = dynamic_cast<Gtk::Window*> (get_toplevel ());
	assert (tlw);
	ArdourWidgets::PathsDialog pd (*tlw, _("Edit Sample Library Path"), Config->get_sample_lib_path (), "");
	if (pd.run () != Gtk::RESPONSE_ACCEPT) {
		return;
	}
	Config->set_sample_lib_path (pd.get_serialized_paths ());
}

bool
TriggerClipPicker::refill_dropdown ()
{
	_clip_dir_menu.clear_items ();
	_root_paths.clear ();

	/* Bundled Content */
	Searchpath spath (ardour_data_search_path ());
	spath.add_subdirectory_to_paths (media_dir_name);
	for (auto const& f : spath) {
		maybe_add_dir (f);
	}

	/* User config folder */
	maybe_add_dir (Glib::build_filename (user_config_directory (), media_dir_name));

	/* Freesuund dir, in case they use it */
	maybe_add_dir (UIConfiguration::instance().get_freesound_dir());

	/* Anything added by Gtkmm2ext::add_volume_shortcuts */
	for (auto const& f : _fcd.list_shortcut_folders ()) {
		maybe_add_dir (f);
	}

	/* Custom Paths */
	assert (_clip_dir_menu.items ().size () > 0);
	if (!Config->get_sample_lib_path ().empty ()) {
		_clip_dir_menu.add_menu_elem (Menu_Helpers::SeparatorElem ());
		Searchpath cpath (Config->get_sample_lib_path ());
		for (auto const& f : cpath) {
			maybe_add_dir (f);
		}
	}

	_clip_library_listed = maybe_add_dir (clip_library_dir (false));

	_clip_dir_menu.add_menu_elem (Menu_Helpers::SeparatorElem ());
	_clip_dir_menu.add_menu_elem (Menu_Helpers::MenuElem (_("Edit..."), sigc::mem_fun (*this, &TriggerClipPicker::edit_path)));
	_clip_dir_menu.add_menu_elem (Menu_Helpers::MenuElem (_("Other..."), sigc::mem_fun (*this, &TriggerClipPicker::open_dir)));
	_clip_dir_menu.add_menu_elem (Menu_Helpers::MenuElem (_("Download..."), sigc::mem_fun (*this, &TriggerClipPicker::open_downloader)));
	return false;
}

static bool
is_subfolder (std::string const& parent, std::string dir)
{
	assert (Glib::file_test (dir, Glib::FILE_TEST_IS_DIR | Glib::FILE_TEST_EXISTS));
	assert (Glib::file_test (parent, Glib::FILE_TEST_IS_DIR | Glib::FILE_TEST_EXISTS));

	if (parent.size () > dir.size ()) {
		return false;
	}
	if (parent == dir) {
		return false;
	}
	if (dir == Glib::path_get_dirname (dir)) {
		/* dir must be root */
		return false;
	}
	while (parent.size () < dir.size ()) {
		/* step up, compare with parent */
		dir = Glib::path_get_dirname (dir);
		if (parent == dir) {
			return true;
		}
	}
	return false;
}

static std::string
display_name (std::string const& dir) {
	std::string metadata = Glib::build_filename (dir, ".daw-meta.xml");
	if (Glib::file_test (metadata, Glib::FILE_TEST_IS_REGULAR | Glib::FILE_TEST_EXISTS)) {
		XMLTree tree;
		if (tree.read (metadata) && tree.root()->name () == "DAWDirectory") {
			XMLNode* root = tree.root();
			std::string type;
			if (root->get_property ("type", type)) {
				if (type == "bundled") {
					return string_compose (_("%1 Bundled Content"), PROGRAM_NAME);
				}
			}
#if ENABLE_NLS
			if (ARDOUR::translations_are_enabled ()) {
				for (XMLNodeList::const_iterator n = root->children ("title").begin (); n != root->children ("title").end (); ++n) {
					std::string lang;
					if (!(*n)->get_property ("lang", lang)) {
						continue;
					}
					if (lang != "en_US") { // TODO: get current lang
						continue;
					}
					return (*n)->child_content ();
				}
			}
#endif
			/* pick first title, if any */
			XMLNode* child = root->child ("title");
			if (child) {
				return child->child_content ();
			}
		}
	}
	return Glib::path_get_basename (dir);
}

bool
TriggerClipPicker::maybe_add_dir (std::string const& dir)
{
	if (dir.empty () || dir == "." || !Glib::file_test (dir, Glib::FILE_TEST_IS_DIR | Glib::FILE_TEST_EXISTS)) {
		return false;
	}

	_clip_dir_menu.add_menu_elem (Gtkmm2ext::MenuElemNoMnemonic (display_name (dir), sigc::bind (sigc::mem_fun (*this, &TriggerClipPicker::list_dir), dir, (Gtk::TreeNodeChildren*)0)));

	/* check if a parent path of the given dir already exists,
	 * or if this new path is parent to any existing ones.
	 */
	bool insert = true;
	auto it = _root_paths.begin ();
	while (it != _root_paths.end ()) {
		bool erase = false;
		if (it->size () > dir.size()) {
			if (is_subfolder (dir, *it)) {
				erase = true;
			}
		} else if (is_subfolder (*it, dir)) {
			insert = false;
			break;
		}
		if (erase) {
			auto it2 = it;
			++it;
			_root_paths.erase (it2);
		} else {
			++it;
		}
	}
	if (insert) {
		_root_paths.insert (dir);
	}
	return true;
}

/* ****************************************************************************
 * Treeview Callbacks
 */

void
TriggerClipPicker::drag_begin (Glib::RefPtr<Gdk::DragContext> const& context)
{
	TreeView::Selection::ListHandle_Path rows = _view.get_selection ()->get_selected_rows ();
	if (!rows.empty()) {
		Glib::RefPtr< Gdk::Pixmap > pix = _view.create_row_drag_icon (*rows.begin ());

		int w, h;
		pix->get_size (w, h);
		context->set_icon (pix->get_colormap (), pix, Glib::RefPtr<Gdk::Bitmap> (), 4, h / 2);
	}
}

void
TriggerClipPicker::drag_end (Glib::RefPtr<Gdk::DragContext> const&)
{
	stop_audition ();
}

void
TriggerClipPicker::cursor_changed ()
{
	if (!_session || _search_updating) {
		return;
	}

	TreeModel::Path p;
	TreeViewColumn* col = NULL;
	_view.get_cursor (p, col);
	TreeModel::iterator i = _model->get_iter (p);
	preview_file (i && (*i)[_columns.file] ? std::string ((*i)[_columns.path]) : "");
	if (!_autoplay_btn.get_active ()) { return; }
	_pending_audition.clear ();
	_preview_start = 0;
	_session->cancel_audition ();
	/* This also plays the file if the cursor change deselects the row.
	 * However, checking if `i` is _view.get_selection () does not reliably work from this context.
	 */
	if (i && (*i)[_columns.file]) {
		mark_auditioned (i);
		audition ((*i)[_columns.path]);
	}
}

void
TriggerClipPicker::row_selected ()
{
	if (!_session) {
		return;
	}

	if (!_autoplay_btn.get_active ()) {
		_pending_audition.clear ();
		_session->cancel_audition ();
	}

	if (_view.get_selection ()->count_selected_rows () < 1) {
		preview_file ("");
		_play_btn.set_sensitive (false);
		_midi_prop_table.hide();
	} else {
		TreeView::Selection::ListHandle_Path rows = _view.get_selection ()->get_selected_rows ();
		TreeIter                             i    = _model->get_iter (*rows.begin ());

		_play_btn.set_sensitive ((*i)[_columns.file] && !_autoplay_btn.get_active ());

		std::string path = (*i)[_columns.path];
		preview_file ((*i)[_columns.file] ? path : "");
		if (SMFSource::valid_midi_file (path)) {
			/* TODO: if it's a really big file, we could skip this check */
			std::shared_ptr<SMFSource> ms;
			try {
				ms = std::dynamic_pointer_cast<SMFSource> (SourceFactory::createExternal (DataType::MIDI, *_session, path, 0, Source::Flag (0), false));
			} catch (const std::exception& e) {
				error << string_compose(_("Could not read file: %1 (%2)."), path, e.what()) << endmsg;
			}

			if (ms) {
				if (ms->smf_format()==0) {
					format_text.set_text ("MIDI Type 0");
				} else {
					format_text.set_text (string_compose( _("%1 (%2 Tracks, only the first track will be used)"), ms->smf_format()==2 ? X_("MIDI Type 2") : X_("MIDI Type 1"), ms->num_tracks()));
				}
				channels_value.set_text (string_compose(
				    _("%1 notes on channel: %2%3 "),
					ms->n_note_on_events(),
					ARDOUR_UI_UTILS::midi_channels_as_string (ms->used_channels()),
					ms->has_pgm_change() ? _(", with pgms") : X_("")
					));

				_midi_prop_table.show();
			}
		} else {
			_midi_prop_table.hide();
		}
	}
}

void
TriggerClipPicker::row_activated (TreeModel::Path const& p, TreeViewColumn*)
{
	TreeModel::iterator i = _model->get_iter (p);
	if (i && (*i)[_columns.file]) {
		mark_auditioned (i);
		audition ((*i)[_columns.path]);
	} else if (i) {
		list_dir ((*i)[_columns.path]);
	}
}

bool
TriggerClipPicker::test_expand (TreeModel::iterator const& i, Gtk::TreeModel::Path const&)
{
	TreeModel::Row row = *i;
	if (row[_columns.read]) {
		/* already expanded */
		return false; /* OK */
	}
	row[_columns.read] = true;

	/* remove stub */
	_model->erase (row.children ().begin ());

	list_dir (row[_columns.path], &row.children ());

	return row.children ().size () == 0;
}

void
TriggerClipPicker::row_collapsed (TreeModel::iterator const& i, Gtk::TreeModel::Path const&)
{
	/* forget about expanded sub-view, refresh when expanded again */
	TreeModel::Row row = *i;
	row[_columns.read] = false;
	Gtk::TreeIter ti;
	while ((ti = row.children ().begin ()) != row.children ().end ()) {
		_model->erase (ti);
	}
	/* add stub child */
	row                = *(_model->append (row.children ()));
	row[_columns.read] = false;
}

void
TriggerClipPicker::drag_data_get (Glib::RefPtr<Gdk::DragContext> const&, SelectionData& data, guint, guint time)
{
	if (data.get_target () != "text/uri-list") {
		return;
	}
	std::vector<std::string>             uris;
	TreeView::Selection::ListHandle_Path rows = _view.get_selection ()->get_selected_rows ();
	for (TreeView::Selection::ListHandle_Path::iterator i = rows.begin (); i != rows.end (); ++i) {
		TreeIter iter;
		if ((iter = _model->get_iter (*i))) {
			if ((*iter)[_columns.file]) {
				uris.push_back (Glib::filename_to_uri ((*iter)[_columns.path]));
			}
		}
	}
	data.set_uris (uris);
	reset_audition_marks ();
}

bool
TriggerClipPicker::drag_motion (Glib::RefPtr<Gdk::DragContext> const& context, int, int, guint time)
{
	for (auto i : context->get_targets ()) {
		if (i == "x-ardour/region.pbdid") {
			/* prepare for export to local clip library */
			if (!_clip_library_dir.empty () && _current_path != _clip_library_dir) {
				list_dir (_clip_library_dir);
			}
			context->drag_status (Gdk::ACTION_COPY, time);
			return true;
		}
	}

	/* drag from clip-picker (to slots), or
	 * drag of an external folder to the clip-picker (add to sample_lib_path)
	 */
	context->drag_status (Gdk::ACTION_LINK, time);
	return true;
}

void
TriggerClipPicker::drag_data_received (Glib::RefPtr<Gdk::DragContext> const& context, int /*x*/, int y, Gtk::SelectionData const& data, guint /*info*/, guint time)
{
	if (data.get_target () == "x-ardour/region.pbdid") {
		PBD::ID rid (data.get_data_as_string ());
		std::shared_ptr<Region> region = RegionFactory::region_by_id (rid);
		if (export_to_clip_library (region, this)) {
			context->drag_finish (true, false, time);
		} else {
			context->drag_finish (true, false, time);
		}
	} else {
		bool                     changed = false;
		std::string              path;
		std::string              path_to_list;
		std::vector<std::string> paths;

		std::vector<std::string> a = PBD::parse_path (Config->get_sample_lib_path ());
		if (ARDOUR_UI_UTILS::convert_drop_to_paths (paths, data)) {
			for (std::vector<std::string>::const_iterator s = paths.begin (); s != paths.end (); ++s) {
				if (Glib::file_test (*s, Glib::FILE_TEST_IS_DIR)) {
					if (std::find (a.begin(), a.end(), *s) == a.end()) {
						a.push_back (*s);
						changed = true;
					}
					path_to_list = *s;
				}
			}
			if (changed) {
				size_t j = 0;
				for (std::vector<std::string>::const_iterator i = a.begin (); i != a.end (); ++i, ++j) {
					if (j > 0)
						path += G_SEARCHPATH_SEPARATOR;
					path += *i;
				}
				Config->set_sample_lib_path (path);
			}
			if (!path_to_list.empty ()) {
				list_dir (path_to_list);
			}
		}
	}
}

/* ****************************************************************************
 * Dir Listing
 */

void
TriggerClipPicker::open_downloader ()
{
	ARDOUR_UI::instance()->show_library_download_window ();
}

void
TriggerClipPicker::open_dir ()
{
	Gtk::Window* tlw = dynamic_cast<Gtk::Window*> (get_toplevel ());
	assert (tlw);
#ifndef __APPLE__
	if (tlw) {
		_fcd.set_transient_for (*tlw);
	}
#endif

	if (!_current_path.empty () && Glib::file_test (_current_path, Glib::FILE_TEST_IS_DIR)) {
		_fcd.set_current_folder (_current_path);
	}
	int result = _fcd.run ();
	_fcd.hide ();

	switch (result) {
		case RESPONSE_OK:
			list_dir (_fcd.get_filename ());
			break;
		case RESPONSE_ACCEPT:
			if (Glib::file_test (_fcd.get_filename (), Glib::FILE_TEST_IS_DIR)) {
				size_t                   j = 0;
				std::string              path;
				std::vector<std::string> a = PBD::parse_path (Config->get_sample_lib_path ());
				if (std::find (a.begin(), a.end(), _fcd.get_filename ()) != a.end()) {
					list_dir (_fcd.get_filename ());
					break;
				}
				a.push_back (_fcd.get_filename ());
				for (std::vector<std::string>::const_iterator i = a.begin (); i != a.end (); ++i, ++j) {
					if (j > 0)
						path += G_SEARCHPATH_SEPARATOR;
					path += *i;
				}
				Config->set_sample_lib_path (path);
				list_dir (_fcd.get_filename ());
			}
			break;
		default:
			break;
	}
	_view.grab_focus ();
}

void
TriggerClipPicker::list_dir (std::string const& path, Gtk::TreeNodeChildren const* pc)
{
	if (_ignore_list_dir) {
		return;
	}
	/* do not recurse when calling _clip_dir_menu.set_active() */
	PBD::Unwinder<bool> uw (_ignore_list_dir, true);

	if (!Glib::file_test (path, Glib::FILE_TEST_IS_DIR)) {
		_browser_status.set_text (_("Folder is unavailable. Choose another folder."));
		_browser_status.show ();
		return;
	}

	if (!pc && !_search_entry.get_text ().empty ()) {
		if (_current_path == path) { search_changed (); return; }
		PBD::Unwinder<bool> updating (_search_updating, true);
		_search_entry.set_text ("");
		_search_debounce.disconnect ();
		_search->cancel ();
	}

	if (!pc) {
		_view.set_model (Glib::RefPtr<Gtk::TreeStore>(0));
		_model->clear ();
		_clip_dir_menu.set_active (display_name (path));
		bool const navigated = _current_path != path;
		_current_path = path;
		_directory_label.set_text (path);
		ArdourWidgets::set_tooltip (_directory_label, path);
		if (navigated) {
			UIConfiguration::instance ().set_browser_directory (path);
			UIConfiguration::instance ().save_state ();
		}
		if (_clip_library_dir == path) { _open_library_btn.show (); }
		else { _open_library_btn.hide (); }
		_browser_status.set_text ("");
	}

	std::vector<std::string> dirs;
	std::vector<std::string> files;

	try {
		Glib::Dir dir (path);
		for (Glib::DirIterator i = dir.begin (); i != dir.end (); ++i) {
			std::string fullpath = Glib::build_filename (path, *i);
			std::string basename = *i;

			if (basename.size () == 0 || basename[0] == '.') {
				continue;
			}

			if (Glib::file_test (fullpath, Glib::FILE_TEST_IS_DIR)) {
				dirs.push_back (*i);
				continue;
			}

			if (Glib::file_test (fullpath, Glib::FILE_TEST_IS_REGULAR) && BrowserFileFilter::matches (basename, _file_extensions)) {
				files.push_back (*i);
			}
		}
	} catch (Glib::FileError const& err) {
		_browser_status.set_text (_("Could not read folder. Check that it is accessible."));
	}

	std::sort (dirs.begin (), dirs.end (), [](const std::string a, const std::string b) {
		return naturally_less (a, b);
	});
	std::sort (files.begin (), files.end (), [](const std::string a, const std::string b) {
		return naturally_less (a, b);
	});

	if (!pc) {
		if (_root_paths.find (_current_path) == _root_paths.end ()) {
			TreeModel::Row row  = *(_model->append ());
			row[_columns.name]  = "..";
			row[_columns.path]  = Glib::path_get_dirname (_current_path);
			row[_columns.read]  = false;
			row[_columns.file]  = false;
			row[_columns.color] = _color_foreground;
		}
	}

	for (auto& f : dirs) {
		TreeModel::Row row;
		if (pc) {
			row = *(_model->append (*pc));
		} else {
			row = *(_model->append ());
		}
		row[_columns.name] = f;
		row[_columns.path] = Glib::build_filename (path, f);
		row[_columns.read] = false;
		row[_columns.file] = false;
		/* add stub child */
		row                 = *(_model->append (row.children ()));
		row[_columns.read]  = false;
		row[_columns.color] = _color_foreground;
	}

	for (auto& f : files) {
		TreeModel::Row row;
		if (pc) {
			row = *(_model->append (*pc));
		} else {
			row = *(_model->append ());
		}
		row[_columns.name] = f;
		row[_columns.path] = Glib::build_filename (path, f);
		row[_columns.read] = false;
		row[_columns.file] = true;
	}

	if (!pc) {
		_view.set_model (_model);
		if (_view.get_mapped () && !_search_entry.has_focus ()) { _view.grab_focus (); }
		if (_browser_status.get_text ().empty () && files.empty ()) {
			_browser_status.set_text (_file_extensions.empty () ? _("No file types enabled. Edit File Types to show files.") : _("No matching files in this folder."));
		}
	}
}

void
TriggerClipPicker::refresh_library ()
{
	if (!_current_path.empty ()) { list_dir (_current_path); }
}

void
TriggerClipPicker::open_library ()
{
	PBD::open_folder (_clip_library_dir);
}

/* ****************************************************************************
 * Auditioner
 */

void
TriggerClipPicker::set_session (Session* s)
{
	_pending_audition.clear ();
	_preview_playing = false;
	_preview_start = -1;
	_audio_preview->clear ();
	SessionHandlePtr::set_session (s);

	_play_btn.set_sensitive (false);
	_stop_btn.set_sensitive (false);
	_midi_prop_table.hide ();  //only shown when a valid smf is chosen

	if (!_session) {
		_seek_slider.set_sensitive (false);
		_auditioner_connections.drop_connections ();
		_processor_connections.drop_connections ();
		audition_processor_going_away ();
		std::shared_ptr<Controllable> none;
		_gain_control.set_controllable (none);
	} else {
		_auditioner_connections.drop_connections ();
		_session->AuditionActive.connect (_auditioner_connections, invalidator (*this), std::bind (&TriggerClipPicker::audition_active, this, _1), gui_context ());
		_session->the_auditioner ()->AuditionProgress.connect (_auditioner_connections, invalidator (*this), std::bind (&TriggerClipPicker::audition_progress, this, _1, _2), gui_context ());
		_session->the_auditioner ()->processors_changed.connect (_auditioner_connections, invalidator (*this), std::bind (&TriggerClipPicker::audition_processors_changed, this), gui_context ());
		audition_processors_changed (); /* set sensitivity */

		_gain_control.set_controllable (_session->the_auditioner ()->gain_control ());
	}
}

void
TriggerClipPicker::autoplay_toggled ()
{
	UIConfiguration::instance ().set_autoplay_clips (_autoplay_btn.get_active ());
	row_selected (); /* maybe cancel audition, update sensitivity */
}

void
TriggerClipPicker::preview_file (std::string const& path)
{
 bool const midi = !path.empty () && SMFSource::safe_midi_file_extension (path);
 std::string audio = midi ? "" : path;
 if (_audio_preview->path () != audio) {
  _pending_audition.clear ();
  _preview_playing = false;
  _preview_start = -1;
 }
 _audio_preview->set_file (audio);
 _normalize_btn.set_sensitive (!audio.empty ());
 if (midi) { _seek_slider.show (); _audio_preview->hide (); }
 else { _seek_slider.hide (); _audio_preview->show (); }
}

void
TriggerClipPicker::preview_ready ()
{
 auto data = _audio_preview->analysis ();
 if (!data || !_session) return;
 if (!data->error.empty ()) {
  _pending_audition.clear ();
  _stop_btn.set_sensitive (false);
  return;
 }
 if (!_pending_audition.empty () && _pending_audition == _audio_preview->path ()) {
  std::string path = _pending_audition;
  audition (path);
 }
}

void
TriggerClipPicker::normalize_toggled ()
{
 bool enabled = _normalize_btn.get_active ();
 if (UIConfiguration::instance ().get_normalize_browser_audio () == enabled) return;
 UIConfiguration::instance ().set_normalize_browser_audio (enabled);
 UIConfiguration::instance ().save_state ();
 if (_session && (_preview_playing || !_pending_audition.empty ())) {
  _preview_start = _audio_preview->position ();
  audition (_audio_preview->path ());
 }
}

void
TriggerClipPicker::preview_seek (double fraction)
{
 if (!_session || _audio_preview->path ().empty ()) return;
 if (_preview_playing && _session->is_auditioning ()) {
  _session->the_auditioner ()->seek_to_percent (100 * fraction);
 } else {
  _preview_start = fraction;
  audition (_audio_preview->path ());
 }
}

void
TriggerClipPicker::stop_audition ()
{
	_pending_audition.clear ();
	_preview_start = -1;
	_preview_playing = false;
	_stop_btn.set_sensitive (false);
	_audio_preview->set_position (0);
	if (_session) {
		_session->cancel_audition ();
	}
}

void
TriggerClipPicker::audition_active (bool active)
{
	_play_btn.set_sensitive (!active && !_autoplay_btn.get_active ());
	_stop_btn.set_sensitive (active);
	_seek_slider.set_sensitive (active);

	if (!active) {
		_preview_playing = false;
		_audio_preview->set_position (0);
		_seek_slider.set_value (0);
		_seeking = false;
	} else if (_preview_playing && _preview_start >= 0 && _session) {
		_session->the_auditioner ()->seek_to_percent (100 * _preview_start);
		_preview_start = -1;
	}
}

void
TriggerClipPicker::audition_progress (ARDOUR::samplecnt_t pos, ARDOUR::samplecnt_t len)
{
	if (!_seeking && len > 0) {
		_seek_slider.set_value (1000.0 * pos / len);
		_seek_slider.set_sensitive (true);
		if (_preview_playing) { _audio_preview->set_position (double (pos) / len); }
	}
}

bool
TriggerClipPicker::seek_button_press (GdkEventButton*)
{
	_seeking = true;
	return false;
}

bool
TriggerClipPicker::seek_button_release (GdkEventButton*)
{
	_seeking = false;
	_session->the_auditioner ()->seek_to_percent (_seek_slider.get_value () / 10.0);
	_seek_slider.set_sensitive (false);
	return false;
}

void
TriggerClipPicker::audition_selected ()
{
	_preview_start = 0;
	if (_view.get_selection ()->count_selected_rows () < 1) {
		return;
	}
	TreeView::Selection::ListHandle_Path rows = _view.get_selection ()->get_selected_rows ();
	TreeIter                             i    = _model->get_iter (*rows.begin ());
	mark_auditioned (i);
	audition ((*i)[_columns.path]);
}

void
TriggerClipPicker::mark_auditioned (TreeModel::iterator i)
{
	if (!UIConfiguration::instance ().get_highlight_auditioned_clips()) {
		return;
	}
	if (!(*i)[_columns.file]) {
		return;
	}
	(*i)[_columns.color]      = _color_auditioned;
	(*i)[_columns.auditioned] = true;
}

void
TriggerClipPicker::reset_audition_marks (bool force)
{
	for (auto const& i : _model->children ()) {
		if (i[_columns.auditioned] || force) {
			i[_columns.color] = _color_foreground;
		}
	}
}

void
TriggerClipPicker::on_theme_changed ()
{
	_color_foreground = _view.get_style ()->get_text (STATE_NORMAL);
	_color_auditioned = _view.get_style ()->get_text (STATE_INSENSITIVE);

	for (auto const& i : _model->children ()) {
		if (i[_columns.auditioned]) {
			i[_columns.color] = _color_auditioned;
		} else {
			i[_columns.color] = _color_foreground;
		}
	}
}


void
TriggerClipPicker::audition (std::string const& path)
{
	if (!_session) {
		return;
	}
	_session->cancel_audition ();
	_pending_audition.clear ();
	_preview_playing = false;

	if (!Glib::file_test (path, Glib::FILE_TEST_EXISTS)) {
		warning << string_compose (_("Could not read file: %1 (%2)."), path, strerror (errno)) << endmsg;
		return;
	}

	std::shared_ptr<Region> r;

	if (SMFSource::valid_midi_file (path)) {
		std::shared_ptr<SMFSource> ms = std::dynamic_pointer_cast<SMFSource> (SourceFactory::createExternal (DataType::MIDI, *_session, path, 0, Source::Flag (0), false));

		std::string rname = region_name_from_path (ms->path (), false);

		PropertyList plist;
		plist.add (ARDOUR::Properties::start, timepos_t (Temporal::Beats ()));
		plist.add (ARDOUR::Properties::length, ms->length ());
		plist.add (ARDOUR::Properties::name, rname);
		plist.add (ARDOUR::Properties::layer, 0);

		r = std::dynamic_pointer_cast<MidiRegion> (RegionFactory::create (std::dynamic_pointer_cast<Source> (ms), plist, false));
		assert (r);

	} else {
		preview_file (path);
		auto analysis = _audio_preview->analysis ();
		if (_normalize_btn.get_active () && !analysis) {
			_pending_audition = path;
			_stop_btn.set_sensitive (true);
			return;
		}
		if (_normalize_btn.get_active () && !analysis->error.empty ()) { return; }
		SourceList                         srclist;
		std::shared_ptr<AudioFileSource> afs;
		bool                               old_sbp = AudioSource::get_build_peakfiles ();

		/* don't even think of building peakfiles for these files */

		SoundFileInfo info;
		std::string   error_msg;
		if (!AudioFileSource::get_soundfile_info (path, info, error_msg)) {
			error << string_compose (_("Cannot get info from audio file %1 (%2)"), path, error_msg) << endmsg;
			return;
		}

		AudioSource::set_build_peakfiles (false);

		for (uint16_t n = 0; n < info.channels; ++n) {
			try {
				afs = std::dynamic_pointer_cast<AudioFileSource> (SourceFactory::createExternal (DataType::AUDIO, *_session, path, n, Source::Flag (ARDOUR::AudioFileSource::NoPeakFile), false));
				if (afs->sample_rate () != _session->nominal_sample_rate ()) {
					std::shared_ptr<SrcFileSource> sfs (new SrcFileSource (*_session, afs, ARDOUR::SrcGood));
					srclist.push_back (sfs);
				} else {
					srclist.push_back (afs);
				}
			} catch (failed_constructor& err) {
				error << _("Could not access soundfile: ") << path << endmsg;
				AudioSource::set_build_peakfiles (old_sbp);
				return;
			}
		}

		AudioSource::set_build_peakfiles (old_sbp);

		if (srclist.empty ()) {
			return;
		}

		afs               = std::dynamic_pointer_cast<AudioFileSource> (srclist[0]);
		std::string rname = region_name_from_path (afs->path (), false);

		PropertyList plist;
		plist.add (ARDOUR::Properties::start, timepos_t (0));
		plist.add (ARDOUR::Properties::length, srclist[0]->length ());
		plist.add (ARDOUR::Properties::name, rname);
		plist.add (ARDOUR::Properties::layer, 0);

		r = std::dynamic_pointer_cast<AudioRegion> (RegionFactory::create (srclist, plist, false));
		if (r && _normalize_btn.get_active ()) {
			std::dynamic_pointer_cast<AudioRegion> (r)->set_scale_amplitude (analysis->gain);
		}
		_preview_playing = true;
	}

	r->set_position (timepos_t ());

	_session->audition_region (r);
}

void
TriggerClipPicker::audition_processor_idle ()
{
	if (!_session || _session->deletion_in_progress () || !_session->the_auditioner ()) {
		return;
	}
	assert (_session && _session->the_auditioner ());
	ARDOUR_UI::instance ()->get_process_buffers ();
	_session->the_auditioner ()->idle_synth_update ();
	ARDOUR_UI::instance ()->drop_process_buffers ();
}

bool
TriggerClipPicker::audition_processor_viz (bool show)
{
	if (show) {
		_idle_connection = Timers::fps_connect (sigc::mem_fun (*this, &TriggerClipPicker::audition_processor_idle));
	} else {
		_idle_connection.disconnect ();
	}
	return false;
}

void
TriggerClipPicker::audition_show_plugin_ui ()
{
	if (!_audition_plugnui) {
		std::shared_ptr<PluginInsert> plugin_insert = std::dynamic_pointer_cast<PluginInsert> (_session->the_auditioner ()->the_instrument ());
		if (plugin_insert) {
			_audition_plugnui = new PluginUIWindow (plugin_insert);
			_audition_plugnui->set_session (_session);
			_audition_plugnui->show_all ();
			_audition_plugnui->set_title (/* generate_processor_title (plugin_insert)*/ _("Audition Synth"));
			plugin_insert->DropReferences.connect (_processor_connections, invalidator (*this), std::bind (&TriggerClipPicker::audition_processor_going_away, this), gui_context());

			_audition_plugnui->signal_map_event ().connect (sigc::hide (sigc::bind (sigc::mem_fun (*this, &TriggerClipPicker::audition_processor_viz), true)));
			_audition_plugnui->signal_unmap_event ().connect (sigc::hide (sigc::bind (sigc::mem_fun (*this, &TriggerClipPicker::audition_processor_viz), false)));
		}
	}
	if (_audition_plugnui) {
		_audition_plugnui->present ();
	}
}

void
TriggerClipPicker::audition_processor_going_away ()
{
	if (_audition_plugnui) {
		_idle_connection.disconnect ();
		delete _audition_plugnui;
	}
	_audition_plugnui = 0;
}

void
TriggerClipPicker::audition_processors_changed ()
{
	if (!_session || _session->deletion_in_progress () || !_session->the_auditioner ()) {
		_show_plugin_btn.set_sensitive (false);
		set_tooltip (_show_plugin_btn, "You must first play one midi file to show the plugin's GUI");
		return;
	}

	if (_session && _session->the_auditioner ()->get_audition_synth_info()) {
		std::shared_ptr<PluginInsert> plugin_insert = std::dynamic_pointer_cast<PluginInsert> (_session->the_auditioner ()->the_instrument ());
		if (plugin_insert) {
			set_tooltip (_show_plugin_btn, "Show the selected audition-instrument's GUI");
			_show_plugin_btn.set_sensitive (true);
		}
	}
}
