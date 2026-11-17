#pragma once
#include <string>
namespace PBD { class EventLoop; }
namespace ArdourSurface {
/* Called by the HTTP thread; GTK work is marshalled to the UI event loop. */
std::string mcp_ui_call (PBD::EventLoop*, std::string const& tool,
                         std::string const& action, std::string const& group,
                         std::string const& key, std::string const& window);
}
