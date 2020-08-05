
#include <image_transport/image_transport.h>
#include <nodelet/nodelet.h>
#include <ros/ros.h>
#include <sensor_msgs/image_encodings.h>
#include <thread>

#include <gst/app/app.h>
#include <gst/gst.h>

namespace gst_video_server {

namespace enc = sensor_msgs::image_encodings;

GstClockTime operator"" _sec(unsigned long long sec) {
    return sec * GST_SECOND;
}
GstClockTime operator"" _ms(unsigned long long millisec) {
    return millisec * GST_MSECOND;
}

/**
 * Video server nodelet class
 */
class GstVideoServerNodelet : public nodelet::Nodelet {
   public:
    GstVideoServerNodelet();
    ~GstVideoServerNodelet();

    virtual void onInit();

   private:
    std::unique_ptr<image_transport::ImageTransport> image_transport_;
    image_transport::Subscriber image_sub_;

    //! GStreamer pipeline string
    std::string gsconfig_;

    //! Restart counter
    size_t restart_counter_;
    static constexpr size_t RESTART_LIM = 5;

    //! event loop, needed for gstreamer callbacks
    GMainLoop *loop_;
    std::thread loop_thread_;

    //! GStreamer pipeline
    GstElement *pipeline_;
    GstElement *appsrc_;
    guint bus_watch_id_;

    //! create and initialize pipeline object
    bool configure_pipeline();

    //! create GstCaps for sensor_msgs::Image
    GstCaps *gst_caps_new_from_image(const sensor_msgs::Image::ConstPtr &msg);

    //! calculate gst time from image stamp
    // GstClockTime gst_time_from_stamp(const ros::Time &stamp);

    //! create GstSample from sendor_msgs::Image
    GstSample *gst_sample_new_from_image(
        const sensor_msgs::Image::ConstPtr &msg);

    //! image_raw topic callback
    void image_cb(const sensor_msgs::Image::ConstPtr &msg);

    //! gstreamer error printer
    static gboolean bus_message_cb_wrapper(GstBus *bus, GstMessage *message,
                                           gpointer data);
    gboolean bus_message_cb(GstBus *bus, GstMessage *message);
};

GstVideoServerNodelet::GstVideoServerNodelet()
    : nodelet::Nodelet(),
      restart_counter_(0),
      loop_(nullptr),
      pipeline_(nullptr),
      appsrc_(nullptr),
      bus_watch_id_(0) {}

GstVideoServerNodelet::~GstVideoServerNodelet() {
    NODELET_INFO("Terminating gst_video_server...");

    // pipeline bin manage appsrc deletion
    if (pipeline_ != nullptr) {
        gst_app_src_end_of_stream(GST_APP_SRC_CAST(appsrc_));
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(GST_OBJECT(pipeline_));
        pipeline_ = nullptr;
        appsrc_ = nullptr;
    }

    // but if pipeline not created appsrc will be deleted here
    if (appsrc_ != nullptr) {
        gst_object_unref(GST_OBJECT(appsrc_));
        appsrc_ = nullptr;
    }

    if (loop_ != nullptr) {
        g_main_loop_quit(loop_);
        if (loop_thread_.joinable()) loop_thread_.join();
    }
}

void GstVideoServerNodelet::onInit() {
    NODELET_INFO("Starting gst_video_server instance: %s", getName().c_str());

    // init image transport
    ros::NodeHandle &nh = getNodeHandle();
    ros::NodeHandle &priv_nh = getPrivateNodeHandle();
    image_transport_.reset(new image_transport::ImageTransport(nh));

    // load configuration
    if (!priv_nh.getParam("pipeline", gsconfig_)) {
        NODELET_WARN(
            "No pipeline configuration found! Used default testing bin.");
        gsconfig_ = "autovideoconvert ! autovideosink";
    }

    // create and run main event loop
    loop_ = g_main_loop_new(nullptr, FALSE);
    g_assert(loop_);

    loop_thread_ = std::thread([&]() {
        NODELET_DEBUG("GMainLoop started.");
        // blocking
        g_main_loop_run(loop_);
        // terminated!
        g_main_loop_unref(loop_);
        loop_ = nullptr;
        NODELET_INFO("GMainLoop terminated.");
    });
    // NOTE(vooon): may cause a problem on non Linux, but i'm not care.
    pthread_setname_np(loop_thread_.native_handle(), "g_main_loop_run");

    // configure pipeline
    configure_pipeline();

    // finally: subscribe
    image_sub_ = image_transport_->subscribe(
        "image_raw", 10, &GstVideoServerNodelet::image_cb, this);
}

bool GstVideoServerNodelet::configure_pipeline() {
    if (!gst_is_initialized()) {
        NODELET_DEBUG("Initializing gstreamer");
        gst_init(nullptr, nullptr);
    }

    NODELET_INFO("Gstreamer version: %s", gst_version_string());
    NODELET_INFO("Pipeline: %s", gsconfig_.c_str());

    GError *error = nullptr;
    pipeline_ = gst_parse_launch(gsconfig_.c_str(), &error);
    if (pipeline_ == nullptr) {
        NODELET_ERROR("GST: %s", error->message);
        g_error_free(error);
        return false;
    }

    appsrc_ = gst_element_factory_make("appsrc", "source");
    if (appsrc_ == nullptr) {
        NODELET_ERROR("GST: failed to create appsrc!");
        return false;
    }

    gst_app_src_set_stream_type(GST_APP_SRC_CAST(appsrc_),
                                GST_APP_STREAM_TYPE_STREAM);
    gst_app_src_set_latency(GST_APP_SRC_CAST(appsrc_), 0, -1);
    g_object_set(GST_OBJECT(appsrc_), "format", GST_FORMAT_TIME, "is-live",
                 true, "max-bytes", 0, "do-timestamp", true, NULL);

    // gst_parse_launch() may produce not a pipeline
    // thanks to gscam for example
    if (GST_IS_PIPELINE(pipeline_)) {
        // find pipeline sink (where we may link appsrc)
        GstPad *inpad =
            gst_bin_find_unlinked_pad(GST_BIN(pipeline_), GST_PAD_SINK);
        g_assert(inpad);

        GstElement *inelement = gst_pad_get_parent_element(inpad);
        g_assert(inelement);
        gst_object_unref(GST_OBJECT(inpad));
        NODELET_DEBUG("GST: inelement: %s", gst_element_get_name(inelement));

        if (!gst_bin_add(GST_BIN(pipeline_), appsrc_)) {
            NODELET_ERROR("GST: gst_bin_add() failed!");
            gst_object_unref(GST_OBJECT(pipeline_));
            gst_object_unref(GST_OBJECT(inelement));
            return false;
        }

        if (!gst_element_link(appsrc_, inelement)) {
            NODELET_ERROR("GST: cannot link %s -> %s",
                          gst_element_get_name(appsrc_),
                          gst_element_get_name(inelement));
            gst_object_unref(GST_OBJECT(pipeline_));
            gst_object_unref(GST_OBJECT(inelement));
            return false;
        }

        gst_object_unref(GST_OBJECT(inelement));
    } else {
        // we have one sink element, create bin and link it.
        GstElement *launchpipe = pipeline_;
        pipeline_ = gst_pipeline_new(nullptr);
        g_assert(pipeline_);

        gst_object_unparent(GST_OBJECT(launchpipe));

        NODELET_DEBUG("GST: launchpipe: %s", gst_element_get_name(launchpipe));
        gst_bin_add_many(GST_BIN(pipeline_), appsrc_, launchpipe, nullptr);

        if (!gst_element_link(appsrc_, launchpipe)) {
            NODELET_ERROR("GST: cannot link %s -> %s",
                          gst_element_get_name(appsrc_),
                          gst_element_get_name(launchpipe));
            gst_object_unref(GST_OBJECT(pipeline_));
            return false;
        }
    }

    // Register bus watch callback.
    auto bus = gst_element_get_bus(pipeline_);
    bus_watch_id_ = gst_bus_add_watch(
        bus, &GstVideoServerNodelet::bus_message_cb_wrapper, this);
    gst_object_unref(GST_OBJECT(bus));

    // pause pipeline
    gst_element_set_state(pipeline_, GST_STATE_PAUSED);
    if (gst_element_get_state(pipeline_, nullptr, nullptr, 1_sec) ==
        GST_STATE_CHANGE_FAILURE) {
        NODELET_ERROR("GST: state change error. Check your pipeline.");
        return false;
    } else {
        NODELET_INFO("GST: pipeline paused.");
    }

    return true;
}

GstCaps *GstVideoServerNodelet::gst_caps_new_from_image(
    const sensor_msgs::Image::ConstPtr &msg) {
    // http://gstreamer.freedesktop.org/data/doc/gstreamer/head/pwg/html/section-types-definitions.html
    static const ros::M_string known_formats = {{
        {enc::RGB8, "RGB"},
        {enc::RGB16, "RGB16"},
        {enc::RGBA8, "RGBA"},
        {enc::RGBA16, "RGBA16"},
        {enc::BGR8, "BGR"},
        {enc::BGR16, "BGR16"},
        {enc::BGRA8, "BGRA"},
        {enc::BGRA16, "BGRA16"},
        {enc::MONO8, "GRAY8"},
        {enc::MONO16, "GRAY16_LE"},
    }};

    if (msg->is_bigendian) {
        NODELET_ERROR("GST: big endian image format is not supported");
        return nullptr;
    }

    auto format = known_formats.find(msg->encoding);
    if (format == known_formats.end()) {
        NODELET_ERROR("GST: image format '%s' unknown", msg->encoding.c_str());
        return nullptr;
    }

    return gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, format->second.c_str(), "width",
        G_TYPE_INT, msg->width, "height", G_TYPE_INT, msg->height, "framerate",
        GST_TYPE_FRACTION, 0, 1,  // 0/1 = dynamic
        nullptr);
}

#if 0
GstClockTime GstVideoServerNodelet::gst_time_from_stamp(const ros::Time &stamp)
{
#if 1
	// 3. use pipeline time.
	g_assert(pipeline_);
	auto clock = gst_pipeline_get_pipeline_clock(GST_PIPELINE(pipeline_));
	auto ct = gst_clock_get_time(clock);
	gst_object_unref(GST_OBJECT(clock));
	return ct;
#else
	static GstClockTime ct_prev = 0;
	GstClockTime ct = ct_prev;
	ct_prev += gst_util_uint64_scale_int(1, GST_SECOND, 40);
	return ct;
#endif
}
#endif

GstSample *GstVideoServerNodelet::gst_sample_new_from_image(
    const sensor_msgs::Image::ConstPtr &msg) {
    // unfortunately we may not move image data because it is immutable.
    // copying.
    auto buffer = gst_buffer_new_allocate(nullptr, msg->data.size(), nullptr);
    g_assert(buffer);

    /* NOTE(vooon):
     * I found that best is to use `do-timestamp=true` parameter
     * and forgot about stamping stamp problem (PTS).
     */
    // auto ts = gst_time_from_stamp(msg->header.stamp);
    // NODELET_INFO("TS: %lld, %lld", ts, gst_util_uint64_scale_int(1,
    // GST_SECOND, 40));

    gst_buffer_fill(buffer, 0, msg->data.data(), msg->data.size());
    GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_LIVE);
    // GST_BUFFER_PTS(buffer) = ts;

    auto caps = gst_caps_new_from_image(msg);
    if (caps == nullptr) {
        gst_object_unref(GST_OBJECT(buffer));
        return nullptr;
    }

    auto sample = gst_sample_new(buffer, caps, nullptr, nullptr);
    gst_buffer_unref(buffer);
    gst_caps_unref(caps);

    return sample;
}

void GstVideoServerNodelet::image_cb(const sensor_msgs::Image::ConstPtr &msg) {
    NODELET_DEBUG("Image: %d x %d, stamp %f", msg->width, msg->height,
                  msg->header.stamp.toSec());

    GstState state, next_state;

    auto state_change =
        gst_element_get_state(pipeline_, &state, &next_state, 1_ms);
    if (state_change == GST_STATE_CHANGE_ASYNC) {
        NODELET_INFO_THROTTLE(5, "GST: pipeline changing state...");
    } else if (state_change == GST_STATE_CHANGE_FAILURE) {
        if (restart_counter_++ < RESTART_LIM) {
            NODELET_WARN("GST: pipeline state change failure. will retry...");
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            return;
        } else {
            NODELET_ERROR("GST: pipeline restart limit reached. terminating.");
            ros::shutdown();
        }
    }

    // pipeline not yet playing, configure and start
    if (state != GST_STATE_PLAYING && next_state != GST_STATE_PLAYING) {
        auto caps = gst_caps_new_from_image(msg);
        auto capsstr = gst_caps_to_string(caps);

        // really not needed because GstSample may change that anyway
        gst_app_src_set_caps(GST_APP_SRC_CAST(appsrc_), caps);

        NODELET_INFO("GST: appsrc caps: %s", capsstr);
        gst_caps_unref(caps);
        g_free(capsstr);

        gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    }

    auto sample = gst_sample_new_from_image(msg);
    if (sample == nullptr) return;

#if GST_CHECK_VERSION(1, 5, 0)
    auto push_ret = gst_app_src_push_sample(GST_APP_SRC_CAST(appsrc_), sample);
#else
    // NOTE: that function does not increase ref count
    // manual ref prevent problem with bush_buffer
    auto buffer = gst_sample_get_buffer(sample);
    g_assert(buffer);
    gst_buffer_ref(buffer);

    auto caps = gst_sample_get_caps(sample);
    gst_app_src_set_caps(GST_APP_SRC_CAST(appsrc_), caps);

    // that function steal buffer, so ref count should be 2 (sample + buffer)
    auto push_ret = gst_app_src_push_buffer(GST_APP_SRC_CAST(appsrc_), buffer);
#endif

    gst_sample_unref(sample);

    // TODO(vooon): check push_ret value!
    if (push_ret == GST_FLOW_OK && state == GST_STATE_PLAYING) {
        // reset counter
        restart_counter_ = 0;
    }
}

gboolean GstVideoServerNodelet::bus_message_cb_wrapper(GstBus *bus,
                                                       GstMessage *message,
                                                       gpointer data) {
    auto self = static_cast<GstVideoServerNodelet *>(data);
    g_assert(self);

    return self->bus_message_cb(bus, message);
}

gboolean GstVideoServerNodelet::bus_message_cb(__attribute__((unused)) GstBus *bus,
                                               GstMessage *message) {
    gchar *debug = nullptr;
    GError *error = nullptr;

    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            gst_message_parse_error(message, &error, &debug);

            NODELET_ERROR("GST: bus: %s", error->message);
            if (debug != nullptr)
                NODELET_ERROR("GST: debug: %s", debug);  // ->DEBUG

            break;
        }
        case GST_MESSAGE_WARNING: {
            gst_message_parse_warning(message, &error, &debug);

            NODELET_WARN("GST: bus: %s", error->message);
            if (debug != nullptr) NODELET_WARN("GST: debug: %s", debug);

            break;
        }
        case GST_MESSAGE_INFO: {
            gst_message_parse_info(message, &error, &debug);

            NODELET_INFO("GST: bus: %s", error->message);
            if (debug != nullptr) NODELET_INFO("GST: debug: %s", debug);

            break;
        }
        case GST_MESSAGE_EOS:
            NODELET_ERROR("GST: bus EOS");
            // TODO(vooon): self terminate. Error not recoverable.
            ros::shutdown();
            break;

        default:
            break;
    }

    if (error != nullptr) g_error_free(error);
    g_free(debug);

    return TRUE;
}

};  // namespace gst_video_server

// Register nodelet
#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(gst_video_server::GstVideoServerNodelet,
                       nodelet::Nodelet);
