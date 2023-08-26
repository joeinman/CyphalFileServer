#include <socketcan.h>
#include <canard.h>
#include <o1heap.h>
#include <time.h>

#include <uavcan/node/Heartbeat_1_0.h>
#include <uavcan/node/GetInfo_1_0.h>

#define VERSION_MAJOR 1
#define VERSION_MINOR 0
#define VCS_REVISION_ID 0
#define NODE_NAME "meridian.test.node"

O1HeapInstance *heap;
CanardInstance canard;
CanardTxQueue queue;

static CanardMicrosecond getMonotonicMicroseconds()
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        abort();
    }
    return (uint64_t)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
}

static void send(const CanardMicrosecond tx_deadline_usec,
                 const CanardTransferMetadata *const metadata,
                 const size_t payload_size,
                 const void *const payload)
{
    canardTxPush(&queue,
                 &canard,
                 tx_deadline_usec,
                 metadata,
                 payload_size,
                 payload);
}

static void sendResponse(const CanardRxTransfer *const original_request_transfer,
                         const size_t payload_size,
                         const void *const payload)
{
    CanardTransferMetadata meta = original_request_transfer->metadata;
    meta.transfer_kind = CanardTransferKindResponse;
    send(original_request_transfer->timestamp_usec + 1e6, &meta, payload_size, payload);
}

/// Constructs a response to uavcan.node.GetInfo which contains the basic information about this node.
static uavcan_node_GetInfo_Response_1_0 processRequestNodeGetInfo()
{
    uavcan_node_GetInfo_Response_1_0 resp = {0};
    resp.protocol_version.major = CANARD_CYPHAL_SPECIFICATION_VERSION_MAJOR;
    resp.protocol_version.minor = CANARD_CYPHAL_SPECIFICATION_VERSION_MINOR;

    // The hardware version is not populated in this demo because it runs on no specific hardware.
    // An embedded node like a servo would usually determine the version by querying the hardware.

    resp.software_version.major = VERSION_MAJOR;
    resp.software_version.minor = VERSION_MINOR;
    resp.software_vcs_revision_id = VCS_REVISION_ID;

    uint8_t rand_id[16];
    for (auto &value : rand_id)
        value = (uint8_t)rand();
    memcpy(resp.unique_id, rand_id, uavcan_node_GetInfo_Response_1_0_unique_id_ARRAY_CAPACITY_);

    // The node name is the name of the product like a reversed Internet domain name (or like a Java package).
    resp.name.count = strlen(NODE_NAME);
    memcpy(&resp.name.elements, NODE_NAME, resp.name.count);

    // The software image CRC and the Certificate of Authenticity are optional so not populated in this demo.
    return resp;
}

// Handle Received Frames
static void processTransfer(const CanardRxTransfer *const transfer)
{
    if (transfer->metadata.transfer_kind == CanardTransferKindRequest)
    {
        if (transfer->metadata.port_id == uavcan_node_GetInfo_1_0_FIXED_PORT_ID_)
        {
            // The request object is empty so we don't bother deserializing it. Just send the response.
            const uavcan_node_GetInfo_Response_1_0 resp = processRequestNodeGetInfo();
            uint8_t serialized[uavcan_node_GetInfo_Response_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_] = {0};
            size_t serialized_size = sizeof(serialized);
            const int8_t res = uavcan_node_GetInfo_Response_1_0_serialize_(&resp, &serialized[0], &serialized_size);
            if (res >= 0)
                sendResponse(transfer, serialized_size, &serialized[0]);
            else
                assert(false);
        }
    }
}

int main()
{
    // Initialise Random
    struct timespec ts;
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    srand((unsigned)ts.tv_nsec);

    // Create o1heap Instance
    alignas(O1HEAP_ALIGNMENT) static uint8_t heap_arena[1024 * 20] = {0};
    heap = o1heapInit(heap_arena, sizeof(heap_arena));

    // Initialise SocketCAN
    auto socketCAN = socketcanOpen("vcan0", false);
    if (socketCAN < 0)
        return -1;

    // Create libcanard Instance
    canard = canardInit(
        [](CanardInstance *const ins, const size_t amount)
        { return o1heapAllocate(heap, amount); },
        [](CanardInstance *const ins, void *const pointer)
        { o1heapFree(heap, pointer); });
    canard.node_id = 46;
    queue = canardTxInit(100, CANARD_MTU_CAN_CLASSIC);

    // Service servers:
    {
        static CanardRxSubscription rx;
        const int8_t res = canardRxSubscribe(&canard,
                                             CanardTransferKindRequest,
                                             uavcan_node_GetInfo_1_0_FIXED_PORT_ID_,
                                             uavcan_node_GetInfo_Request_1_0_EXTENT_BYTES_,
                                             CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC,
                                             &rx);
        if (res < 0)
            return -res;
    }

    // Main Loop
    auto startTime = getMonotonicMicroseconds();
    while (1)
    {
        // ---------------------------------------------- Handle Loops ---------------------------------------------

        // Timing Variables
        CanardMicrosecond now = getMonotonicMicroseconds();

        // Heartbeat Loop
        static CanardMicrosecond prevHeartbeatLoop = 0;
        if ((now - prevHeartbeatLoop) >= 1e6)
        {
            uavcan_node_Heartbeat_1_0 heartbeat;
            heartbeat.uptime = (now - startTime) / 1e6; // Seconds
            heartbeat.health.value = uavcan_node_Health_1_0_NOMINAL;
            heartbeat.mode.value = uavcan_node_Mode_1_0_OPERATIONAL;
            heartbeat.vendor_specific_status_code = 0;

            uint8_t serialized[uavcan_node_Heartbeat_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_] = {0};
            size_t serialized_size = sizeof(serialized);
            const int8_t err = uavcan_node_Heartbeat_1_0_serialize_(&heartbeat, &serialized[0], &serialized_size);
            if (err >= 0)
            {
                static uint8_t heartbeat_transfer_id; // Must be static or heap-allocated to retain state between calls.
                const CanardTransferMetadata transfer = {
                    .priority = CanardPriorityNominal,
                    .transfer_kind = CanardTransferKindMessage,
                    .port_id = uavcan_node_Heartbeat_1_0_FIXED_PORT_ID_, // This is the subject-ID.
                    .remote_node_id = CANARD_NODE_ID_UNSET,              // Messages cannot be unicast, so use UNSET.
                    .transfer_id = heartbeat_transfer_id++,
                };
                send(now + 1e6, &transfer, serialized_size, &serialized[0]);
            }
            prevHeartbeatLoop = now;
        }

        // ---------------------------------------------------------------------------------------------------------

        // ----------------------------------------- Transmit Queued Frames ----------------------------------------

        for (const CanardTxQueueItem *ti = NULL; (ti = canardTxPeek(&queue)) != NULL;) // Peek at the top of the queue.
        {
            if ((0U == ti->tx_deadline_usec) || (ti->tx_deadline_usec > getMonotonicMicroseconds())) // Check the deadline.
            {
                if (!socketcanPush(socketCAN, &ti->frame, 0))
                    continue; // If the driver is busy, break and retry later.
            }
            canard.memory_free(&canard, canardTxPop(&queue, ti));
        }

        // ---------------------------------------------------------------------------------------------------------

        // ---------------------------------------- Process Received Frames ----------------------------------------

        CanardFrame frame = {0};
        uint8_t buf[CANARD_MTU_CAN_CLASSIC] = {0};
        const int16_t socketcan_result = socketcanPop(socketCAN, &frame, NULL, sizeof(buf), buf, 0, NULL);
        if (socketcan_result == 0)
            continue;

        if (socketcan_result < 0)
            return -socketcan_result;

        CanardRxTransfer transfer = {};
        const int8_t canard_result = canardRxAccept(&canard, getMonotonicMicroseconds(), &frame, 0, &transfer, NULL);
        if (canard_result > 0)
        {
            processTransfer(&transfer);
            canard.memory_free(&canard, (void *)transfer.payload);
        }

        // ---------------------------------------------------------------------------------------------------------
    }
}