#pragma once
#include <functional>
#include <vector>

#include <gst/gst.h>

#include "gutil/auto-gst-object.hpp"

struct CompositorLayouter {
    struct Source {
        AutoGstObject<GstPad> upstream_pad;
        AutoGstObject<GstPad> compositor_pad;

        int  width;
        int  height;
        bool muted;
    };

    GstElement*      compositor;
    int              output_width   = 300;
    int              output_height  = 200;
    std::atomic_uint sink_id_serial = 0;
    bool             verbose        = false;

    std::vector<std::unique_ptr<Source>> sources;

    auto add_src(AutoGstObject<GstPad> upstream_pad, bool mute) -> Source*;
    auto mute_unmute_src(Source* source_ptr, bool mute) -> void;
    auto remove_src(const Source* const source_ptr, const std::function<void(GstPad*)> pad_delete_callback) -> void;
    auto layout_sources() -> void;

    CompositorLayouter(GstElement* compositor);
};
