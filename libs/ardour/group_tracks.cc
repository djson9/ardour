/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <vector>
#include "pbd/xml++.h"
#include "ardour/audioengine.h"
#include "ardour/group_tracks.h"
#include "ardour/io.h"
#include "ardour/io_vector.h"
#include "ardour/port.h"
#include "ardour/rc_configuration.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/solo_control.h"
#include "ardour/solo_isolate_control.h"

using namespace ARDOUR;

namespace {
XMLNode& metadata (std::shared_ptr<Route> const& r)
{
	if (!r->extra_xml ("GroupTrack")) r->add_extra_xml (*new XMLNode ("GroupTrack"));
	return *r->extra_xml ("GroupTrack");
}
using Connections = std::vector<std::vector<std::string>>;
Connections connections (std::shared_ptr<IO> io)
{
	Connections result (io->n_ports ().n_audio ());
	for (size_t n = 0; n < result.size (); ++n) {
		io->ports ()->port (DataType::AUDIO, n)->get_connections (result[n]);
		for (auto& name : result[n]) name = AudioEngine::instance ()->make_port_name_non_relative (name);
		std::sort (result[n].begin (), result[n].end ());
	}
	return result;
}
bool reconnect (std::shared_ptr<IO> io, Connections const& dest)
{
	if (io->n_ports ().n_audio () != dest.size ()) return false;
	bool ok = true;
	for (size_t n = 0; n < dest.size (); ++n) {
		auto port = io->ports ()->port (DataType::AUDIO, n);
		std::vector<std::string> old;
		port->get_connections (old);
		for (auto const& name : old) ok = (io->disconnect (port, name) == 0) && ok;
		for (auto const& name : dest[n]) ok = (io->connect (port, name) == 0) && ok;
	}
	return ok;
}

// Which routes each route reaches through actual port and send connections.
using Reach = std::map<std::shared_ptr<Route>, std::set<std::shared_ptr<Route>>>;
Reach reachability (Session& s)
{
	auto routes = s.get_routes ();
	std::map<std::shared_ptr<Route>, std::vector<std::shared_ptr<Route>>> next;
	for (auto const& from : *routes) for (auto const& to : *routes) {
		if (from != to && from->direct_feeds_according_to_reality (to)) next[from].push_back (to);
	}
	Reach result;
	for (auto const& from : *routes) {
		auto& seen = result[from];
		std::vector<std::shared_ptr<Route>> pending {from};
		while (!pending.empty ()) {
			auto r = pending.back (); pending.pop_back ();
			for (auto const& n : next[r]) if (n != from && seen.insert (n).second) pending.push_back (n);
		}
	}
	return result;
}

// Ardour propagates implicit solo only when a solo changes; route IO handlers
// deliberately ignore new connections and only remove stale implicit solo.
// Grouping adds signal paths (always for a new group bus), so apply what
// Session::route_solo_changed would have done for each new path. Otherwise a
// group carrying a soloed child is muted by that same solo. Existing paths are
// left alone to keep Ardour's solo-within-bus semantics.
void propagate_solo_to_new_paths (Session& s, Reach const& before)
{
	if (Config->get_solo_control_is_listen_control ()) return;
	auto receives = [] (std::shared_ptr<Route> const& r) { return r->can_solo () && !r->solo_isolate_control ()->solo_isolated (); };
	auto soloed = [] (std::shared_ptr<Route> const& r) { return r->solo_control ()->soloed_by_self_or_masters (); };
	std::map<std::shared_ptr<Route>, int32_t> up, down;
	for (auto const& entry : reachability (s)) {
		auto const& from = entry.first;
		auto old = before.find (from);
		for (auto const& to : entry.second) {
			if (old != before.end () && old->second.count (to)) continue;
			if (soloed (from) && receives (to)) ++up[to];
			if (soloed (to) && !to->soloed_by_others_upstream () && receives (from)) ++down[from];
		}
	}
	for (auto const& entry : up) entry.first->solo_control ()->mod_solo_by_others_upstream (entry.second);
	for (auto const& entry : down) entry.first->solo_control ()->mod_solo_by_others_downstream (entry.second);
}
}

bool GroupTracks::is_group (std::shared_ptr<Route> const& r)
{
	bool value = false;
	if (r && !r->is_track () && !r->is_singleton ()) {
		if (auto n = r->extra_xml ("GroupTrack")) n->get_property ("group", value);
	}
	return value;
}
bool GroupTracks::collapsed (std::shared_ptr<Route> const& r)
{
	bool value = false;
	if (is_group (r)) r->extra_xml ("GroupTrack")->get_property ("collapsed", value);
	return value;
}
void GroupTracks::set_collapsed (Session& s, std::shared_ptr<Route> const& r, bool value)
{
	if (!is_group (r)) return;
	metadata (r).set_property ("collapsed", value);
	s.set_dirty ();
}
std::string GroupTracks::parent_id (std::shared_ptr<Route> const& r)
{
	std::string id;
	if (r && !r->is_singleton ()) {
		if (auto n = r->extra_xml ("GroupTrack")) n->get_property ("parent", id);
	}
	return id;
}
std::shared_ptr<Route> GroupTracks::parent (Session& s, std::shared_ptr<Route> const& r)
{
	auto id = parent_id (r);
	auto p = id.empty () ? std::shared_ptr<Route> () : s.route_by_id (PBD::ID (id));
	return p != r && is_group (p) ? p : std::shared_ptr<Route> ();
}
RouteList GroupTracks::ancestors (Session& s, std::shared_ptr<Route> const& r)
{
	RouteList result;
	std::set<std::shared_ptr<Route>> seen {r};
	for (auto p = parent (s, r); p && seen.insert (p).second; p = parent (s, p)) result.push_back (p);
	return result;
}
bool GroupTracks::folded (Session& s, std::shared_ptr<Route> const& r)
{
	for (auto const& p : ancestors (s, r)) if (collapsed (p)) return true;
	return false;
}
RouteList GroupTracks::children (Session& s, std::shared_ptr<Route> const& group, bool recursive)
{
	RouteList result;
	if (!is_group (group)) return result;
	for (auto const& r : *s.get_routes ()) {
		if (r == group) continue;
		if (!recursive) {
			if (parent (s, r) == group) result.push_back (r);
		} else {
			auto chain = ancestors (s, r);
			if (std::find (chain.begin (), chain.end (), group) != chain.end ()) result.push_back (r);
		}
	}
	result.sort (Stripable::Sorter ());
	return result;
}
void GroupTracks::normalize_order (Session& s)
{
	StripableList all, ordered;
	s.get_stripables (all);
	all.sort (Stripable::Sorter ());
	std::set<std::shared_ptr<Stripable>> seen;
	std::function<void(std::shared_ptr<Stripable>)> append = [&] (std::shared_ptr<Stripable> strip) {
		if (!seen.insert (strip).second) return;
		ordered.push_back (strip);
		for (auto const& child : children (s, std::dynamic_pointer_cast<Route> (strip))) append (child);
	};
	for (auto const& strip : all) if (!parent (s, std::dynamic_pointer_cast<Route> (strip))) append (strip);
	for (auto const& strip : all) append (strip); // Keep malformed/cyclic imported state visible.
	PresentationInfo::ChangeSuspender cs;
	PresentationInfo::order_t order = 0;
	for (auto const& strip : ordered) strip->set_presentation_order (order++);
}

std::shared_ptr<Route> GroupTracks::create (Session& s, RouteList selected, std::string& error)
{
	selected.sort (Stripable::Sorter ());
	selected.unique ();
	const std::set<std::shared_ptr<Route>> selection (selected.begin (), selected.end ());
	selected.remove_if ([&] (std::shared_ptr<Route> r) {
		for (auto const& p : ancestors (s, r)) if (selection.count (p)) return true;
		return false;
	});
	if (selected.empty ()) { error = "Select tracks to group."; return {}; }
	auto first = selected.front ();
	auto outer = parent (s, first);
	auto dest = connections (first->output ());
	for (auto const& r : selected) {
		if (r->is_singleton () || parent (s, r) != outer || dest.empty () || connections (r->output ()) != dest) {
			error = "Select sibling audio or instrument tracks with matching audio outputs and channel layouts. MIDI-only tracks cannot be grouped yet.";
			return {};
		}
	}
	const auto before = reachability (s);
	std::shared_ptr<Route> bus;
	try {
		auto created = s.new_audio_route (dest.size (), dest.size (), {}, 1, "Group", PresentationInfo::AudioBus, PresentationInfo::max_order);
		if (!created.empty ()) bus = created.front ();
	} catch (...) {}
	if (!bus) { error = "Could not create the group bus."; return {}; }
	// No external input auto-connections belong on a summing group.
	bus->input ()->disconnect ();
	Connections to_bus (dest.size ());
	for (size_t n = 0; n < dest.size (); ++n) to_bus[n].push_back (bus->input ()->ports ()->port (DataType::AUDIO, n)->name ());
	bool ok = reconnect (bus->output (), dest);
	for (auto const& r : selected) if (ok) ok = reconnect (r->output (), to_bus);
	if (!ok) {
		for (auto const& r : selected) reconnect (r->output (), dest);
		s.remove_route (bus);
		error = "Could not connect the group; the original audio routing was restored.";
		return {};
	}
	metadata (bus).set_property ("group", true);
	metadata (bus).set_property ("parent", outer ? outer->id ().to_s () : std::string ());
	metadata (bus).set_property ("collapsed", false);
	bus->presentation_info ().set_color (first->presentation_info ().color ());
	bus->set_presentation_order (first->presentation_info ().order ());
	for (auto const& r : selected) metadata (r).set_property ("parent", bus->id ().to_s ());
	propagate_solo_to_new_paths (s, before);
	normalize_order (s);
	s.set_dirty ();
	return bus;
}

void GroupTracks::move (Session& s, RouteList const& selected, bool up)
{
	std::set<std::shared_ptr<Route>> moving (selected.begin (), selected.end ());
	for (auto const& r : selected) for (auto const& p : ancestors (s, r)) if (moving.count (p)) { moving.erase (r); break; }
	std::map<std::shared_ptr<Route>, std::vector<std::shared_ptr<Route>>> siblings;
	RouteList all (*s.get_routes ());
	all.sort (Stripable::Sorter ());
	for (auto const& r : all) if (!r->is_singleton ()) siblings[parent (s, r)].push_back (r);
	PresentationInfo::ChangeSuspender cs;
	for (auto& entry : siblings) {
		auto& list = entry.second;
		if (!up) std::reverse (list.begin (), list.end ());
		for (size_t n = 1; n < list.size (); ++n) {
			if (!moving.count (list[n]) || moving.count (list[n-1])) continue;
			auto order = list[n]->presentation_info ().order ();
			list[n]->set_presentation_order (list[n-1]->presentation_info ().order ());
			list[n-1]->set_presentation_order (order);
			std::swap (list[n], list[n-1]);
		}
	}
	normalize_order (s);
	s.set_dirty ();
}

bool GroupTracks::drop (Session& s, RouteList selected, std::shared_ptr<Route> destination,
                        std::shared_ptr<Route> before, std::string& error)
{
	error.clear ();
	auto live = [&] (std::shared_ptr<Route> r) { return r && s.route_by_id (r->id ()) == r; };
	if ((destination && (!live (destination) || !is_group (destination))) ||
	    (before && (!live (before) || before->is_singleton () || parent (s, before) != destination))) {
		error = "The drop target is no longer available."; return false;
	}
	for (auto const& r : selected) if (!live (r) || r->is_singleton ()) {
		error = "Master and monitor tracks cannot be moved into groups."; return false;
	}
	selected.sort (Stripable::Sorter ()); selected.unique ();
	const std::set<std::shared_ptr<Route>> selection (selected.begin (), selected.end ());
	selected.remove_if ([&] (std::shared_ptr<Route> r) {
		for (auto const& p : ancestors (s, r)) if (selection.count (p)) return true;
		return false;
	});
	if (selected.empty ()) return false;
	const std::set<std::shared_ptr<Route>> moving (selected.begin (), selected.end ());
	for (auto p = destination; p; p = parent (s, p)) if (moving.count (p)) {
		error = "A group cannot be placed inside itself or one of its children."; return false;
	}
	// Resolve insertion against the original sibling order, even when the
	// hovered sibling is itself selected. Never split a group's descendants.
	RouteList siblings;
	for (auto const& r : *s.get_routes ()) if (!r->is_singleton () && parent (s, r) == destination) siblings.push_back (r);
	siblings.sort (Stripable::Sorter ());
	auto insertion = before ? std::find (siblings.begin (), siblings.end (), before) : siblings.end ();
	while (insertion != siblings.end () && moving.count (*insertion)) ++insertion;
	before = insertion == siblings.end () ? std::shared_ptr<Route> () : *insertion;
	siblings.remove_if ([&] (std::shared_ptr<Route> r) { return moving.count (r); });
	siblings.insert (before ? std::find (siblings.begin (), siblings.end (), before) : siblings.end (), selected.begin (), selected.end ());

	std::map<std::shared_ptr<Route>, Connections> old, replacement;
	for (auto const& r : selected) {
		if (parent (s, r) == destination) continue; // Reordering must preserve manual routing.
		old[r] = connections (r->output ());
		auto out = old[r];
		if (out.empty ()) { error = "MIDI-only tracks cannot be routed through an audio group."; return false; }
		if (destination) {
			if (out.size () != destination->input ()->n_ports ().n_audio ()) {
				error = "The track and group need matching audio channel layouts."; return false;
			}
			for (size_t n = 0; n < out.size (); ++n) out[n] = {destination->input ()->ports ()->port (DataType::AUDIO, n)->name ()};
		} else {
			// Leaving nested groups bypasses their buses, following their current
			// destinations. Keep manually overridden outputs and MIDI connections.
			for (auto const& ancestor : ancestors (s, r)) {
				auto dest = connections (ancestor->output ());
				for (auto& channel : out) {
					std::vector<std::string> expanded;
					for (auto const& name : channel) {
						auto port = s.engine ().get_port_by_name (name);
						bool replaced = false;
						for (uint32_t n = 0; n < ancestor->input ()->n_ports ().n_audio (); ++n) {
							if (port != ancestor->input ()->ports ()->port (DataType::AUDIO, n)) continue;
							if (n >= dest.size ()) { error = "Restore matching group input/output channels before moving this track out."; return false; }
							expanded.insert (expanded.end (), dest[n].begin (), dest[n].end ()); replaced = true; break;
						}
						if (!replaced) expanded.push_back (name);
					}
					std::sort (expanded.begin (), expanded.end ());
					expanded.erase (std::unique (expanded.begin (), expanded.end ()), expanded.end ());
					channel = expanded;
				}
			}
		}
		replacement[r] = out;
	}
	// Inspect actual connections (including sends/sidechains), not the last
	// processed graph, which can lag an edit. Reject downstream destinations.
	auto all = s.get_routes ();
	std::map<std::string, std::shared_ptr<Route>> inputs;
	for (auto const& r : *all) for (auto const& weak : r->all_inputs ()) if (auto io = weak.lock ()) {
		for (auto const& port : *io->ports ()) inputs[s.engine ().make_port_name_non_relative (port->name ())] = r;
	}
	std::set<std::shared_ptr<Route>> seen;
	std::function<bool(std::shared_ptr<Route>)> reaches_moving = [&] (std::shared_ptr<Route> r) {
		if (moving.count (r)) return true;
		if (!seen.insert (r).second) return false;
		for (auto const& next : *all) if (r->direct_feeds_according_to_reality (next) && reaches_moving (next)) return true;
		return false;
	};
	for (auto const& entry : replacement) for (auto const& channel : entry.second) for (auto const& name : channel) {
		auto found = inputs.find (s.engine ().make_port_name_non_relative (name));
		if (found != inputs.end () && reaches_moving (found->second)) {
			error = "This move would create an audio feedback loop. Change the routing first."; return false;
		}
	}
	const auto reach_before = reachability (s);
	for (auto const& entry : replacement) if (!reconnect (entry.first->output (), entry.second)) {
		bool restored = true;
		for (auto const& restore : old) restored = reconnect (restore.first->output (), restore.second) && restored;
		error = restored ? "Could not connect the track; the original routing was restored." : "Could not connect or fully restore the track outputs. Check audio routing.";
		return false;
	}
	propagate_solo_to_new_paths (s, reach_before);
	PresentationInfo::ChangeSuspender cs;
	for (auto const& r : selected) metadata (r).set_property ("parent", destination ? destination->id ().to_s () : std::string ());
	// Assign unique orders across all stripables before normalizing the tree.
	StripableList ordered; s.get_stripables (ordered); ordered.sort (Stripable::Sorter ());
	ordered.remove_if ([&] (std::shared_ptr<Stripable> strip) { auto r = std::dynamic_pointer_cast<Route> (strip); return r && !r->is_singleton () && parent (s, r) == destination; });
	auto pos = destination ? std::find (ordered.begin (), ordered.end (), destination) : ordered.begin ();
	if (destination && pos != ordered.end ()) ++pos;
	ordered.insert (pos, siblings.begin (), siblings.end ());
	PresentationInfo::order_t order = 0;
	for (auto const& strip : ordered) strip->set_presentation_order (order++);
	normalize_order (s); s.set_dirty ();
	return true;
}

bool GroupTracks::ungroup (Session& s, std::shared_ptr<Route> bus, std::string& error)
{
	if (!is_group (bus)) { error = "Select a group track to ungroup."; return false; }
	auto members = children (s, bus);
	auto dest = connections (bus->output ());
	std::map<std::shared_ptr<Route>, Connections> old, replacement;
	// Inputs from outside the group (including sends) need an explicit routing decision.
	std::set<std::string> member_ports;
	for (auto const& r : members) for (auto const& p : *r->output ()->ports ()) member_ports.insert (s.engine ().make_port_name_non_relative (p->name ()));
	for (auto const& channel : connections (bus->input ())) for (auto const& name : channel) {
		if (!member_ports.count (name)) { error = "Disconnect inputs or sends from outside this group before ungrouping."; return false; }
	}
	for (auto const& r : members) {
		old[r] = connections (r->output ());
		auto out = old[r];
		for (auto& channel : out) {
			std::vector<std::string> expanded;
			for (auto const& name : channel) {
				auto port = s.engine ().get_port_by_name (name);
				bool replaced = false;
				for (uint32_t n = 0; n < bus->input ()->n_ports ().n_audio (); ++n) {
					if (port != bus->input ()->ports ()->port (DataType::AUDIO, n)) continue;
					if (n >= dest.size ()) { error = "The group channel layout changed; restore matching input/output channels before ungrouping."; return false; }
					expanded.insert (expanded.end (), dest[n].begin (), dest[n].end ());
					replaced = true;
					break;
				}
				if (!replaced) expanded.push_back (name); // Preserve manual child output overrides.
			}
			std::sort (expanded.begin (), expanded.end ());
			expanded.erase (std::unique (expanded.begin (), expanded.end ()), expanded.end ());
			channel = expanded;
		}
		replacement[r] = out;
	}
	for (auto const& r : members) {
		if (!reconnect (r->output (), replacement[r])) {
			for (auto const& restore : members) reconnect (restore->output (), old[restore]);
			error = "Could not restore child outputs; the group has been kept.";
			return false;
		}
	}
	for (auto const& r : members) metadata (r).set_property ("parent", parent_id (bus));
	s.remove_route (bus);
	normalize_order (s);
	s.set_dirty ();
	return true;
}
