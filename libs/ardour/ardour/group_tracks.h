/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <memory>
#include <string>
#include "ardour/libardour_visibility.h"
#include "ardour/types.h"

namespace ARDOUR {
class Route;
class Session;

/* Arrangement hierarchy over ordinary buses. No processing-time dispatcher. */
class LIBARDOUR_API GroupTracks {
public:
	static bool is_group (std::shared_ptr<Route> const&);
	static bool collapsed (std::shared_ptr<Route> const&);
	static void set_collapsed (Session&, std::shared_ptr<Route> const&, bool);
	static std::string parent_id (std::shared_ptr<Route> const&);
	static std::shared_ptr<Route> parent (Session&, std::shared_ptr<Route> const&);
	static RouteList ancestors (Session&, std::shared_ptr<Route> const&);
	static bool folded (Session&, std::shared_ptr<Route> const&);
	static RouteList children (Session&, std::shared_ptr<Route> const&, bool recursive = false);
	static void normalize_order (Session&);
	static void move (Session&, RouteList const&, bool up);
	static std::shared_ptr<Route> create (Session&, RouteList, std::string& error);
	static bool ungroup (Session&, std::shared_ptr<Route>, std::string& error);
};
}
