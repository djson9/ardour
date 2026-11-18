/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ardour/group_tracks.h"
#include "ardour/session.h"
#include "ardour/selection.h"
#include "widgets/choice.h"
#include "gtkmm2ext/colors.h"
#include "ardour_message.h"
#include "editor.h"
#include "route_time_axis.h"
#include "selection.h"
#include "verbose_cursor.h"
#include "canvas/item.h"
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
	// Give the parent its own palette color so the inset remains visible
	// against the first child's name block as well as the later children.
	group->presentation_info ().set_color (Gtkmm2ext::gdk_color_to_rgba (AxisView::round_robin_palette_color ()));
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

void Editor::update_group_drop (int x, int y)
{
	if (track_drag->drop_highlight) track_drag->drop_highlight->highlight_drop (false);
	track_drag->drop_highlight = nullptr;
	track_drag->destination.reset (); track_drag->before.reset ();
	track_drag->valid_drop = false;
	std::string hint;
	if (x < 0 || x >= edit_controls_vbox.get_width () || y < 0) { _verbose_cursor->hide (); return; }
	double bottom = 0;
	for (auto* tv : track_views) {
		auto* rtv = dynamic_cast<RouteTimeAxisView*> (tv);
		if (!rtv || !tv->marked_for_display () || tv->hidden () || rtv->route ()->is_singleton ()) continue;
		bottom = std::max (bottom, tv->y_position () + tv->effective_height ());
		if (y < tv->y_position () || y >= tv->y_position () + tv->effective_height ()) continue;
		auto route = rtv->route ();
		const double fraction = (y - tv->y_position ()) / tv->current_height ();
		if (fraction > 1.) break; // An automation lane isn't a header target.
		if (GroupTracks::is_group (route) && fraction >= .2 && fraction < .8) {
			track_drag->destination = route;
			hint = string_compose (_("Move into %1"), route->name ());
		} else {
			track_drag->destination = GroupTracks::parent (*_session, route);
			if (fraction < .5) track_drag->before = route;
			else {
				RouteList siblings;
				for (auto const& r : *_session->get_routes ()) if (!r->is_singleton () && GroupTracks::parent (*_session, r) == track_drag->destination) siblings.push_back (r);
				siblings.sort (Stripable::Sorter ());
				auto i = std::find (siblings.begin (), siblings.end (), route);
				if (i != siblings.end () && ++i != siblings.end ()) track_drag->before = *i;
			}
			hint = string_compose (fraction < .5 ? _("Move before %1") : _("Move after %1"), route->name ());
		}
		track_drag->valid_drop = true;
		track_drag->drop_highlight = tv; tv->highlight_drop (true);
		break;
	}
	if (!track_drag->valid_drop && y >= bottom) {
		track_drag->valid_drop = true;
		hint = _("Move outside groups");
	}
	if (track_drag->valid_drop) {
		_verbose_cursor->set (hint); _verbose_cursor->show ();
		_verbose_cursor->canvas_item ()->set_position (ArdourCanvas::Duple (20, 20));
	} else _verbose_cursor->hide ();
}
