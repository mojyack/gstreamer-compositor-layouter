#include <thread>

#include "compositor-layouter.hpp"

#include <gst/gst.h>
#include <gst/gstmessage.h>

#include "gutil/auto-gst-object.hpp"
#include "gutil/pipeline-helper.hpp"
#include "macros/autoptr.hpp"
#include "macros/unwrap.hpp"
#include "util/assert.hpp"
#include "util/event.hpp"

namespace {
declare_autoptr(GString, gchar, g_free);
declare_autoptr(GstMessage, GstMessage, gst_message_unref);
declare_autoptr(GstCaps, GstCaps, gst_caps_unref);

auto manage() -> bool {
    const auto pipeline = AutoGstObject(gst_pipeline_new(NULL));
    assert_b(pipeline.get() != NULL);

    const auto waylandsink_caps = AutoGstCaps(gst_caps_new_simple("video/x-raw",
                                                                  "width", G_TYPE_INT, 800,
                                                                  "height", G_TYPE_INT, 600,
                                                                  NULL));

    unwrap_pb_mut(compositor, add_new_element_to_pipeine(pipeline.get(), "compositor"));
    unwrap_pb_mut(videoconvert, add_new_element_to_pipeine(pipeline.get(), "videoconvert"));
    unwrap_pb_mut(capsfilter, add_new_element_to_pipeine(pipeline.get(), "capsfilter"));
    g_object_set(&capsfilter, "caps", waylandsink_caps.get(), NULL);
    unwrap_pb_mut(waylandsink, add_new_element_to_pipeine(pipeline.get(), "waylandsink"));
    g_object_set(&waylandsink, "async", FALSE, NULL);

    assert_b(gst_element_link_pads(&compositor, NULL, &videoconvert, NULL) == TRUE);
    assert_b(gst_element_link_pads(&videoconvert, NULL, &capsfilter, NULL) == TRUE);
    assert_b(gst_element_link_pads(&capsfilter, NULL, &waylandsink, NULL) == TRUE);

    auto layouter    = CompositorLayouter(&compositor);
    layouter.verbose = true;

    auto worker = std::thread([&pipeline, &layouter]() -> bool {
        constexpr auto max_sources = 16;
        constexpr auto delay       = std::chrono::milliseconds(500);

        auto sources = std::array<CompositorLayouter::Source*, max_sources>();

        // add sources
        for(auto& source : sources) {
            unwrap_pb_mut(videotestsrc, add_new_element_to_pipeine(pipeline.get(), "videotestsrc"));
            unwrap_pb_mut(src, layouter.add_src(gst_element_get_static_pad(&videotestsrc, "src")));
            assert_b(gst_element_sync_state_with_parent(&videotestsrc) == TRUE);
            source = &src;
            std::this_thread::sleep_for(delay);
        }

        std::this_thread::sleep_for(delay);

        // mute
        for(const auto source : sources) {
            layouter.mute_unmute_src(source, true);
            std::this_thread::sleep_for(delay / 2);
        }

        // unmute
        for(const auto source : sources) {
            layouter.mute_unmute_src(source, false);
            std::this_thread::sleep_for(delay / 2);
        }

        // removing an element from a live pipeline is hard...
        auto delete_ready      = Event();
        auto element_to_delete = (GstElement*)(nullptr);
        auto delete_element    = [&delete_ready, &element_to_delete](GstPad* const pad) {
            assert_n(element_to_delete == nullptr, "unexpected element removal");
            unwrap_pn_mut(element, gst_pad_get_parent_element(pad));
            // we cannot delete element here.
            // because this function maybe called from its streaming thread.
            element_to_delete = &element;
            delete_ready.wakeup();
        };

        // remove elements
        for(auto& source : sources) {
            layouter.remove_src(source, delete_element);
            delete_ready.wait();
            delete_ready.clear();

            assert_b(gst_bin_remove(GST_BIN(pipeline.get()), element_to_delete) == TRUE);
            assert_b(gst_element_set_state(element_to_delete, GST_STATE_NULL) == GST_STATE_CHANGE_SUCCESS);
            gst_object_unref(element_to_delete);
            element_to_delete = nullptr;

            std::this_thread::sleep_for(delay);
        }

        return true;
    });

    const auto ret = run_pipeline(pipeline.get());
    worker.join();
    return ret;
}
} // namespace

auto main(int argc, char* argv[]) -> int {
    gst_init(&argc, &argv);
    manage();
    return 0;
}
