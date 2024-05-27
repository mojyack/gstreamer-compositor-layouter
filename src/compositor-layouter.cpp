#include <optional>

#include "compositor-layouter.hpp"

#include "macros/autoptr.hpp"
#include "macros/unwrap.hpp"
#include "util/assert.hpp"

namespace {
declare_autoptr(GstCaps, GstCaps, gst_caps_unref);

enum class CapsEventHandlerResult {
    Continue,
    Uninstall,
};

using CapsEventHandler = std::function<CapsEventHandlerResult(GstCaps*)>;

auto install_caps_event_probe(GstPad* const pad, const CapsEventHandler handler) -> void {
    struct Context {
        CapsEventHandler handler;
    };

    struct Callbacks {
        static auto on_pad_probe(GstPad* const /*pad*/,
                                 GstPadProbeInfo* const info,
                                 gpointer const         data) -> GstPadProbeReturn {
            const auto event = gst_pad_probe_info_get_event(info);
            if(GST_EVENT_TYPE(event) != GST_EVENT_CAPS) {
                return GST_PAD_PROBE_OK;
            }

            auto caps = (GstCaps*)(nullptr);
            gst_event_parse_caps(event, &caps);

            auto& context = *std::bit_cast<Context*>(data);
            switch(context.handler(caps)) {
            case CapsEventHandlerResult::Continue:
                return GST_PAD_PROBE_OK;
            case CapsEventHandlerResult::Uninstall:
                delete &context;
                return GST_PAD_PROBE_REMOVE;
            }
        }

        static auto destory(const gpointer data) -> void {
            const auto context = std::bit_cast<Context*>(data);
            delete context;
        }
    };

    auto context = new Context{.handler = handler};
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, Callbacks::on_pad_probe, context, Callbacks::destory);
}

auto get_width_and_height_from_caps(GstCaps* const caps) -> std::optional<std::pair<int, int>> {
    for(auto i = guint(0); i < gst_caps_get_size(caps); i += 1) {
        const auto structure = gst_caps_get_structure(caps, i);
        auto       width     = int();
        auto       height    = int();
        if(!gst_structure_get_int(structure, "width", &width) ||
           !gst_structure_get_int(structure, "height", &height)) {
            continue;
        }

        return std::pair<int, int>{width, height};
    }
    return std::nullopt;
}

auto rounddiv(const auto a, const auto b) -> int {
    return (a + b - 1) / b;
}

auto calculate_best_grid_numbers(const std::array<int, 2> screen_size, const std::array<double, 2> preferred_grid_size, const size_t required_grids) -> std::array<int, 2> {
    auto max_scale = 0.0;
    for(auto rows = 1;; rows += 1) {
        const auto columns   = rounddiv(required_grids, rows);
        const auto grid_size = std::array{screen_size[0] / columns, screen_size[1] / rows};
        const auto scale     = std::min(grid_size[0] / preferred_grid_size[0], grid_size[1] / preferred_grid_size[1]);
        if(scale <= max_scale) {
            return {rows - 1, rounddiv(required_grids, rows - 1)};
        }
        max_scale = scale;
    }
}

struct CompositorSinkBlockCallbackData {
    CompositorLayouter*                         self;
    std::unique_ptr<CompositorLayouter::Source> source;

    std::function<void(GstPad*)> pad_delete_callback;
};

auto compositor_sink_block_callback(GstPad* const /*pad*/, GstPadProbeInfo* const /*info*/, CompositorSinkBlockCallbackData* const args) -> GstPadProbeReturn {
    auto& self = *args->self;
    // unlink upstream
    DYN_ASSERT(gst_pad_unlink(args->source->upstream_pad.get(), args->source->compositor_pad.get()) == TRUE);
    gst_element_release_request_pad(self.compositor, args->source->compositor_pad.get());
    if(args->pad_delete_callback) {
        args->pad_delete_callback(args->source->upstream_pad.get());
    }
    delete args;
    return GST_PAD_PROBE_REMOVE;
}
} // namespace

auto CompositorLayouter::add_src(AutoGstObject<GstPad> upstream_pad, bool mute) -> Source* {
    // generate new pad name
    auto sink_name = std::array<char, 16>();
    snprintf(sink_name.data(), sink_name.size(), "sink_%u", sink_id_serial.fetch_add(1));

    // request new pad from compositor
    unwrap_pp_mut(factory, gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(compositor), "sink_%u"));
    auto compositor_pad = AutoGstObject(gst_element_request_pad(compositor, &factory, sink_name.data(), NULL));
    assert_p(compositor_pad.get() != NULL);
    g_object_set(compositor_pad.get(), "max-last-buffer-repeat", -1, NULL);

    // link src to compositor
    assert_p(gst_pad_link(upstream_pad.get(), compositor_pad.get()) == GST_PAD_LINK_OK);

    const auto source_ptr = new Source{
        .upstream_pad   = std::move(upstream_pad),
        .compositor_pad = std::move(compositor_pad),
        .width          = -1,
        .height         = -1,
        .muted          = mute,
    };
    sources.emplace_back(source_ptr);
    auto& source = *source_ptr;

    // get width and height
    const auto handle_caps = [this, &source](GstCaps* const caps) -> CapsEventHandlerResult {
        unwrap_ov(size, get_width_and_height_from_caps(caps), const, CapsEventHandlerResult::Uninstall);
        source.width  = size.first;
        source.height = size.second;
        if(verbose) {
            PRINT("source ", &source, ": size=(", source.width, ",", source.height, ")");
        }
        if(!source.muted) {
            layout_sources();
        }
        return CapsEventHandlerResult::Continue;
    };

    const auto pad  = source.upstream_pad.get();
    const auto caps = AutoGstCaps(gst_pad_get_current_caps(pad));
    if(!caps) {
        install_caps_event_probe(pad, handle_caps);
    } else {
        handle_caps(caps.get());
    }

    return &source;
}

auto CompositorLayouter::mute_unmute_src(Source* const source_ptr, const bool mute) -> void {
    auto& source = *source_ptr;
    g_object_set(source.compositor_pad.get(),
                 "alpha", mute ? 0.0 : 1.0,
                 NULL);
    source.muted = mute;
    if(source.width != -1 && source.height != -1) {
        layout_sources();
    }
}

auto CompositorLayouter::remove_src(const Source* const source_ptr, const std::function<void(GstPad*)> pad_delete_callback) -> void {
    auto source = std::unique_ptr<Source>();
    for(auto i = sources.begin(); i != sources.end(); i += 1) {
        if(i->get() == source_ptr) {
            source = std::move(*i);
            sources.erase(i);
            break;
        }
    }
    assert_n(source);
    if(!source->muted) {
        layout_sources();
    }

    const auto args = new CompositorSinkBlockCallbackData{
        .self                = this,
        .source              = std::move(source),
        .pad_delete_callback = pad_delete_callback,
    };
    gst_pad_add_probe(args->source->upstream_pad.get(),
                      GST_PAD_PROBE_TYPE_IDLE,
                      GstPadProbeCallback(compositor_sink_block_callback),
                      args,
                      NULL);
}

auto CompositorLayouter::layout_sources() -> void {
    auto valid_sources = std::vector<Source*>();
    for(auto& source : sources) {
        if(source->width != -1 && source->height != -1 && !source->muted) {
            valid_sources.push_back(source.get());
        }
    }
    if(valid_sources.empty()) {
        return;
    }

    // calculate average resolution
    auto average_resolution = std::array{0.0, 0.0};
    for(const auto& source : valid_sources) {
        average_resolution[0] += source->width;
        average_resolution[1] += source->height;
    }
    average_resolution[0] /= valid_sources.size();
    average_resolution[1] /= valid_sources.size();

    const auto [rows, cols] = calculate_best_grid_numbers({output_width, output_height}, average_resolution, valid_sources.size());
    const auto index_limit  = std::min(int(valid_sources.size()), rows * cols);
    const auto grid_width   = output_width / cols;
    const auto grid_height  = output_height / rows;
    for(auto r = 0; r < rows; r += 1) {
        for(auto c = 0; c < cols; c += 1) {
            const auto i = r * cols + c;
            if(i >= index_limit) {
                return;
            }
            const auto& source = *valid_sources[i];
            const auto  scale  = std::min(1. * grid_width / source.width, 1. * grid_height / source.height);
            const auto  width  = source.width * scale;
            const auto  height = source.height * scale;
            const auto  xpos   = c * grid_width + (grid_width - width) / 2;
            const auto  ypos   = r * grid_height + (grid_height - height) / 2;
            g_object_set(source.compositor_pad.get(),
                         "xpos", int(xpos),
                         "ypos", int(ypos),
                         "width", int(width),
                         "height", int(height),
                         NULL);
            if(verbose) {
                PRINT("source ", &source, ": layout at pos=(", int(xpos), ",", int(ypos), ") size=(", int(width), ",", int(height), ")");
            }
        }
    }
}

CompositorLayouter::CompositorLayouter(GstElement* const compositor) : compositor(compositor) {
    const auto compositor_src = AutoGstObject(gst_element_get_static_pad(compositor, "src"));
    DYN_ASSERT(compositor_src.get() != NULL, "compositor does not have src pad");

    const auto handle_caps = [this](GstCaps* const caps) -> CapsEventHandlerResult {
        unwrap_ov(size, get_width_and_height_from_caps(caps), const, CapsEventHandlerResult::Uninstall);
        output_width  = size.first;
        output_height = size.second;
        if(verbose) {
            PRINT("output: size=(", output_width, ",", output_height, ")");
        }
        layout_sources();
        return CapsEventHandlerResult::Continue;
    };

    install_caps_event_probe(compositor_src.get(), handle_caps);
}
