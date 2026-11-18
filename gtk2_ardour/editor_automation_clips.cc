/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <ytkmm/comboboxtext.h>
#include <ytkmm/spinbutton.h>
#include <ytkmm/checkbutton.h>
#include <ytkmm/label.h>
#include "pbd/memento_command.h"
#include "pbd/stateful_diff_command.h"
#include "ardour/automation_clip.h"
#include "ardour/audio_track.h"
#include "ardour/audioregion.h"
#include "ardour/automation_control.h"
#include "ardour/automation_list.h"
#include "ardour/gain_control.h"
#include "ardour/playlist.h"
#include "ardour/plugin_insert.h"
#include "ardour/session.h"
#include "ardour_dialog.h"
#include "editor.h"
#include "route_time_axis.h"
#include "selection.h"
#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace Temporal;

void Editor::toggle_automation_clips () {
 if (!_session || !_automation_clips) { return; }
 _automation_clips_visible = !_automation_clips_visible;
 bool found = false;
 for (auto const& r : *_session->get_routes()) {
  if (!AutomationClips::is_track(r)) { continue; }
  found = true; r->presentation_info().set_hidden (!_automation_clips_visible);
 }
 if (!found && _automation_clips_visible) { add_automation_track(); }
 queue_redisplay_track_views();
}
void Editor::add_automation_track () {
 if (!_automation_clips) { return; }
 auto track = _automation_clips->new_track();
 if (!track) { return; }
 _automation_clips_visible = true;
 add_automation_clip (track);
 queue_redisplay_track_views();
}
void Editor::add_automation_clip (std::shared_ptr<Route> route) {
 auto track = std::dynamic_pointer_cast<AudioTrack>(route);
 if (!_automation_clips || !track) { return; }
 auto playlist = track->playlist();
 auto position = get_preferred_edit_position();
 /* One bar at the edit point; native trim/duplicate operations change its length. */
 auto length = timecnt_t (Temporal::Beats (4,0), position);
 begin_reversible_command (_("Add automation clip"));
 playlist->clear_changes();
 _automation_clips->add_clip (track, position, length);
 add_command (new PBD::StatefulDiffCommand (playlist));
 commit_reversible_command();
}
void Editor::edit_automation_clip_targets (std::shared_ptr<Route> track) {
 if (!_automation_clips || !AutomationClips::is_track(track)) { return; }
 ArdourDialog dialog (_("Automation clip targets"), true);
 Gtk::VBox body (false, 10);
 body.set_border_width (12);
 Gtk::Label instructions (_("One clip controls every linked target. Select a target to add or edit its mapping.\nRanges use the target's native units; Reverse swaps the clip's low and high values."));
 instructions.set_alignment (0, .5);
 Gtk::ComboBoxText target_choice;
 Gtk::HBox range (false, 8);
 Gtk::Label min_label (_("Minimum")), max_label (_("Maximum"));
 Gtk::SpinButton minimum (0, 4), maximum (0, 4);
 Gtk::CheckButton reverse (_("Reverse direction"));
 Gtk::Label linked, status;
 linked.set_alignment (0,.5); status.set_alignment (0,.5);
 linked.set_line_wrap(true); status.set_line_wrap(true);
 range.pack_start (min_label, false, false); range.pack_start (minimum);
 range.pack_start (max_label, false, false); range.pack_start (maximum);
 body.pack_start(instructions, false, false); body.pack_start(target_choice, false, false);
 body.pack_start(range, false, false); body.pack_start(reverse, false, false);
 body.pack_start(linked, false, false); body.pack_start(status, false, false);
 dialog.get_vbox()->pack_start(body);
 dialog.add_button (_("Unlink target"), 2);
 dialog.add_button (_("Apply mapping"), 1);
 dialog.add_button (_("Close"), Gtk::RESPONSE_CLOSE);
 std::vector<std::shared_ptr<AutomationControl>> controls;
 std::vector<std::string> names;
 auto append = [&] (std::shared_ptr<AutomationControl> c, std::string const& name) {
  if (!c || !c->alist() || (c->flags() & PBD::Controllable::NotAutomatable)) { return; }
  controls.push_back(c); names.push_back(name); target_choice.append_text(name);
 };
 for (auto const& r : *_session->get_routes()) {
  if (AutomationClips::is_track(r)) { continue; }
  append (r->gain_control(), r->name() + " / Volume (linear gain)");
  append (r->pan_azimuth_control(), r->name() + " / Pan");
  append (r->pan_width_control(), r->name() + " / Width");
  r->foreach_processor ([&] (std::weak_ptr<Processor> wp) {
   auto pi = std::dynamic_pointer_cast<PluginInsert>(wp.lock());
   if (!pi) { return; }
   for (auto const& p : pi->what_can_be_automated()) {
    append(pi->automation_control(p,true),r->name()+" / "+pi->name()+" / "+pi->describe_parameter(p));
   }
  });
 }
 auto update = [&] {
  int index = target_choice.get_active_row_number(); if (index < 0) { return; }
  auto c = controls[index]; auto const& d = c->desc();
  minimum.set_range(d.lower,d.upper); maximum.set_range(d.lower,d.upper);
  double step = d.integer_step ? 1 : std::max(0.0001, (d.upper-d.lower)/100.);
  minimum.set_increments(step,step*10); maximum.set_increments(step,step*10);
  minimum.set_value(d.lower); maximum.set_value(d.upper); reverse.set_active(false);
  std::string text = "Linked targets:";
  for (auto const& m : _automation_clips->mappings()) {
   if (m.track != track->id().to_s()) { continue; }
   auto it = std::find_if(controls.begin(),controls.end(),[&] (std::shared_ptr<AutomationControl> c) { return c->id().to_s()==m.target; });
   if (it != controls.end()) { text += "\n" + names[it-controls.begin()] + (m.inverted ? " (reverse)" : ""); }
   if (m.target == c->id().to_s()) { minimum.set_value(m.minimum); maximum.set_value(m.maximum); reverse.set_active(m.inverted); }
  }
  linked.set_text(text); status.set_text("");
 };
 target_choice.signal_changed().connect(update);
 target_choice.set_active(0);
 dialog.set_default_size(600,-1); dialog.show_all();
 while (true) {
  int response = dialog.run(); if (response != 1 && response != 2) { break; }
  int index = target_choice.get_active_row_number(); if (index < 0) { continue; }
  auto c = controls[index];
  std::unique_ptr<XMLNode> before (&_automation_clips->get_state());
  if (response == 1) {
   std::string error;
   if (!_automation_clips->map(track,c,minimum.get_value(),maximum.get_value(),reverse.get_active(),error)) { status.set_text(error); continue; }
  } else {
   auto const& mappings = _automation_clips->mappings();
   auto own = std::find_if(mappings.begin(),mappings.end(),[&] (AutomationClips::Mapping const& m) { return m.track == track->id().to_s() && m.target == c->id().to_s(); });
   if (own == mappings.end()) { status.set_text("This target is not linked to this track."); continue; }
   _automation_clips->unmap(c->id().to_s());
  }
  begin_reversible_command (_("Change automation clip targets"));
  add_command(new MementoCommand<AutomationClips>(*_automation_clips,before.release(),&_automation_clips->get_state()));
  commit_reversible_command(); update();
 }
}

void Editor::edit_selected_automation_targets () {
 for (auto view : selection->tracks) {
  auto r = dynamic_cast<RouteTimeAxisView*>(view);
  if (r && AutomationClips::is_track(r->route())) { edit_automation_clip_targets(r->route()); return; }
 }
 for (auto r : *_session->get_routes()) {
  if (AutomationClips::is_track(r)) { edit_automation_clip_targets(r); return; }
 }
}
