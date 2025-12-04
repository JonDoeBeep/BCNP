#pragma once

#include "bcnp/static_vector.h"
#include <cstddef>
#include <type_traits>
#include <vector>

namespace bcnp {

/**
 * @brief Type traits to detect container capabilities for packet storage.
 * 
 * A valid packet storage must support:
 * - push_back(const T&) or push_back(T&&)
 * - size() -> size_type
 * - empty() -> bool
 * - clear()
 * - begin() / end() iterators
 * - operator[](size_type) -> T&
 * - reserve(size_type) (can be no-op for fixed storage)
 */

namespace detail {

// Detection idiom helpers
template<typename, typename = void>
struct has_push_back : std::false_type {};

template<typename T>
struct has_push_back<T, std::void_t<decltype(std::declval<T>().push_back(std::declval<typename T::value_type>()))>> 
    : std::true_type {};

template<typename, typename = void>
struct has_size : std::false_type {};

template<typename T>
struct has_size<T, std::void_t<decltype(std::declval<T>().size())>> 
    : std::true_type {};

template<typename, typename = void>
struct has_clear : std::false_type {};

template<typename T>
struct has_clear<T, std::void_t<decltype(std::declval<T>().clear())>> 
    : std::true_type {};

template<typename, typename = void>
struct has_reserve : std::false_type {};

template<typename T>
struct has_reserve<T, std::void_t<decltype(std::declval<T>().reserve(std::size_t{}))>> 
    : std::true_type {};

template<typename, typename = void>
struct has_begin_end : std::false_type {};

template<typename T>
struct has_begin_end<T, std::void_t<decltype(std::declval<T>().begin()), decltype(std::declval<T>().end())>> 
    : std::true_type {};

template<typename, typename = void>
struct has_subscript : std::false_type {};

template<typename T>
struct has_subscript<T, std::void_t<decltype(std::declval<T>()[std::size_t{}])>> 
    : std::true_type {};

} // namespace detail

/**
 * @brief Concept-like check for valid packet storage containers.
 * 
 * Works with std::vector, StaticVector, or any container meeting the interface.
 */
template<typename Container>
struct IsValidPacketStorage {
    static constexpr bool value = 
        detail::has_push_back<Container>::value &&
        detail::has_size<Container>::value &&
        detail::has_clear<Container>::value &&
        detail::has_reserve<Container>::value &&
        detail::has_begin_end<Container>::value &&
        detail::has_subscript<Container>::value;
};

template<typename Container>
inline constexpr bool IsValidPacketStorage_v = IsValidPacketStorage<Container>::value;

/**
 * @brief Default packet storage using heap allocation.
 * 
 * Use for: Large batches, trajectory uploads, config dumps.
 * Pros: Unlimited size, familiar API.
 * Cons: Heap allocation, potential fragmentation in tight loops.
 */
template<typename T>
using DynamicPacketStorage = std::vector<T>;

/**
 * @brief Real-time packet storage using stack allocation.
 * 
 * Use for: Control loop telemetry, command batches.
 * Pros: No heap allocation, predictable performance.
 * Cons: Fixed capacity, throws on overflow.
 * 
 * Default capacity of 64 covers all FRC use cases:
 * - 64 × 12 bytes (EncoderData) = 768 bytes (safe for stack)
 * - 64 × 32 bytes (TrajectoryPoint) = 2 KB (safe for stack)
 */
template<typename T, std::size_t Capacity = 64>
using StaticPacketStorage = StaticVector<T, Capacity>;

/**
 * @brief Alias for the recommended real-time default (64 messages).
 */
template<typename T>
using DefaultRealtimeStorage = StaticPacketStorage<T, 64>;

/**
 * @brief Helper to reserve capacity (no-op for static storage).
 * 
 * This allows generic code to call reserve() without knowing storage type.
 */
template<typename Container>
void ReserveIfPossible(Container& container, std::size_t capacity) {
    if constexpr (detail::has_reserve<Container>::value) {
        container.reserve(capacity);
    }
}

} // namespace bcnp
