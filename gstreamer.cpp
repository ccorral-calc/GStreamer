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

class VideoRecorder
{
public:
    VideoRecorder(int port, const std::string &filename)
        : port_(port), filename_(filename), keep_running_(true), data_flowing_(false)
    {

        // Define unique pipeline per instance
        std::string pipeline_desc =
            "input-selector name=sel sync-streams=false ! mpegtsmux alignment=1 ! filesink location=" + filename_ + " "
                                                                                                                    "appsrc name=mysrc format=time is-live=true do-timestamp=true ! tsdemux ! h264parse ! sel.sink_0 "
                                                                                                                    "videotestsrc pattern=snow is-live=true ! video/x-raw,width=720,height=480,framerate=30/1 ! x264enc tune=zerolatency ! h264parse ! sel.sink_1";

        pipeline_ = gst_parse_launch(pipeline_desc.c_str(), NULL);
        selector_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sel");
        app_src_ = gst_bin_get_by_name(GST_BIN(pipeline_), "mysrc");

        primary_pad_ = gst_element_get_static_pad(selector_, "sink_0");
        fallback_pad_ = gst_element_get_static_pad(selector_, "sink_1");

        // Start on fallback
        g_object_set(selector_, "active-pad", fallback_pad_, NULL);
        gst_element_set_state(pipeline_, GST_STATE_PLAYING);

        worker_thread_ = std::thread(&VideoRecorder::socket_worker, this);
    }

    ~VideoRecorder()
    {
        stop();
    }

    void stop()
    {
        if (keep_running_)
        {
            keep_running_ = false;
            if (worker_thread_.joinable())
                worker_thread_.join();
            gst_element_send_event(pipeline_, gst_event_new_eos());
            // Wait a moment for EOS to propagate before NULL state
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_object_unref(pipeline_);
        }
    }

private:
    void socket_worker()
    {
        int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);
        bind(sockfd, (const struct sockaddr *)&addr, sizeof(addr));

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
                    g_object_set(selector_, "active-pad", primary_pad_, NULL);
                }
                GstBuffer *gst_buf = gst_buffer_new_allocate(NULL, n, NULL);
                gst_buffer_fill(gst_buf, 0, buffer, n);
                GstFlowReturn ret;
                g_signal_emit_by_name(app_src_, "push-buffer", gst_buf, &ret);
                gst_buffer_unref(gst_buf);
            }
            else
            {
                if (data_flowing_.exchange(false))
                {
                    g_object_set(selector_, "active-pad", fallback_pad_, NULL);
                }
            }
        }
        close(sockfd);
    }

    int port_;
    std::string filename_;
    GstElement *pipeline_, *selector_, *app_src_;
    GstPad *primary_pad_, *fallback_pad_;
    std::thread worker_thread_;
    std::atomic<bool> keep_running_, data_flowing_;
};

// Global for signal handler
std::vector<VideoRecorder *> recorders;

void handle_sigint(int)
{
    std::cout << "\n[SYSTEM] Shutting down all streams...\n";
    for (auto r : recorders)
        r->stop();
    exit(0);
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    signal(SIGINT, handle_sigint);

    // Initialize 9 recorders on ports 5001-5009
    for (int i = 1; i <= 9; ++i)
    {
        int port = 5000 + i;
        std::string file = "stream_" + std::to_string(port) + ".ts";
        recorders.push_back(new VideoRecorder(port, file));
        std::cout << "[INFO] Started recorder on port " << port << " -> " << file << "\n";
    }

    // Main thread stays alive
    while (true)
        std::this_thread::sleep_for(std::chrono::seconds(1));

    return 0;
}
