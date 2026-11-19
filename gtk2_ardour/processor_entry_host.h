/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <memory>

namespace Gtk { class Widget; class SelectionData; }
namespace ARDOUR { class Processor; }
class ProcessorEntry;

/** Container operations used by a processor row. Model edits belong to the host. */
class ProcessorEntryHost {
public:
	virtual ~ProcessorEntryHost () = default;
	virtual void all_visible_processors_active (bool) = 0;
	virtual void setup_routing_feeds () = 0;
	virtual void update_gui_object_state (ProcessorEntry*, bool emit = false) = 0;
	virtual void edit_processor (std::shared_ptr<ARDOUR::Processor>) = 0;
	virtual void generic_edit_processor (std::shared_ptr<ARDOUR::Processor>) = 0;
	virtual Gtk::Widget* entry_toplevel () = 0;
	virtual bool entry_drag_data (Gtk::SelectionData&, std::shared_ptr<ARDOUR::Processor>) { return false; }
};
