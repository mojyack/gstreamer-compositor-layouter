#include <thread>

#include "compositor-layouter.hpp"

#include <gst/gst.h>
#include <gst/gstmessage.h>

#include "gutil/auto-gst-object.hpp"
#include "gutil/pipeline-helper.hpp"
#include "macros/autoptr.hpp"
#include "macros/unwrap.hpp"
#include "util/assert.hpp"

namespace {
declare_autoptr(GString, gchar, g_free);
declare_autoptr(GstMessage, GstMessage, gst_message_unref);
declare_autoptr(GstCaps, GstCaps, gst_caps_unref);

auto compositor_pad_added_handler(GstElement* const /*compositor*/, GstPad* const pad, gpointer const /*data*/) -> void {
    const auto name_g = AutoGString(gst_object_get_name(GST_OBJECT(pad)));
    const auto name   = std::string_view(name_g.get());

    PRINT("pad added name=", name);
}

auto compositor_pad_removed_handler(GstElement* const /*compositor*/, GstPad* const pad, gpointer const /*data*/) -> void {
    const auto name_g = AutoGString(gst_object_get_name(GST_OBJECT(pad)));
    const auto name   = std::string_view(name_g.get());
    PRINT("pad removed name=", name);
}

auto manage() -> bool {
    const auto pipeline = AutoGstObject(gst_pipeline_new(NULL));
    assert_b(pipeline.get() != NULL);

    unwrap_pb_mut(compositor, add_new_element_to_pipeine(pipeline.get(), "compositor"));
    unwrap_pb_mut(videoconvert, add_new_element_to_pipeine(pipeline.get(), "videoconvert"));
    unwrap_pb_mut(waylandsink, add_new_element_to_pipeine(pipeline.get(), "waylandsink"));

    auto layouter    = CompositorLayouter(&compositor);
    layouter.verbose = true;

    g_object_set(&waylandsink, "async", FALSE, NULL);
    g_signal_connect(G_OBJECT(&compositor), "pad-added", G_CALLBACK(compositor_pad_added_handler), &layouter);
    g_signal_connect(G_OBJECT(&compositor), "pad-removed", G_CALLBACK(compositor_pad_removed_handler), &layouter);

    assert_b(gst_element_link_pads(&compositor, NULL, &videoconvert, NULL) == TRUE);
    assert_b(gst_element_link_pads(&videoconvert, NULL, &waylandsink, NULL) == TRUE);

    auto w = std::thread([&pipeline, &layouter]() {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        unwrap_pb_mut(videotestsrc1, add_new_element_to_pipeine(pipeline.get(), "videotestsrc"));
        unwrap_pb(src1, layouter.add_src(gst_element_get_static_pad(&videotestsrc1, "src")));

        std::this_thread::sleep_for(std::chrono::seconds(2));
        unwrap_pb_mut(videotestsrc2, add_new_element_to_pipeine(pipeline.get(), "videotestsrc"));
        unwrap_pb(src2, layouter.add_src(gst_element_get_static_pad(&videotestsrc2, "src")));

        std::this_thread::sleep_for(std::chrono::seconds(2));
        unwrap_pb_mut(videotestsrc3, add_new_element_to_pipeine(pipeline.get(), "videotestsrc"));
        unwrap_pb(src3, layouter.add_src(gst_element_get_static_pad(&videotestsrc3, "src")));

        std::this_thread::sleep_for(std::chrono::seconds(2));
        unwrap_pb_mut(videotestsrc4, add_new_element_to_pipeine(pipeline.get(), "videotestsrc"));
        unwrap_pb(src4, layouter.add_src(gst_element_get_static_pad(&videotestsrc4, "src")));

        std::this_thread::sleep_for(std::chrono::seconds(2));
        layouter.remove_src(&src4, {});
        std::this_thread::sleep_for(std::chrono::seconds(2));
        layouter.remove_src(&src3, {});
        std::this_thread::sleep_for(std::chrono::seconds(2));
        layouter.remove_src(&src2, {});
        std::this_thread::sleep_for(std::chrono::seconds(2));
        layouter.remove_src(&src1, {});
        return true;
    });

    return run_pipeline(pipeline.get());
}
} // namespace

auto main(int argc, char* argv[]) -> int {
    gst_init(&argc, &argv);
    manage();
    return 0;
}
