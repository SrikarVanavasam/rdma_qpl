#include "qpl/c_api/cxl.h"
#include "cxl_client.hpp"

using namespace qpl::ml::dispatcher;

extern "C" qpl_status qpl_cxl_initialize(const char* server_ip, const char* cxl_bdf, int numa_node) {
    if (CxlClient::get_instance().initialize(server_ip, cxl_bdf, numa_node)) {
        return QPL_STS_OK;
    }
    return QPL_STS_INIT_WORK_QUEUES_NOT_AVAILABLE; // Represents initialization failure
}

extern "C" qpl_status qpl_cxl_register_buffer(void* buffer, size_t size, uint64_t* out_iova) {
    if (CxlClient::get_instance().register_buffer(buffer, size, out_iova)) {
        return QPL_STS_OK;
    }
    return QPL_STS_NO_MEM_ERR; 
}

extern "C" qpl_status qpl_cxl_register_completion_buffer(void* buffer, size_t size, uint64_t* out_iova) {
    if (CxlClient::get_instance().register_completion_buffer(buffer, size, out_iova)) {
        return QPL_STS_OK;
    }
    return QPL_STS_NO_MEM_ERR; 
}

extern "C" qpl_status qpl_cxl_deregister_buffer(void* buffer) {
    if (CxlClient::get_instance().deregister_buffer(buffer)) {
        return QPL_STS_OK;
    }
    return QPL_STS_NO_MEM_ERR;
}
