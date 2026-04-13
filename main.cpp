#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

// Global handles
GMainLoop *main_loop = nullptr;
GstElement *pipeline, *selector, *app_src;
GstPad *primary_pad, *fallback_pad;
std::atomic<bool> keep_running(true);
std::atomic<bool> data_flowing(false);

// Signal Handler
void handle_interrupt(int sig)
{
    printf("\n[SYSTEM] Interrupt received. Finalizing recording...\n");

    if (pipeline)
    {
        // 1. Send EOS to the pipeline to finalize the file
        gst_element_send_event(pipeline, gst_event_new_eos());
    }

    // 2. Stop the network thread loop
    keep_running = false;
}

// Bus Watch: Listens for the EOS we sent or for Errors
static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data)
{
    GMainLoop *loop = (GMainLoop *)data;
    switch (GST_MESSAGE_TYPE(msg))
    {
    case GST_MESSAGE_EOS:
        printf("[SYSTEM] EOS reached. Quitting loop.\n");
        g_main_loop_quit(loop);
        break;
    case GST_MESSAGE_ERROR:
    {
        gchar *debug;
        GError *error;
        gst_message_parse_error(msg, &error, &debug);
        g_printerr("[ERROR] %s\n", error->message);
        g_error_free(error);
        g_free(debug);
        g_main_loop_quit(loop);
        break;
    }
    default:
        break;
    }
    return TRUE;
}

// Socket thread: Receives UDP and feeds AppSrc
void socket_worker()
{
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(5001);

    bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr));

    // Set a 200ms socket timeout for the "Connection Lost" detection
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buffer[2048]; // MPEG-TS packets are 188 bytes; 2048 handles MTU

    while (keep_running)
    {
        ssize_t n = recvfrom(sockfd, buffer, sizeof(buffer), 0, NULL, NULL);

        if (n > 0)
        {
            // Data received: Switch to primary
            if (!data_flowing.exchange(true))
            {
                printf("[NETWORK] Data detected. Switching to Primary.\n");
                g_object_set(selector, "active-pad", primary_pad, NULL);
            }

            // Wrap in GstBuffer and push to pipeline
            GstBuffer *gst_buf = gst_buffer_new_allocate(NULL, n, NULL);
            gst_buffer_fill(gst_buf, 0, buffer, n);

            // Push buffer. 'appsrc' handles the internal queueing.
            GstFlowReturn ret;
            g_signal_emit_by_name(app_src, "push-buffer", gst_buf, &ret);
            gst_buffer_unref(gst_buf);
        }
        else
        {
            // Timeout reached: Switch to fallback
            if (data_flowing.exchange(false))
            {
                printf("[NETWORK] Timeout! Switching to Black Frames.\n");
                g_object_set(selector, "active-pad", fallback_pad, NULL);
            }
        }
    }
    close(sockfd);
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    // Setup Signal Handling
    signal(SIGINT, handle_interrupt);
    signal(SIGTERM, handle_interrupt);

    // Pipeline: appsrc replaces udpsrc
    const char *desc =
        "input-selector name=sel sync-streams=false ! mpegtsmux alignment=1 ! filesink location=output.ts "
        "appsrc name=mysrc format=time is-live=true do-timestamp=true ! tsdemux ! h264parse ! sel.sink_0 "
        "videotestsrc pattern=black is-live=true ! video/x-raw,width=720,height=480,framerate=30/1 ! x264enc tune=zerolatency ! h264parse ! sel.sink_1";

    pipeline = gst_parse_launch(desc, NULL);
    main_loop = g_main_loop_new(NULL, FALSE);
    // Add bus watch to handle the EOS event properly
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_watch(bus, bus_call, main_loop);
    gst_object_unref(bus);

    selector = gst_bin_get_by_name(GST_BIN(pipeline), "sel");
    app_src = gst_bin_get_by_name(GST_BIN(pipeline), "mysrc");

    primary_pad = gst_element_get_static_pad(selector, "sink_0");
    fallback_pad = gst_element_get_static_pad(selector, "sink_1");

    // Start with fallback
    g_object_set(selector, "active-pad", fallback_pad, NULL);

    // Launch the socket thread
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    std::thread net_thread(socket_worker);

    printf("Recording... Press Ctrl+C to stop safely.\n");
    g_main_loop_run(main_loop);

    // Graceful Cleanup after loop exits (triggered by EOS)
    net_thread.join();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_main_loop_unref(main_loop);

    printf("[SYSTEM] Shutdown complete.\n");
    return 0;
}
