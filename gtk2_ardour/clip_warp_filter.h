#pragma once
#include <functional>
#include <memory>
#include <string>
#include "ardour/filter.h"
#include "clip_warp.h"

namespace ARDOUR { struct TimeFXRequest; }

/* One drag reads its immutable original source once. Later pointer updates
 * reuse this private input file; each worker gets a fresh output file. */
struct ClipWarpInputCache {
 std::string path;
 const ARDOUR::Region* region = nullptr;
 ARDOUR::samplepos_t start = 0;
 ARDOUR::samplecnt_t count = 0;
 unsigned channels = 0;
 bool valid = false;
 ClipWarpInputCache () = default;
 ClipWarpInputCache (ClipWarpInputCache const&) = delete;
 ClipWarpInputCache& operator= (ClipWarpInputCache const&) = delete;
 void ensure ();
 ~ClipWarpInputCache ();
};

class ClipWarpFilter : public ARDOUR::Filter {
 ARDOUR::TimeFXRequest& request;
 ClipWarpSettings settings;
 std::string& failure;
 std::function<bool()> stop;
 bool preview;
 ClipWarpInputCache* input_cache;
 std::function<void(std::shared_ptr<ARDOUR::Region>, ARDOUR::samplecnt_t)> preview_update;
 bool cancelled () const;
 template<class T> static std::string text (T);
 struct Scratch;
 void worker (const Scratch&, std::string const& input_path, int rate, unsigned channels, double ratio, std::function<void()> tick = {});
public:
 ClipWarpFilter (ARDOUR::Session& s, ARDOUR::TimeFXRequest& req, ClipWarpSettings const& options, std::string& error, std::function<bool()> cancel = {}, bool is_preview = false, std::function<void(std::shared_ptr<ARDOUR::Region>, ARDOUR::samplecnt_t)> update = {}, ClipWarpInputCache* cache = nullptr)
 : Filter (s), request (req), settings (options), failure (error), stop (cancel), preview (is_preview), input_cache (is_preview ? cache : nullptr), preview_update (update) {}
 int run (std::shared_ptr<ARDOUR::Region>, PBD::Progress*) override;
};
