#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <csignal>
#include <string>
#include <cstring> // For strerror

class VideoRecorder
{
public:
    int port_;
    VideoRecorder(int port, const std::string &filename)
        : port_(port), filename_(filename), keep_running_(true), data_flowing_(false)
    {

        // List all critical elements your pipeline_desc uses
        std::vector<std::string> required_elements = {
            "mpegtsmux", "input-selector", "tsdemux", "h264parse", "x264enc"};

        for (const auto &el : required_elements)
        {
            if (!check_plugin(el))
            {
                // Early exit or throw exception
                keep_running_ = false;
                return;
            }
        }

        // Define elements
        std::string input_selector = "input-selector name=sel sync-streams=true sync-mode=1 ! h264parse config-interval=1 ! mpegtsmux ! filesink location=" + filename + " ";
        std::string branch_live = "appsrc name=mysrc format=time is-live=true ! tsdemux ! h264parse ! queue leaky=downstream ! sel.sink_0 ";
        std::string branch_fallback_video = "multifilesrc location=black_720.ts loop=true ! tsdemux ! h264parse !  identity sync=true ! queue flush-on-eos=true leaky=downstream ! sel.sink_1";

        std::string pipeline_desc = input_selector + branch_live + branch_fallback_video;
        GError *error = NULL;
        pipeline_ = gst_parse_launch(pipeline_desc.c_str(), &error);
        if (error)
        {
            printf("[PORT %d] GStreamer Parse Error: %s\n", port, error->message);
            g_error_free(error);
            return;
        }
        selector_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sel");
        app_src_ = gst_bin_get_by_name(GST_BIN(pipeline_), "mysrc");
        if (!selector_ || !app_src_)
        {
            fprintf(stderr, "[PORT %d] Failed to find internal elements\n", port);
            return;
        }

        primary_pad_ = gst_element_get_static_pad(selector_, "sink_0");
        fallback_pad_ = gst_element_get_static_pad(selector_, "sink_1");

        // Start on fallback
        g_object_set(selector_, "active-pad", fallback_pad_, NULL);

        GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE)
        {
            fprintf(stderr, "[PORT %d] Failed to start pipeline. Check if plugins are missing!\n", port);
            return;
        }

        worker_thread_ = std::thread(&VideoRecorder::socket_worker, this);
    }

    ~VideoRecorder() = default;

    void stop()
    {
        //  Atomically ensure we only stop once
        if (!keep_running_.exchange(false))
            return;

        // 3. Signal GStreamer to finish the file
        if (app_src_)
        {
            printf("Stopping the app_src\r\n");
            gst_app_src_end_of_stream(GST_APP_SRC(app_src_));
        }

        // Change state to NULL immediately.
        // Do NOT wait for EOS on the bus; it won't come due to 'loop=true' fallback.
        gst_element_set_state(pipeline_, GST_STATE_NULL);

        // Stop the thread
        if (worker_thread_.joinable())
        {
            printf("Stopping the Thread\r\n");
            worker_thread_.join();
        }

        // 6. Cleanup refs (Null checks are important)
        if (selector_)
            gst_object_unref(selector_);
        if (app_src_)
            gst_object_unref(app_src_);
        if (primary_pad_)
            gst_object_unref(primary_pad_);
        if (fallback_pad_)
            gst_object_unref(fallback_pad_);
        gst_object_unref(pipeline_);

        pipeline_ = nullptr; // Prevent double cleanup
    }

    bool check_plugin(const std::string &name)
    {
        GstElementFactory *factory = gst_element_factory_find(name.c_str());
        if (!factory)
        {
            std::cerr << "[CRITICAL] Missing GStreamer element: " << name
                      << ". Please install the necessary plugin package.\n";
            return false;
        }
        gst_object_unref(factory);
        return true;
    }

private:
    void
    socket_worker()
    {
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0)
        {
            printf("[PORT %d] Socket Creation Error: %s\n", port_, strerror(errno));
            return;
        }

        // Reuse Addr
        int opt = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(sockfd, (const struct sockaddr *)&addr, sizeof(addr)) < 0)
        {
            printf("[PORT %d] Bind Error: %s\n", port_, strerror(errno));
            close(sockfd);
            return;
        }

        struct timeval tv = {0, 200000}; // 200ms timeout
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        uint8_t buffer[2048];
        while (keep_running_)
        {

            ssize_t n = recvfrom(sockfd, buffer, sizeof(buffer), 0, NULL, NULL);
            if (n > 0)
            {
                if (!data_flowing_.exchange(true))
                {
                    // gst_pad_push_event(primary_pad_, gst_event_new_flush_stop(TRUE));
                    printf("[PORT %d] Incoming Data Detected -> Switching to Primary\n", port_);
                    g_object_set(selector_, "active-pad", primary_pad_, NULL);
                }
                GstBuffer *gst_buf = gst_buffer_new_allocate(NULL, n, NULL);
                gst_buffer_fill(gst_buf, 0, buffer, n);
                GstFlowReturn ret;
                g_signal_emit_by_name(app_src_, "push-buffer", gst_buf, &ret);
                gst_buffer_unref(gst_buf);
                if (ret != GST_FLOW_OK)
                {
                    printf("[PORT %d] appsrc push failed: %d\n", port_, ret);
                }
            }
            else
            {
                if (data_flowing_.exchange(false))
                {
                    // gst_pad_push_event(primary_pad_, gst_event_new_flush_start());
                    printf("[PORT %d] Signal Lost -> Switching to Fallback\n", port_);
                    g_object_set(selector_, "active-pad", fallback_pad_, NULL);
                }
            }
        }
        keep_running_.store(false);
        close(sockfd);
        sockfd = -1;
    }

    int sockfd;
    std::string filename_;
    GstElement *pipeline_ = nullptr, *selector_ = nullptr, *app_src_ = nullptr;
    GstPad *primary_pad_ = nullptr, *fallback_pad_ = nullptr;
    std::thread worker_thread_;
    std::atomic<bool> keep_running_, data_flowing_;
};

// Global for signal handler
std::vector<VideoRecorder *> recorders;
std::atomic<bool> _running = true;
void handle_sigint(int)
{
    _running.store(false);
}

typedef struct Config
{
    int base_port;
    int num_recorders;
} Config;

Config parse_args(int argc, char *argv[])
{
    Config cfg = {5000, 1}; // Default
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "-n" && i + 1 < argc)
            cfg.num_recorders = std::stoi(argv[++i]);
        else if (arg == "-p" && i + 1 < argc)
            cfg.base_port = std::stoi(argv[++i]);
    }
    return cfg;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv); // Pass args to GStreamer
    signal(SIGINT, handle_sigint);

    Config cfg = parse_args(argc, argv);

    printf("Initializing %d recorders\n", cfg.num_recorders);

    // Initialize recorders
    for (int i = 0; i < cfg.num_recorders; ++i)
    {
        int port = cfg.base_port + i;
        std::string file = "stream_" + std::to_string(port) + ".ts";
        recorders.push_back(new VideoRecorder(port, file));
        std::cout << "[INFO] Started recorder on port " << port << " -> " << file << "\n";
    }

    // Keep main thread alive
    while (_running.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Cleanup
    std::cout << "\n[SYSTEM] Shutting down all streams...\n";
    for (auto r : recorders)
    {
        r->stop();
        std::cout << "[INFO] Deleted recorder on port " << r->port_ << "\n";
        delete r;
    }

    return 0;
}
