#include "shm_defs.h"

#include <cerrno>

namespace breeze::shm
{

std::expected<ChannelLayout, int> ComputeLayout(std::uint64_t tx_capacity,
                                                std::uint64_t rx_capacity,
                                                std::uint32_t tx_max_payload,
                                                std::uint32_t rx_max_payload,
                                                std::uint32_t page_size,
                                                std::uint64_t max_ring_capacity)
{
    // 页大小：2 的幂；不小于 4096（头与控制块各占一页，大小见 shm_defs.h 的 static_assert）；
    // 不超过最小容量，容量才一定是页的整数倍。
    if (!IsPowerOfTwo(page_size) || page_size < 4096 || page_size > kMinRingCapacity)
        return std::unexpected(EINVAL);
    if (!IsValidCapacity(tx_capacity, max_ring_capacity) || !IsValidCapacity(rx_capacity, max_ring_capacity))
        return std::unexpected(EINVAL);
    if (!IsValidMaxPayload(tx_max_payload, tx_capacity) || !IsValidMaxPayload(rx_max_payload, rx_capacity))
        return std::unexpected(EINVAL);

    ChannelLayout l;
    l.page_size = page_size;
    l.tx.capacity = tx_capacity;
    l.tx.max_payload = tx_max_payload;
    l.rx.capacity = rx_capacity;
    l.rx.max_payload = rx_max_payload;
    l.tx.control_offset = page_size;
    l.rx.control_offset = 2ULL * page_size;
    l.tx.data_offset = 3ULL * page_size;
    l.rx.data_offset = l.tx.data_offset + tx_capacity;
    l.total_size = l.rx.data_offset + rx_capacity;
    return l;
}

} // namespace breeze::shm
