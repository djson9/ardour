/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ardour/group_tracks.h"
#include "ardour/session.h"
#include "ardour/selection.h"
#include "widgets/choice.h"
#include "ardour_message.h"
#include "editor.h"
#include "route_time_axis.h"
#include "selection.h"
#include "pbd/i18n.h"

using namespace ARDOUR;

void Editor::group_selected_tracks ()
{
	if (!_session) return;
	RouteList routes;
	for (auto* tv : selection->tracks) if (auto* rtv = dynamic_cast<RouteTimeAxisView*> (tv)) routes.push_back (rtv->route ());
	std::string error;
	auto group = GroupTracks::create (*_session, routes, error);
	if (!group) {
		ArdourMessageDialog message (error, false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK);
		message.run ();
		return;
	}
	queue_redisplay_track_views ();
	// RouteAdded can be marshalled to the GUI asynchronously. Select using the
	// model so the eventual view also starts selected.
	_session->selection ().select_stripable_and_maybe_group (group, SelectionSet);
}

void Editor::ungroup_selected_tracks ()
{
	if (!_session) return;
	RouteList groups;
	for (auto* tv : selection->tracks) {
		if (auto* rtv = dynamic_cast<RouteTimeAxisView*> (tv)) {
			if (GroupTracks::is_group (rtv->route ())) groups.push_back (rtv->route ());
		}
	}
	if (groups.empty ()) return;
	ArdourWidgets::Choice confirmation (_("Ungroup tracks"), _("Keep the child tracks and remove the selected group buses?\nGroup effects, automation, gain and mute settings will be removed.\nThis action cannot be undone."), {_ ("Cancel"), _ ("Ungroup")});
	if (confirmation.run () != 1) return;
	for (auto const& group : groups) {
		std::string error;
		if (!GroupTracks::ungroup (*_session, group, error)) {
			ArdourMessageDialog message (error, false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK);
			message.run ();
			break;
		}
	}
	queue_redisplay_track_views ();
}

void Editor::toggle_group_track (std::shared_ptr<Route> group)
{
	if (!_session || !GroupTracks::is_group (group)) return;
	const bool fold = !GroupTracks::collapsed (group);
	GroupTracks::set_collapsed (*_session, group, fold);
	if (fold) {
		bool selected_child = false;
		for (auto const& child : GroupTracks::children (*_session, group, true)) selected_child |= child->is_selected ();
		if (selected_child) _session->selection ().select_stripable_and_maybe_group (group, SelectionSet);
	}
	queue_redisplay_track_views ();
}
