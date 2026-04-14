#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <csignal>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

// Minimal H.264 IDR Black Frame (SPS+PPS+IDR)
const std::vector<uint8_t> BLACK_H264_FRAME = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x1e, 0xda, 0x02, 0x80, 0xf6,
    0x80, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80, 0x00, 0x00, 0x00,
    0x01, 0x65, 0xb8, 0x00, 0x04, 0x00, 0x00, 0x13, 0x80, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x01};

class RawVideoRecorder
{
public:
    int port_;
    RawVideoRecorder(int port, std::string filename)
        : port_(port), filename_(filename), running_(true), continuity_counter_(0)
    {
        worker_ = std::thread(&RawVideoRecorder::run, this);
    }

    ~RawVideoRecorder()
    {
        running_ = false;
        if (worker_.joinable())
            worker_.join();
    }

private:
    std::string filename_;
    std::atomic<bool> running_;
    std::thread worker_;
    uint8_t continuity_counter_; // Must persist across fallback packets

    void run()
    {
        int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in addr = {AF_INET, htons(port_), {INADDR_ANY}};
        bind(sockfd, (const struct sockaddr *)&addr, sizeof(addr));

        struct timeval tv = {0, 200000}; // 200ms timeout
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        AVFormatContext *out_ctx = nullptr;
        avformat_alloc_output_context2(&out_ctx, nullptr, "mpegts", filename_.c_str());

        AVIOContext *pb = nullptr;
        if (avio_open(&pb, filename_.c_str(), AVIO_FLAG_WRITE) < 0)
            return;
        out_ctx->pb = pb;

        avformat_new_stream(out_ctx, nullptr);
        int ret = avformat_write_header(out_ctx, nullptr);
        printf("%d\r\n", ret);

        uint8_t packet_buffer[2048];
        bool data_flowing = false;

        while (running_)
        {
            ssize_t n = recvfrom(sockfd, packet_buffer, sizeof(packet_buffer), 0, nullptr, nullptr);

            if (n > 0)
            {
                avio_write(out_ctx->pb, packet_buffer, n);
                avio_flush(out_ctx->pb);
                data_flowing = true;
            }
            else
            {
                if (data_flowing)
                {
                    std::cout << "[PORT " << port_ << "] Timeout: Switching to fallback." << std::endl;
                    data_flowing = false;
                }
                write_black_frame_packet(out_ctx);
                // Throttle fallback to ~30fps
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
            }
        }

        av_write_trailer(out_ctx);
        avio_closep(&out_ctx->pb);
        avformat_free_context(out_ctx);
        close(sockfd);
    }

    void write_black_frame_packet(AVFormatContext *ctx)
    {
        if (!ctx || !ctx->pb)
            return;

        // TS Packet is strictly 188 bytes
        uint8_t ts_packet[188];
        memset(ts_packet, 0xFF, 188); // Fill with padding initially

        // 1. Header (4 bytes)
        ts_packet[0] = 0x47; // Sync byte
        ts_packet[1] = 0x41; // Payload start + PID high (0x01)
        ts_packet[2] = 0x00; // PID low (Total PID 0x100 / 256)

        // 2. Continuity Counter (lower 4 bits of byte 3)
        // Header: 0x10 (No adaptation field, payload only) | counter
        ts_packet[3] = 0x10 | (continuity_counter_ & 0x0F);
        continuity_counter_++;

        // 3. Insert H.264 Data (Limit to fit in one TS packet for simplicity)
        size_t payload_size = std::min((size_t)184, BLACK_H264_FRAME.size());
        memcpy(&ts_packet[4], BLACK_H264_FRAME.data(), payload_size);

        // 4. Write to FFmpeg IO context
        avio_write(ctx->pb, ts_packet, 188);
        avio_flush(ctx->pb);
    }
};

// Global for signal handler
std::vector<RawVideoRecorder *> recorders;
std::atomic<bool> _running = true;
void handle_sigint(int)
{
    _running = false;
}

int main(int argc, char *argv[])
{
    signal(SIGINT, handle_sigint);

    // Initialize 9 recorders on ports 5001-5009
    for (int i = 1; i <= 9; ++i)
    {
        int port = 5000 + i;
        std::string file = "stream_" + std::to_string(port) + ".ts";
        recorders.push_back(new RawVideoRecorder(port, file));
        std::cout << "[INFO] Started recorder on port " << port << " -> " << file << "\n";
    }

    // Main thread stays alive
    while (_running)
        std::this_thread::sleep_for(std::chrono::seconds(1));

    // Cleanup
    std::cout << "\n[SYSTEM] Shutting down all streams...\n";
    for (auto &r : recorders)
    {
        std::cout << "[INFO] Stopped recorder on port " << r->port_ << "\n";
        delete r;
    }

    return 0;
}
