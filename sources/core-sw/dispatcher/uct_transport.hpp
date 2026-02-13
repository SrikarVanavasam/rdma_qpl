#ifndef QPL_UCT_TRANSPORT_HPP
#define QPL_UCT_TRANSPORT_HPP

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <stdexcept>
#include <cstdint>

#include <uct/api/uct.h>

namespace qpl::ml::dispatcher {

// Forward declarations
class UctEndpoint;

/**
 * @brief UCT Context manager. Handles Async Context, Worker, Memory Domain.
 */
class UctContext {
public:
    UctContext();
    ~UctContext();

    // Initialize UCT resources
    // dev_name: "mlx5_0", "mlx5_2", etc.
    bool init(const std::string& dev_name = "mlx5_0");

    // Progress the worker (must be called for completions/progress)
    void progress();

    // Register memory
    // returns uct_mem_h on success, nullptr on failure
    uct_mem_h register_memory(void* addr, size_t length);
    void deregister_memory(uct_mem_h memh);

    // Getters
    uct_worker_h get_worker() const { return worker_; }
    uct_md_h get_md() const { return md_; }
    uct_component_h get_component() const { return component_; }
    
    // Check XGVMI support (Exported MKey)
    bool supports_xgvmi() const;

private:
    ucs_async_context_t* async_ = nullptr;
    uct_worker_h worker_ = nullptr;
    uct_component_h component_ = nullptr; // IB component
    uct_md_h md_ = nullptr;
    uct_md_config_t* md_config_ = nullptr;
};

/**
 * @brief Represents a connection to a remote peer via RC transport.
 */
class UctEndpoint {
public:
    UctEndpoint(UctContext& ctx);
    ~UctEndpoint();

    // Open the local RC interface
    // dev_name: "mlx5_0:1", etc.
    bool open_interface(const std::string& dev_name = "mlx5_0:1");

    // Get local address info to send to peer
    struct AddressInfo {
        std::vector<uint8_t> dev_addr;
        std::vector<uint8_t> iface_addr;
        std::vector<uint8_t> ep_addr;
    };
    AddressInfo get_local_address() const;

    // Connect to remote peer using their address info
    bool connect(const AddressInfo& remote_addr);

    // Send a short message (e.g. Descriptor)
    // addr: remote virtual address
    // rkey: remote key
    ucs_status_t put_short(const void* buffer, size_t length, uint64_t remote_addr, uct_rkey_t rkey);

    // RDMA Write (Zero Copy)
    ucs_status_t put_zcopy(const void* buffer, size_t length, uct_mem_h memh, uint64_t remote_addr, uct_rkey_t rkey);

    // RDMA Read (Zero Copy)
    ucs_status_t get_zcopy(void* buffer, size_t length, uct_mem_h memh, uint64_t remote_addr, uct_rkey_t rkey, uct_completion_t* comp);

    // RDMA Read (Buffered Copy)
    // cb: Callback when unpack is done
    using UnpackCallback = void (*)(void *arg, const void *data, size_t length);
    ucs_status_t get_bcopy(UnpackCallback cb, void* cb_arg, size_t length, uint64_t remote_addr, uct_rkey_t rkey, uct_completion_t* comp = nullptr);

    // Unpack an RKey buffer received from peer
    // returns packed rkey specific to this component
    ucs_status_t unpack_rkey(const void* rkey_buffer, uct_rkey_bundle_t* bundle);

    // Flush pending operations
    void flush();

    uct_ep_h get_ep() const { return ep_; }

private:
    UctContext& ctx_;
    uct_iface_h iface_ = nullptr;
    uct_ep_h ep_ = nullptr;
    uct_iface_config_t* iface_config_ = nullptr;
};

/**
 * @brief Helper for TCP side-band connection to exchange UCT addresses.
 */
class TcpConnection {
public:
    TcpConnection() : fd_(-1) {}
    ~TcpConnection();

    bool listen(int port);
    bool connect(const std::string& ip, int port);
    std::shared_ptr<TcpConnection> accept();

    bool send(const void* data, size_t size);
    bool recv(void* data, size_t size);

    // Helpers to send/recv vectors
    bool send_vec(const std::vector<uint8_t>& vec);
    bool recv_vec(std::vector<uint8_t>& vec);
    
    // Helpers to send/recv primitive types
    template<typename T>
    bool send_val(const T& val) {
        return send(&val, sizeof(T));
    }
    
    template<typename T>
    bool recv_val(T& val) {
        return recv(&val, sizeof(T));
    }

    bool is_active();

private:
    int fd_;
};

} // namespace qpl::ml::dispatcher

#endif // QPL_UCT_TRANSPORT_HPP
