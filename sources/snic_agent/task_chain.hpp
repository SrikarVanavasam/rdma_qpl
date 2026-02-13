#ifndef SNIC_AGENT_TASK_CHAIN_HPP
#define SNIC_AGENT_TASK_CHAIN_HPP

#include <cstdint>

namespace snic {

// Magic number for task chain validation
constexpr uint32_t TASK_CHAIN_MAGIC = 0x54435348; // "TCSH"
constexpr uint32_t TASK_CHAIN_VERSION = 1;

// Operation types
enum class OpType : uint32_t {
    NOP = 0,
    DECOMPRESS = 1,
    COMPRESS = 2,
    SCAN_EQ = 3,
    SCAN_RANGE = 4,
    SCAN_NE = 5,
    EXPAND = 6,
    SELECT = 7,
    EXTRACT = 8,
    CRC64 = 9,
    // SNIC-local operations (executed on Arm cores)
    AGGREGATE_SUM = 100,
    AGGREGATE_COUNT = 101,
    AGGREGATE_MIN = 102,
    AGGREGATE_MAX = 103,
};

// Buffer flags
constexpr uint32_t BUFFER_FLAG_READ = 0x01;
constexpr uint32_t BUFFER_FLAG_WRITE = 0x02;
constexpr uint32_t BUFFER_FLAG_INTERMEDIATE = 0x04;  // SNIC-managed buffer

// Special buffer index for intermediate storage
constexpr uint32_t BUFFER_INTERMEDIATE = 0xFFFFFFFF;

// Buffer descriptor - describes a memory region
struct BufferDesc {
    uint64_t addr;      // Virtual address (host or SNIC)
    uint32_t rkey;      // RDMA rkey (0 if local/cross-GVMI)
    uint32_t size;      // Buffer size in bytes
    uint32_t flags;     // BUFFER_FLAG_*
    uint32_t reserved;
};

// Operation parameters - union for different op types
struct OpParams {
    union {
        // SCAN_EQ params
        struct {
            uint32_t element_width;   // 1, 2, 4, or 8 bytes
            uint64_t value;           // Value to match
        } scan_eq;
        
        // SCAN_RANGE params
        struct {
            uint32_t element_width;
            uint64_t lower;
            uint64_t upper;
        } scan_range;
        
        // DECOMPRESS params
        struct {
            uint32_t flags;           // Compression flags (e.g., gzip header)
        } decompress;
        
        // CRC64 params
        struct {
            uint64_t polynomial;
            uint64_t seed;
        } crc64;
        
        // Generic padding
        uint8_t raw[64];
    };
};

// Single operation in the chain
struct Operation {
    OpType   op_type;         // What operation to perform
    uint32_t input_buffer;    // Index into buffers[] or BUFFER_INTERMEDIATE
    uint32_t input_offset;    // Offset within input buffer
    uint32_t input_size;      // Size of input (0 = use full buffer)
    uint32_t output_buffer;   // Index into buffers[] or BUFFER_INTERMEDIATE
    uint32_t output_offset;   // Offset within output buffer
    uint32_t reserved[2];
    OpParams params;          // Operation-specific parameters
};

// Task chain header - sent from host to SNIC
struct TaskChainHeader {
    uint32_t magic;           // TASK_CHAIN_MAGIC
    uint32_t version;         // TASK_CHAIN_VERSION
    uint32_t num_operations;  // Number of operations
    uint32_t num_buffers;     // Number of buffer descriptors
    
    // Completion notification (SNIC writes status here)
    uint64_t completion_addr; // Host address for completion
    uint32_t completion_rkey; // RDMA rkey for completion
    uint32_t chain_id;        // Unique ID for this chain
};

// Completion record - written by SNIC to host
struct ChainCompletion {
    uint32_t chain_id;        // Matches TaskChainHeader.chain_id
    uint32_t status;          // 0 = success, non-zero = error
    uint32_t completed_ops;   // Number of operations completed
    uint32_t error_op_idx;    // Index of failed operation (if status != 0)
    uint64_t result_value;    // Result for aggregation ops
    uint64_t output_size;     // Actual output size (if applicable)
    uint64_t elapsed_ns;      // Execution time in nanoseconds
    uint8_t  reserved[32];
};

} // namespace snic

#endif // SNIC_AGENT_TASK_CHAIN_HPP
