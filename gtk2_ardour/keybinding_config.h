/* Live reload of the existing per-user .keys format. */
#pragma once

#include <set>
#include <string>
#include <vector>
#include "pbd/xml++.h"
#include "gtkmm2ext/actions.h"
#include "gtkmm2ext/bindings.h"

namespace ArdourKeybindingConfig {

inline bool valid_modifiers (std::string value)
{
	std::set<std::string> seen;
	std::string::size_type pos;
	while ((pos = value.find ('-')) != std::string::npos) {
		std::string modifier = value.substr (0, pos);
		/* Some shipped bindings use Primary->Secondary->Tertiary-key. */
		if (!modifier.empty () && modifier.front () == '>') {
			modifier.erase (0, 1);
		}
		if ((modifier != "Primary" && modifier != "Secondary" && modifier != "Tertiary" && modifier != "Level4" && modifier != "Shift") || !seen.insert (modifier).second) {
			return false;
		}
		value.erase (0, pos + 1);
	}
	return !value.empty ();
}

/* Validate the entire snapshot before touching live objects. Windows retain
 * pointers to these objects, so replacing the binding registry is not safe. */
inline bool reload (std::string const& contents, std::string& error)
{
	using namespace Gtkmm2ext;
	XMLTree tree;
	if (contents.find ('\0') != std::string::npos || !tree.read_buffer (contents.c_str ()) || !tree.root () || tree.root ()->name () != "BindingSet") {
		error = "expected a valid BindingSet XML document";
		return false;
	}

	/* Keep previously known relative actions valid after an unbind. */
	static std::set<std::string> known_actions;
	for (Bindings* bindings : Bindings::bindings) {
		XMLNode saved ("Bindings");
		bindings->save (saved);
		for (XMLNode* operation : saved.children ()) {
			for (XMLNode* binding : operation->children ()) {
				if (auto action = binding->property ("action")) {
					known_actions.insert (action->value ());
				}
			}
		}
	}

	std::set<std::string> groups;
	std::vector<std::pair<Bindings*, XMLNode const*> > updates;
	for (XMLNode* group : tree.root ()->children ()) {
		if (group->name () == "comment") { continue; }
		auto name = group->property ("name");
		Bindings* live = name ? Bindings::get_bindings (name->value ()) : nullptr;
		if (group->name () != "Bindings" || !live || !groups.insert (name->value ()).second) {
			error = "unknown, missing, or duplicate Bindings name";
			return false;
		}
		std::set<std::string> operations;
		for (XMLNode* operation : group->children ()) {
			if (operation->name () == "comment") { continue; }
			if ((operation->name () != "Press" && operation->name () != "Release") || !operations.insert (operation->name ()).second) {
				error = "expected unique Press/Release sections";
				return false;
			}
			std::set<KeyboardKey> keys;
			std::set<MouseButton> buttons;
			std::set<std::string> actions;
			for (XMLNode* binding : operation->children ()) {
				if (binding->name () == "comment") { continue; }
				auto action = binding->property ("action");
				auto key = binding->property ("key");
				auto button = binding->property ("button");
				if (binding->name () != "Binding" || !action || action->value ().empty () || (bool (key) == bool (button)) || !actions.insert (action->value ()).second) {
					error = "invalid or duplicate Binding action in " + name->value () + ": " + binding->name () + (action ? " " + action->value () : " (missing action)");
					return false;
				}
				if (!known_actions.count (action->value ()) && !ActionManager::get_action (action->value (), false)) {
					error = "unknown action: " + action->value ();
					return false;
				}
				KeyboardKey parsed_key;
				MouseButton parsed_button;
				if (key && (!valid_modifiers (key->value ()) || !KeyboardKey::make_key (key->value (), parsed_key) || !keys.insert (parsed_key).second)) {
					error = "invalid or duplicate key: " + key->value ();
					return false;
				}
				if (button && (!valid_modifiers (button->value ()) || !MouseButton::make_button (button->value (), parsed_button) || parsed_button.button () == 0 || !buttons.insert (parsed_button).second)) {
					error = "invalid or duplicate mouse button";
					return false;
				}
			}
		}
		if (operations.empty ()) {
			error = "Bindings must contain Press or Release";
			return false;
		}
		updates.emplace_back (live, group);
	}
	if (updates.empty ()) {
		error = "BindingSet contains no binding groups";
		return false;
	}

	for (auto const& update : updates) {
		/* Upstream load() clears keyboard maps but not mouse maps. */
		XMLNode previous ("Bindings");
		update.first->save (previous);
		for (XMLNode* operation : previous.children ()) {
			for (XMLNode* binding : operation->children ()) {
				if (auto button = binding->property ("button")) {
					MouseButton parsed;
					if (MouseButton::make_button (button->value (), parsed)) {
						update.first->remove (parsed, operation->name () == "Press" ? Bindings::Press : Bindings::Release);
					}
				}
			}
		}
		update.first->load (*update.second);
		Bindings::BindingsChanged (update.first);
	}
	Bindings::associate_all ();
	error.clear ();
	return true;
}

/* Two equal observations debounce editor saves, including atomic rename.
 * Missing files leave the current shortcuts intact. Failed snapshots are only
 * retried after another edit, avoiding a repeated warning every poll. */
class WatchState {
public:
	bool ready (std::string const& contents) {
		if (contents != pending) {
			pending = contents;
			return false;
		}
		if (have_attempt && attempted == contents) {
			return false;
		}
		attempted = contents;
		have_attempt = true;
		return true;
	}
	void missing () { pending.clear (); have_attempt = false; }
private:
	std::string pending;
	std::string attempted;
	bool have_attempt = false;
};
}
