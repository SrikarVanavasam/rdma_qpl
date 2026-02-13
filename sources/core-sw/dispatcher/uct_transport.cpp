#include "uct_transport.hpp"

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

namespace qpl::ml::dispatcher {

// --- TcpConnection Implementation ---

TcpConnection::~TcpConnection() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

bool TcpConnection::listen(int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return false;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server_fd);
        return false;
    }

    if (::listen(server_fd, 1) < 0) {
        close(server_fd);
        return false;
    }

    // Accept one connection
    std::cout << "[Tcp] Listening on port " << port << "..." << std::endl;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    fd_ = ::accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
    
    close(server_fd); // Close listening socket
    return fd_ >= 0;
}

bool TcpConnection::connect(const std::string& ip, int port) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (::connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

std::shared_ptr<TcpConnection> TcpConnection::accept() {
    // Already implemented 'accept' inside listen for simplicity in this V1
    // but typically listen should just listen and accept returns a new obj.
    // For now, TcpConnection manages the ACTIVE socket.
    return nullptr; 
}

bool TcpConnection::send(const void* data, size_t size) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    size_t remaining = size;
    while (remaining > 0) {
        ssize_t sent = ::write(fd_, ptr, remaining);
        if (sent <= 0) return false;
        ptr += sent;
        remaining -= sent;
    }
    return true;
}

bool TcpConnection::recv(void* data, size_t size) {
    uint8_t* ptr = static_cast<uint8_t*>(data);
    size_t remaining = size;
    while (remaining > 0) {
        ssize_t received = ::read(fd_, ptr, remaining);
        if (received <= 0) return false;
        ptr += received;
        remaining -= received;
    }
    return true;
}

bool TcpConnection::is_active() {
    if (fd_ < 0) return false;
    char buf;
    ssize_t ret = ::recv(fd_, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
    if (ret == 0) return false; // EOF (Closed)
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true; // Active but no data
        return false; // Error
    }
    return true; // Data available (Active)
}

bool TcpConnection::send_vec(const std::vector<uint8_t>& vec) {
    size_t size = vec.size();
    if (!send_val(size)) return false;
    return send(vec.data(), size);
}

bool TcpConnection::recv_vec(std::vector<uint8_t>& vec) {
    size_t size;
    if (!recv_val(size)) return false;
    vec.resize(size);
    return recv(vec.data(), size);
}


// --- UctContext Implementation ---

UctContext::UctContext() {}

UctContext::~UctContext() {
    if (worker_) uct_worker_destroy(worker_);
    if (md_) uct_md_close(md_);
    if (component_) {
        // Warning: uct_release_component_list expects a list, 
        // but we only kept one handle. In full impl we should keep the list pointer.
        // For now, assume it's fine or we might leak the component list structure.
    }
    if (async_) ucs_async_context_destroy(async_);
}

bool UctContext::init(const std::string& dev_name) {
    // 1. Async Context
    ucs_status_t status = ucs_async_context_create(UCS_ASYNC_MODE_THREAD_SPINLOCK, &async_);
    if (status != UCS_OK) {
        std::cerr << "Failed to create async context" << std::endl;
        return false;
    }

    // 2. Worker
    status = uct_worker_create(async_, UCS_THREAD_MODE_SINGLE, &worker_);
    if (status != UCS_OK) {
        std::cerr << "Failed to create worker" << std::endl;
        return false;
    }

    // 3. Component (IB)
    uct_component_h *components;
    unsigned num_components;
    status = uct_query_components(&components, &num_components);
    if (status != UCS_OK) return false;

    for (unsigned i = 0; i < num_components; i++) {
        uct_component_attr_t attr = {};
        attr.field_mask = UCT_COMPONENT_ATTR_FIELD_NAME;
        uct_component_query(components[i], &attr);
        if (strcmp(attr.name, "ib") == 0) {
            component_ = components[i];
            break;
        }
    }
    uct_release_component_list(components);

    if (!component_) {
        std::cerr << "IB component not found" << std::endl;
        return false;
    }

    // 4. Memory Domain (MD)
    status = uct_md_config_read(component_, nullptr, nullptr, &md_config_);
    if (status != UCS_OK) return false;

    status = uct_md_open(component_, dev_name.c_str(), md_config_, &md_);
    uct_config_release(md_config_);
    
    if (status != UCS_OK) {
        std::cerr << "Failed to open MD for device " << dev_name << ": " << ucs_status_string(status) << std::endl;
        return false;
    }

    return true;
}

void UctContext::progress() {
    uct_worker_progress(worker_);
}

uct_mem_h UctContext::register_memory(void* addr, size_t length) {
    uct_mem_h memh;
    // Basic registration for RMA access
    ucs_status_t status = uct_md_mem_reg(md_, addr, length, 
                                       UCT_MD_MEM_ACCESS_RMA | UCT_MD_MEM_ACCESS_REMOTE_ATOMIC | UCT_MD_MEM_FLAG_LOCK, 
                                       &memh);
    if (status != UCS_OK) {
        std::cerr << "Failed to register memory: " << ucs_status_string(status) << std::endl;
        return nullptr;
    }
    return memh;
}

void UctContext::deregister_memory(uct_mem_h memh) {
    uct_md_mem_dereg(md_, memh);
}

bool UctContext::supports_xgvmi() const {
    if (!md_) return false;
    uct_md_attr_t md_attr;
    uct_md_query(md_, &md_attr);
    return (md_attr.cap.flags & UCT_MD_FLAG_EXPORTED_MKEY);
}


// --- UctEndpoint Implementation ---

UctEndpoint::UctEndpoint(UctContext& ctx) : ctx_(ctx) {}

UctEndpoint::~UctEndpoint() {
    if (ep_) uct_ep_destroy(ep_);
    if (iface_) uct_iface_close(iface_);
}

bool UctEndpoint::open_interface(const std::string& dev_name) {
    uct_md_iface_config_read(ctx_.get_md(), "rc_mlx5", nullptr, nullptr, &iface_config_);
    
    uct_iface_params_t params = {};
    params.field_mask = UCT_IFACE_PARAM_FIELD_OPEN_MODE | UCT_IFACE_PARAM_FIELD_DEVICE;
    params.open_mode = UCT_IFACE_OPEN_MODE_DEVICE;
    params.mode.device.tl_name = "rc_mlx5";
    params.mode.device.dev_name = dev_name.c_str();

    ucs_status_t status = uct_iface_open(ctx_.get_md(), ctx_.get_worker(), &params, iface_config_, &iface_);
    uct_config_release(iface_config_);

    if (status != UCS_OK) {
        std::cerr << "Failed to open Interface " << dev_name << ": " << ucs_status_string(status) << std::endl;
        return false;
    }

    uct_iface_progress_enable(iface_, UCT_PROGRESS_SEND | UCT_PROGRESS_RECV);

    // Create Endpoint immediately so we can get its address
    uct_ep_params_t ep_params = {};
    ep_params.field_mask = UCT_EP_PARAM_FIELD_IFACE;
    ep_params.iface = iface_;
    status = uct_ep_create(&ep_params, &ep_);
    if (status != UCS_OK) {
         std::cerr << "Failed to create EP: " << ucs_status_string(status) << std::endl;
         return false;
    }

    return true;
}

UctEndpoint::AddressInfo UctEndpoint::get_local_address() const {
    AddressInfo info;
    uct_iface_attr_t attr;
    uct_iface_query(iface_, &attr);

    info.dev_addr.resize(attr.device_addr_len);
    info.iface_addr.resize(attr.iface_addr_len);
    info.ep_addr.resize(attr.ep_addr_len);

    uct_iface_get_device_address(iface_, (uct_device_addr_t*)info.dev_addr.data());
    uct_iface_get_address(iface_, (uct_iface_addr_t*)info.iface_addr.data());
    
    if (ep_) {
        uct_ep_get_address(ep_, (uct_ep_addr_t*)info.ep_addr.data());
    }
    
    return info; 
}

bool UctEndpoint::connect(const AddressInfo& remote_addr) {
    if (!ep_) return false;

    // For RC, we connect to remote EP info
    ucs_status_t status = uct_ep_connect_to_ep(ep_, 
        (const uct_device_addr_t*)remote_addr.dev_addr.data(), 
        (const uct_ep_addr_t*)remote_addr.ep_addr.data());
        
    return status == UCS_OK;
}

ucs_status_t UctEndpoint::put_short(const void* buffer, size_t length, uint64_t remote_addr, uct_rkey_t rkey) {
    return uct_ep_put_short(ep_, buffer, length, remote_addr, rkey);
}

ucs_status_t UctEndpoint::put_zcopy(const void* buffer, size_t length, uct_mem_h memh, uint64_t remote_addr, uct_rkey_t rkey) {
    uct_iov_t iov;
    iov.buffer = const_cast<void*>(buffer);
    iov.length = length;
    iov.memh = memh;
    iov.stride = 0;
    iov.count = 1;

    return uct_ep_put_zcopy(ep_, &iov, 1, remote_addr, rkey, nullptr);
}

ucs_status_t UctEndpoint::get_zcopy(void* buffer, size_t length, uct_mem_h memh, uint64_t remote_addr, uct_rkey_t rkey, uct_completion_t* comp) {
    uct_iov_t iov;
    iov.buffer = buffer;
    iov.length = length;
    iov.memh   = memh;
    iov.stride = 0;
    iov.count  = 1;

    return uct_ep_get_zcopy(ep_, &iov, 1, remote_addr, rkey, comp);
}

ucs_status_t UctEndpoint::get_bcopy(UnpackCallback cb, void* cb_arg, size_t length, uint64_t remote_addr, uct_rkey_t rkey, uct_completion_t* comp) {
    return uct_ep_get_bcopy(ep_, cb, cb_arg, length, remote_addr, rkey, comp);
}

ucs_status_t UctEndpoint::unpack_rkey(const void* rkey_buffer, uct_rkey_bundle_t* bundle) {
    return uct_rkey_unpack(ctx_.get_component(), rkey_buffer, bundle);
}

// Callback for completion counting (no-op)
static void flush_completion_cb(uct_completion_t * /*self*/) {}

void UctEndpoint::flush() {
    uct_completion_t comp;
    comp.func = flush_completion_cb;
    comp.count = 2; // Initial count 1 (local) + 1 (flush operation)
    
    ucs_status_t status = uct_ep_flush(ep_, 0, &comp);
    if (status == UCS_OK) {
        return;
    }
    
    if (status == UCS_INPROGRESS) {
        // Decrement local reference and wait for flush to complete (count reaches 0)
        comp.count--; 
        while (comp.count > 0) {
            ctx_.progress();
        }
        return;
    }
    
    std::cerr << "Flush failed: " << ucs_status_string(status) << std::endl;
}

} // namespace
