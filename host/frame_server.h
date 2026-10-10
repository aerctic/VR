// Compositor end of the capture protocol: accepts connections from games
// (and from vrlayer-ctl), imports their shared frame slots into our Vulkan
// device and hands out the newest frame.
#pragma once

#include <sys/types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gpu.h"
#include "protocol.h"

namespace vrl {

// A game frame ready to be read by shaders (layout GENERAL).
struct FrameView {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkBuffer depth = VK_NULL_HANDLE;
    VkDeviceSize depth_size = 0;
    DepthFormat depth_format = DepthFormat::None;
    uint32_t depth_width = 0, depth_height = 0;
    bool has_depth = false;
    uint64_t frame_index = 0;
    uint64_t timestamp_ns = 0;
    // Game camera (before injection) and the head pose injected into this frame.
    bool camera_valid = false;
    float cam_rot[9] = {}, cam_pos[3] = {};
    uint32_t pose_seq = 0;
    uint32_t eye = 0;  // 0 both, 1 left, 2 right
};

struct ClientInfo {
    int id = 0;
    pid_t pid = 0;
    uint32_t appid = 0;
    std::string exe;
    Transport transport = Transport::Gpu;
    uint32_t width = 0, height = 0;
    bool depth = false;
    uint64_t frames = 0;
    bool has_analysis = false;
    AnalysisMsg analysis{};
};

class FrameServer {
public:
    explicit FrameServer(Gpu* gpu);
    ~FrameServer();
    FrameServer(const FrameServer&) = delete;
    FrameServer& operator=(const FrameServer&) = delete;

    bool listen(const std::string& name);

    // Handle pending socket traffic without blocking (timeout_ms > 0 waits
    // for activity first).
    void poll(int timeout_ms = 0);

    // True if the active client delivered a frame not yet consumed.
    bool has_new_frame() const;

    // Records into `cmd` whatever makes the newest frame readable (queue
    // family acquire for the gpu transport, an upload for cpu). The caller
    // must submit `cmd` and call gpu_work_done() once it has completed, and
    // must not call begin_frame() again before that.
    bool begin_frame(VkCommandBuffer cmd, FrameView* out);
    void gpu_work_done();

    const ClientInfo* active() const;
    // Camera control for the active game.
    bool send_camera(const CameraMsg& m);
    void restart_analysis();
    std::vector<ClientInfo> clients() const;
    void cycle_active();

    // Text command from vrlayer-ctl -> reply.
    std::function<std::string(const std::string&)> control_handler;
    // Active game changed (connected, switched or disconnected).
    std::function<void(const ClientInfo*)> on_active_changed;

private:
    struct Client;
    struct InFlight {
        int client_id;
        uint32_t generation;
        uint32_t slot;
    };

    void accept_clients();
    bool handle_message(Client& c, const char* buf, size_t n, std::vector<int>& fds);
    bool import_slots(Client& c, const SlotsMsg& m, std::vector<int>& fds);
    void free_imports(Client& c);
    void drop_client(size_t index);
    void release(Client& c, uint32_t generation, uint32_t slot);
    void update_active();
    Client* find(int id);

    Gpu* gpu_;
    int listen_fd_ = -1;
    int next_id_ = 1;
    int active_id_ = 0;
    std::vector<std::unique_ptr<Client>> clients_;
    std::vector<InFlight> in_flight_;
};

}  // namespace vrl
