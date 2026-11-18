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
#include "ardour/port.h"
#include "ardour/route.h"
#include "ardour/session.h"

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
