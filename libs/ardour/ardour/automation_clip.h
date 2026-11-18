/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "pbd/statefuldestructible.h"
#include "pbd/signals.h"
#include "ardour/types.h"
#include "ardour/parameter_descriptor.h"

namespace ARDOUR {
class Session; class Route; class AudioTrack; class AudioRegion; class AutomationControl;

/* Edits are compiled into ordinary target automation; no realtime dispatcher. */
class LIBARDOUR_API AutomationClips : public PBD::StatefulDestructible {
public:
 struct Mapping {
  std::string track, target;
  double minimum = 0, maximum = 1, baseline = 0;
  bool inverted = false;
  std::shared_ptr<XMLNode> original;
 };
 explicit AutomationClips (Session&);
 ~AutomationClips ();
 static bool is_track (std::shared_ptr<Route> const&);
 static bool is_clip (std::shared_ptr<AudioRegion> const&);
 static ParameterDescriptor descriptor ();
 std::shared_ptr<AudioTrack> new_track ();
 std::shared_ptr<AudioRegion> add_clip (std::shared_ptr<AudioTrack>, timepos_t const&, timecnt_t const&);
 std::vector<Mapping> const& mappings () const { return _mappings; }
 /* Range endpoints are in the target's native units. Existing automation is protected. */
 bool map (std::shared_ptr<Route>, std::shared_ptr<AutomationControl>, double, double, bool, std::string& error);
 void unmap (std::string const& target);
 void refresh ();
 XMLNode& get_state () const override;
 int set_state (XMLNode const&, int) override;
private:
 Session& _session;
 bool _refreshing = false;
 std::vector<Mapping> _mappings;
 PBD::ScopedConnectionList _connections;
 std::shared_ptr<AutomationControl> resolve (std::string const&) const;
 void persist ();
 void restore (Mapping const&);
};
}
