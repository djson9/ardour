/*
 * Copyright (C) 2005-2016 Paul Davis <paul@linuxaudiosystems.com>
 * Copyright (C) 2005 Taybin Rutkin <taybin@taybin.com>
 * Copyright (C) 2008-2010 David Robillard <d@drobilla.net>
 * Copyright (C) 2009-2011 Carl Hetherington <carl@carlh.net>
 * Copyright (C) 2012-2016 Tim Mayberry <mojofunk@gmail.com>
 * Copyright (C) 2014-2018 Ben Loftis <ben@harrisonconsoles.com>
 * Copyright (C) 2015-2016 Nick Mainsbridge <mainsbridge@gmail.com>
 * Copyright (C) 2015-2017 Robin Gareus <robin@gareus.org>
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

#include "pbd/convert.h"
#include "pbd/error.h"
#include "pbd/file_utils.h"
#include "pbd/basename.h"
#include <fstream>
#include <iterator>
#include <glibmm/main.h>

#include "ardour/filesystem_paths.h"
#include "ardour/revision.h"

#include "ardour_ui.h"
#include "public_editor.h"
#include "keyboard.h"
#include "keybinding_config.h"
#include "keyeditor.h"
#include "opts.h"
#include "ui_config.h"

#include "pbd/i18n.h"

using namespace std;
using namespace Gtk;
using namespace PBD;
using namespace ARDOUR;
using Gtkmm2ext::Keyboard;

#ifdef __APPLE__
guint ArdourKeyboard::constraint_mod = Keyboard::PrimaryModifier;
#else
guint ArdourKeyboard::constraint_mod = Keyboard::TertiaryModifier;
#endif

/* RegionSlipContentsDrag */
guint ArdourKeyboard::slip_contents_mod = Keyboard::PrimaryModifier|Keyboard::TertiaryModifier;

/* TrimDrag::motion() */
guint ArdourKeyboard::trim_overlap_mod = Keyboard::TertiaryModifier;

/* TrimDrag::start_grab() */
guint ArdourKeyboard::trim_anchored_mod = Keyboard::PrimaryModifier|Keyboard::TertiaryModifier;

/* ControlPointDrag::motion() && LineDrag::motion()*/
guint ArdourKeyboard::fine_adjust_mod = Keyboard::PrimaryModifier|Keyboard::SecondaryModifier; // XXX better just 2ndary

/* ControlPointDrag::start_grab() && MarkerDrag::motion() */
guint ArdourKeyboard::push_points_mod = Keyboard::PrimaryModifier|Keyboard::Level4Modifier;

/* NoteResizeDrag::start_grab() */
guint ArdourKeyboard::note_size_relative_mod = Keyboard::TertiaryModifier; // XXX better: 2ndary

ArdourKeyboard::ArdourKeyboard (ARDOUR_UI& ardour_ui) : ui (ardour_ui)
{
	Keyboard::RelevantModifierKeysChanged.connect (sigc::mem_fun (*this, &ArdourKeyboard::reset_relevant_modifier_key_mask));
}

void
ArdourKeyboard::find_bindings_files (map<string,string>& files)
{
	vector<std::string> found;
	Searchpath spath = ardour_config_search_path();

	find_files_matching_pattern (found, spath, string_compose ("*%1", Keyboard::binding_filename_suffix));

	if (found.empty()) {
		return;
	}

	for (vector<std::string>::iterator x = found.begin(); x != found.end(); ++x) {
		std::string path(*x);
		pair<string,string> namepath;
		namepath.second = path;
		namepath.first = PBD::basename_nosuffix (path);
		files.insert (namepath);
	}
}

/* The compact arrangement has no keyboard-switchable editing toolbar.
 * Remove only those bindings; menus, transport, selection and text still work.
 * Apply after startup and live reload without rewriting the user's key file. */
static void
remove_hidden_toolbar_shortcuts ()
{
	if (Glib::getenv ("ARDOUR_LIVE_LAYOUT") != "1") { return; }
	using Gtkmm2ext::Bindings;
	for (auto* bindings : Bindings::bindings) {
		if (bindings->name () != "Editor" && bindings->name () != "Global" && bindings->name () != "Editing") { continue; }
		XMLNode saved ("Bindings");
		bindings->save (saved);
		for (auto* operation : saved.children ()) {
			for (auto* binding : operation->children ()) {
				auto* action = binding->property ("action");
				if (!action || !binding->property ("key")) { continue; }
				const std::string name = action->value ();
				const std::string match = (name.find ("EditorEditing/") == 0 || name.find ("EditorSnap/") == 0) ? name.substr (6) : name;
				for (const char* prefix : {
				    "Editing/set-mouse-mode-", "Editor/set-mouse-mode-", "Editing/snap-", "Snap/grid-type-",
				    "Editing/cycle-snap-mode", "Editing/next-grid-choice", "Editing/prev-grid-choice",
				    "Editor/set-edit-mode-", "Editor/cycle-edit-mode", "Editor/set-edit-point-", "Editor/cycle-edit-point",
				    "Editor/nudge-", "Common/nudge-", "Editor/playhead-to-next-grid", "Editor/playhead-to-prev-grid"}) {
					if (match.compare (0, std::char_traits<char>::length (prefix), prefix) == 0) {
						bindings->remove (operation->name () == "Release" ? Bindings::Release : Bindings::Press, name, false);
						break;
					}
				}
			}
		}
	}
	if (auto* editor = Bindings::get_bindings ("Editor")) {
		Gtkmm2ext::KeyboardKey slash (0, GDK_slash);
		std::string previous;
		if (editor->is_bound (slash, Bindings::Press, &previous)) editor->remove (Bindings::Press, previous, false);
		editor->replace (slash, Bindings::Press, "Editor/find-and-display-stripable", false);
		Gtkmm2ext::KeyboardKey loop;
		Gtkmm2ext::KeyboardKey::make_key ("Primary-l", loop);
		if (editor->is_bound (loop, Bindings::Press, &previous)) editor->remove (Bindings::Press, previous, false);
		editor->replace (loop, Bindings::Press, "Editor/loop-selection", false);
		for (auto const& shortcut : {std::make_pair ("Primary-g", "Editor/group-selected-tracks"), std::make_pair ("Primary-Tertiary-g", "Editor/ungroup-selected-tracks")}) {
			Gtkmm2ext::KeyboardKey key; Gtkmm2ext::KeyboardKey::make_key (shortcut.first, key);
			if (editor->is_bound (key, Bindings::Press, &previous)) editor->remove (Bindings::Press, previous, false);
			editor->replace (key, Bindings::Press, shortcut.second, false);
		}
	}

	for (const char* group : {"Editor", "Global"}) {
		if (auto* bindings = Bindings::get_bindings (group)) {
			for (auto const& shortcut : {std::make_pair ("Primary-t", "Main/AddAudioTrack"), std::make_pair ("Primary-Tertiary-t", "Main/AddMidiTrack")}) {
				Gtkmm2ext::KeyboardKey key; Gtkmm2ext::KeyboardKey::make_key (shortcut.first, key);
				std::string previous;
				if (bindings->is_bound (key, Bindings::Press, &previous)) bindings->remove (Bindings::Press, previous, false);
				bindings->replace (key, Bindings::Press, shortcut.second, false);
			}
		}
	}

}

void
ArdourKeyboard::setup_keybindings ()
{
	string keybindings_path = ARDOUR_COMMAND_LINE::keybindings_path;
	string default_bindings = string_compose ("%1%2", UIConfiguration::instance().get_default_bindings(), Keyboard::binding_filename_suffix);
	vector<string> strs;

	binding_files.clear ();

	find_bindings_files (binding_files);

	/* set up the per-user bindings path */

	string lowercase_program_name = downcase (string(PROGRAM_NAME));

	/* The major-version preferences directory already separates releases.
	 * Keep this filename stable across local source commits. */

	user_keybindings_path = Glib::build_filename (user_config_directory(), lowercase_program_name + binding_filename_suffix);

	if (Glib::file_test (user_keybindings_path, Glib::FILE_TEST_EXISTS)) {
		std::pair<string,string> newpair;
		newpair.first = _("your own");
		newpair.second = user_keybindings_path;
		binding_files.insert (newpair);
	}

	/* check to see if they gave a style name ("ergonomic") or an actual filename (*.bindings)
	*/

	if (!keybindings_path.empty() && keybindings_path.find (binding_filename_suffix) == string::npos) {

		// just a style name - allow user to
		// specify the layout type.

		char* layout;

		if ((layout = getenv ("ARDOUR_KEYBOARD_LAYOUT")) != 0 && layout[0] != '\0') {

			/* user-specified keyboard layout */

			keybindings_path += '-';
			keybindings_path += layout;

		} else {

			/* default to US/ANSI - we have to pick something */

			keybindings_path += "-us";
		}

		keybindings_path += binding_filename_suffix;
	}

	if (keybindings_path.empty()) {

		/* no path or binding name given: check the user one first */

		if (!Glib::file_test (user_keybindings_path, Glib::FILE_TEST_EXISTS)) {

			keybindings_path = "";

		} else {

			keybindings_path = user_keybindings_path;
		}
	}

	/* if we still don't have a path at this point, use the default */

	if (keybindings_path.empty()) {
		keybindings_path = default_bindings;
	}

	while (true) {

		if (!Glib::path_is_absolute (keybindings_path)) {

			/* not absolute - look in the usual places */
			std::string keybindings_file;

			if (!find_file (ardour_config_search_path(), keybindings_path, keybindings_file)) {

				if (keybindings_path == default_bindings) {
					error << string_compose (_("Default keybindings not found - %1 will be hard to use!"), PROGRAM_NAME) << endmsg;
					return;
				} else {
					warning << string_compose (_("Key bindings file \"%1\" not found. Default bindings used instead"),
								   keybindings_path)
						<< endmsg;
					keybindings_path = default_bindings;
				}

			} else {

				/* use it */

				keybindings_path = keybindings_file;
				break;

			}

		} else {

			/* path is absolute already */

			if (!Glib::file_test (keybindings_path, Glib::FILE_TEST_EXISTS)) {
				if (keybindings_path == default_bindings) {
					error << string_compose (_("Default keybindings not found - %1 will be hard to use!"), PROGRAM_NAME) << endmsg;
					return;
				} else {
					keybindings_path = default_bindings;
				}

			} else {
				break;
			}
		}
	}

	load_keybindings (keybindings_path);
	remove_hidden_toolbar_shortcuts ();

	/* Poll the pathname, not the inode: text editors often save by rename.
	 * Apply on the GUI thread, keeping every open window's Bindings pointer. */
	static sigc::connection config_watch;
	if (!config_watch.connected ()) {
		config_watch = Glib::signal_timeout ().connect ([path = ARDOUR_COMMAND_LINE::keybindings_path.empty () ? user_keybindings_path : keybindings_path, state = ArdourKeybindingConfig::WatchState ()] () mutable -> bool {
			std::ifstream file (path, std::ios::binary);
			if (!file) {
				state.missing ();
				return true;
			}
			std::string contents ((std::istreambuf_iterator<char> (file)), std::istreambuf_iterator<char> ());
			if (file.bad () || !state.ready (contents)) {
				return true;
			}
			std::string reason;
			if (ArdourKeybindingConfig::reload (contents, reason)) {
				remove_hidden_toolbar_shortcuts ();
				KeyEditor::UpdateBindings ();
				info << "Reloaded keyboard shortcuts from " << path << endmsg;
			} else {
				warning << "Keeping current keyboard shortcuts: " << reason << endmsg;
			}
			return true;
		}, 500);
	}

	/* catch changes made via some GTK mechanism */

	// GtkAccelMap* accelmap = gtk_accel_map_get();
	// g_signal_connect (accelmap, "changed", (GCallback) accel_map_changed, this);
}

XMLNode&
ArdourKeyboard::get_state () const
{
	XMLNode* node = &Keyboard::get_state ();

	node->set_property ("constraint-modifier", constraint_mod);
	node->set_property ("slip-contents-modifier", slip_contents_mod);
	node->set_property ("trim-overlap-modifier", trim_overlap_mod);
	node->set_property ("trim-anchored-modifier", trim_anchored_mod);
	node->set_property ("fine-adjust-modifier", fine_adjust_mod);
	node->set_property ("push-points-modifier", push_points_mod);
	node->set_property ("note-size-relative-modifier", note_size_relative_mod);

	return *node;
}

int
ArdourKeyboard::set_state (const XMLNode& node, int version)
{
	node.get_property ("constraint-modifier", constraint_mod);
	node.get_property ("slip-contents-modifier", slip_contents_mod);
	node.get_property ("trim-overlap-modifier", trim_overlap_mod);
	node.get_property ("trim-anchored-modifier", trim_anchored_mod);
	node.get_property ("fine-adjust-modifier", fine_adjust_mod);
	node.get_property ("push-points-modifier", push_points_mod);
	node.get_property ("note-size-relative-modifier", note_size_relative_mod);

	return Keyboard::set_state (node, version);
}

void
ArdourKeyboard::reset_relevant_modifier_key_mask ()
{
	RelevantModifierKeyMask = GdkModifierType (RelevantModifierKeyMask | constraint_mod);
	RelevantModifierKeyMask = GdkModifierType (RelevantModifierKeyMask | slip_contents_mod);
	RelevantModifierKeyMask = GdkModifierType (RelevantModifierKeyMask | trim_overlap_mod);
	RelevantModifierKeyMask = GdkModifierType (RelevantModifierKeyMask | trim_anchored_mod);
	RelevantModifierKeyMask = GdkModifierType (RelevantModifierKeyMask | fine_adjust_mod);
	RelevantModifierKeyMask = GdkModifierType (RelevantModifierKeyMask | push_points_mod);
	RelevantModifierKeyMask = GdkModifierType (RelevantModifierKeyMask | note_size_relative_mod);
}

/* Snap and snap delta modifiers may contain each other, so we use the
 * following two methods to sort that out:
 */
bool
ArdourKeyboard::indicates_snap (guint state)
{
	const bool contains_s = Keyboard::modifier_state_contains (state, Keyboard::snap_modifier ());
	const bool contains_d = Keyboard::modifier_state_contains (state, Keyboard::snap_delta_modifier ());
	const bool s_contains_d = Keyboard::modifier_state_contains (Keyboard::snap_modifier (), Keyboard::snap_delta_modifier ());

	return  (contains_s && ((contains_d && s_contains_d) || !contains_d));
}

bool
ArdourKeyboard::indicates_snap_delta (guint state)
{
	const bool contains_d = Keyboard::modifier_state_contains (state, Keyboard::snap_delta_modifier ());
	const bool contains_s = Keyboard::modifier_state_contains (state, Keyboard::snap_modifier ());
	const bool d_contains_s = Keyboard::modifier_state_contains (Keyboard::snap_delta_modifier (), Keyboard::snap_modifier ());

	return (contains_d && ((contains_s && d_contains_s) || !contains_s));
}

/* Constraint and copy modifiers are both in effect at the beginning of some drags, and may be set ambiguously */
bool
ArdourKeyboard::indicates_copy (guint state)
{
	const bool contains_c = Keyboard::modifier_state_contains (state, Keyboard::CopyModifier);
	const bool equals_cs = Keyboard::modifier_state_equals (state, constraint_modifier ());

	return  contains_c && !equals_cs;
}

bool
ArdourKeyboard::indicates_constraint (guint state)
{
	const bool contains_cs = Keyboard::modifier_state_contains (state, constraint_modifier ());
	const bool equals_c = Keyboard::modifier_state_equals (state, Keyboard::CopyModifier);

	return contains_cs && !equals_c;
}

void
ArdourKeyboard::set_constraint_modifier (guint mod)
{
	constraint_mod = mod;
	the_keyboard().reset_relevant_modifier_key_mask();
}

void
ArdourKeyboard::set_slip_contents_modifier (guint mod)
{
	slip_contents_mod = mod;
	the_keyboard().reset_relevant_modifier_key_mask();
}

void
ArdourKeyboard::set_trim_overlap_modifier (guint mod)
{
	trim_overlap_mod = mod;
	the_keyboard().reset_relevant_modifier_key_mask();
}

void
ArdourKeyboard::set_trim_anchored_modifier (guint mod)
{
	trim_anchored_mod = mod;
	the_keyboard().reset_relevant_modifier_key_mask();
}

void
ArdourKeyboard::set_fine_adjust_modifier (guint mod)
{
	fine_adjust_mod = mod;
	the_keyboard().reset_relevant_modifier_key_mask();
}

void
ArdourKeyboard::set_push_points_modifier (guint mod)
{
	push_points_mod = mod;
	the_keyboard().reset_relevant_modifier_key_mask();
}

void
ArdourKeyboard::set_note_size_relative_modifier (guint mod)
{
	note_size_relative_mod = mod;
	the_keyboard().reset_relevant_modifier_key_mask();
}

SelectionOperation
ArdourKeyboard::selection_type (guint state)
{
	/* note that there is no modifier for "Add" */

	if (modifier_state_equals (state, RangeSelectModifier)) {
		return SelectionExtend;
	} else if (modifier_state_equals (state, PrimaryModifier)) {
		return SelectionToggle;
	} else {
		return SelectionSet;
	}
}
