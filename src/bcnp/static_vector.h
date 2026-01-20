#pragma once

/**
 * @file static_vector.h
 * @brief Fixed-capacity vector with stack allocation (no heap).
 * 
 * BCNP v3.6.0: Migrated to CrabLib foundation.
 */

#include <crab/static_vector.h>

#include <cstddef>
#include <initializer_list>
#include <stdexcept>

namespace bcnp {

/**
 * @brief Fixed-capacity vector with no heap allocation.
 * 
 * @tparam T Element type
 * @tparam Capacity Maximum number of elements
 */
template<typename T, std::size_t Capacity>
class StaticVector {
public:
    using value_type = T;
    using size_type = std::size_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;
    using iterator = T*;
    using const_iterator = const T*;

    /// @brief Default constructor - creates empty vector.
    StaticVector() noexcept = default;

    /// @brief Construct from initializer list.
    StaticVector(std::initializer_list<T> init) {
        if (init.size() > Capacity) {
            throw std::out_of_range("StaticVector initializer list too large");
        }
        for (const auto& value : init) {
            push_back(value);
        }
    }

    /// @brief Copy constructor.
    StaticVector(const StaticVector& other) {
        for (size_type i = 0; i < other.size(); ++i) {
            push_back(other[i]);
        }
    }

    /// @brief Move constructor.
    StaticVector(StaticVector&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        for (size_type i = 0; i < other.size(); ++i) {
            // Size already verified via other.size(), safe to ignore Result
            (void)m_inner.try_push_back(std::move(other.m_inner[i]));
        }
        other.clear();
    }

    /// @brief Copy assignment.
    StaticVector& operator=(const StaticVector& other) {
        if (this != &other) {
            clear();
            for (size_type i = 0; i < other.size(); ++i) {
                push_back(other[i]);
            }
        }
        return *this;
    }

    /// @brief Move assignment.
    StaticVector& operator=(StaticVector&& other) noexcept(
        std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>) {
        if (this != &other) {
            clear();
            for (size_type i = 0; i < other.size(); ++i) {
                // Size already verified via other.size(), safe to ignore Result
                (void)m_inner.try_push_back(std::move(other.m_inner[i]));
            }
            other.clear();
        }
        return *this;
    }

    ~StaticVector() = default;

    // ========================================================================
    // Capacity
    // ========================================================================
    
    [[nodiscard]] size_type size() const noexcept { return m_inner.size(); }
    [[nodiscard]] constexpr size_type capacity() const noexcept { return Capacity; }
    [[nodiscard]] bool empty() const noexcept { return m_inner.empty(); }
    [[nodiscard]] bool is_full() const noexcept { return m_inner.is_full(); }

    // ========================================================================
    // Iterators
    // ========================================================================
    
    iterator begin() noexcept { return m_inner.begin(); }
    iterator end() noexcept { return m_inner.end(); }
    const_iterator begin() const noexcept { return m_inner.begin(); }
    const_iterator end() const noexcept { return m_inner.end(); }
    const_iterator cbegin() const noexcept { return m_inner.cbegin(); }
    const_iterator cend() const noexcept { return m_inner.cend(); }

    // ========================================================================
    // Element Access
    // ========================================================================
    
    /// @brief Bounds-checked access (throws on out-of-range).
    reference at(size_type index) {
        if (index >= size()) {
            throw std::out_of_range("StaticVector index out of range");
        }
        return m_inner.unchecked(index);
    }

    const_reference at(size_type index) const {
        if (index >= size()) {
            throw std::out_of_range("StaticVector index out of range");
        }
        return m_inner.unchecked(index);
    }

    /// @brief Unchecked access (no bounds checking).
    [[nodiscard]] reference operator[](size_type index) noexcept {
        return m_inner[index];
    }

    [[nodiscard]] const_reference operator[](size_type index) const noexcept {
        return m_inner[index];
    }

    pointer data() noexcept { return m_inner.data(); }
    const_pointer data() const noexcept { return m_inner.data(); }

    reference front() noexcept { return m_inner.front(); }
    const_reference front() const noexcept { return m_inner.front(); }
    reference back() noexcept { return m_inner.back(); }
    const_reference back() const noexcept { return m_inner.back(); }

    // ========================================================================
    // Modifiers
    // ========================================================================
    
    void clear() noexcept { m_inner.clear(); }

    /// @brief Add element by copy (throws if full).
    void push_back(const T& value) {
        auto result = m_inner.try_push_back(value);
        if (result.is_err()) {
            throw std::out_of_range("StaticVector capacity exceeded");
        }
    }

    /// @brief Add element by move (throws if full).
    void push_back(T&& value) {
        auto result = m_inner.try_push_back(std::move(value));
        if (result.is_err()) {
            throw std::out_of_range("StaticVector capacity exceeded");
        }
    }

    /// @brief Emplace element (throws if full).
    template<typename... Args>
    reference emplace_back(Args&&... args) {
        auto result = m_inner.try_emplace_back(std::forward<Args>(args)...);
        if (result.is_err()) {
            throw std::out_of_range("StaticVector capacity exceeded");
        }
        return back();
    }

    void pop_back() noexcept { (void)m_inner.pop_back(); }

    void resize(size_type new_size) {
        if (new_size > Capacity) {
            throw std::out_of_range("StaticVector resize exceeds capacity");
        }
        while (size() > new_size) {
            pop_back();
        }
        while (size() < new_size) {
            auto result = m_inner.try_emplace_back();
            if (result.is_err()) break;
        }
    }

    void resize(size_type new_size, const T& value) {
        if (new_size > Capacity) {
            throw std::out_of_range("StaticVector resize exceeds capacity");
        }
        while (size() > new_size) {
            pop_back();
        }
        while (size() < new_size) {
            push_back(value);
        }
    }

    /// @brief Reserve (no-op, for API compatibility).
    void reserve(size_type requested) {
        if (requested > Capacity) {
            throw std::out_of_range("StaticVector reserve exceeds capacity");
        }
    }

private:
    crab::StaticVector<T, Capacity> m_inner;
};

} // namespace bcnp