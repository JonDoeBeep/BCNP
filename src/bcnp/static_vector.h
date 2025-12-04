#pragma once

#include <cstddef>
#include <initializer_list>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bcnp {

/**
 * @brief Fixed-capacity vector with no heap allocation.
 * 
 * Stores elements in-place using aligned storage. Properly implements
 * the Rule of Five to handle non-trivial types (e.g., std::string).
 * 
 * @tparam T Element type
 * @tparam Capacity Maximum number of elements
 */
template <typename T, std::size_t Capacity>
class StaticVector {
public:
    using value_type = T;
    using size_type = std::size_t;
    using iterator = T*;
    using const_iterator = const T*;

    StaticVector() noexcept = default;

    StaticVector(std::initializer_list<T> init) {
        if (init.size() > Capacity) {
            throw std::out_of_range("StaticVector initializer list too large");
        }
        for (const auto& value : init) {
            push_back(value);
        }
    }

    // Copy constructor - properly copy-constructs each element
    StaticVector(const StaticVector& other) : m_size(0) {
        for (size_type i = 0; i < other.m_size; ++i) {
            push_back(other[i]);
        }
    }

    // Move constructor - properly move-constructs each element
    StaticVector(StaticVector&& other) noexcept(std::is_nothrow_move_constructible_v<T>) 
        : m_size(0) {
        for (size_type i = 0; i < other.m_size; ++i) {
            emplace_back(std::move(other[i]));
        }
        other.clear();
    }

    // Copy assignment - clear and copy
    StaticVector& operator=(const StaticVector& other) {
        if (this != &other) {
            clear();
            for (size_type i = 0; i < other.m_size; ++i) {
                push_back(other[i]);
            }
        }
        return *this;
    }

    // Move assignment - clear and move
    StaticVector& operator=(StaticVector&& other) noexcept(
        std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>) {
        if (this != &other) {
            clear();
            for (size_type i = 0; i < other.m_size; ++i) {
                emplace_back(std::move(other[i]));
            }
            other.clear();
        }
        return *this;
    }

    ~StaticVector() { clear(); }

    size_type size() const noexcept { return m_size; }
    constexpr size_type capacity() const noexcept { return Capacity; }
    bool empty() const noexcept { return m_size == 0; }

    iterator begin() noexcept { return data(); }
    iterator end() noexcept { return data() + m_size; }
    const_iterator begin() const noexcept { return data(); }
    const_iterator end() const noexcept { return data() + m_size; }
    const_iterator cbegin() const noexcept { return data(); }
    const_iterator cend() const noexcept { return data() + m_size; }

    T& operator[](size_type index) noexcept { return data()[index]; }
    const T& operator[](size_type index) const noexcept { return data()[index]; }

    T& at(size_type index) {
        if (index >= m_size) {
            throw std::out_of_range("StaticVector index out of range");
        }
        return data()[index];
    }

    const T& at(size_type index) const {
        if (index >= m_size) {
            throw std::out_of_range("StaticVector index out of range");
        }
        return data()[index];
    }

    T& front() noexcept { return data()[0]; }
    const T& front() const noexcept { return data()[0]; }

    T& back() noexcept { return data()[m_size - 1]; }
    const T& back() const noexcept { return data()[m_size - 1]; }

    T* data() noexcept { return std::launder(reinterpret_cast<T*>(m_storage)); }
    const T* data() const noexcept { return std::launder(reinterpret_cast<const T*>(m_storage)); }

    void clear() noexcept {
        // Always call destructors in reverse order for proper cleanup
        T* storage = data();
        for (size_type i = m_size; i > 0; --i) {
            storage[i - 1].~T();
        }
        m_size = 0;
    }

    void push_back(const T& value) {
        if (m_size >= Capacity) {
            throw std::out_of_range("StaticVector capacity exceeded");
        }
        new (data() + m_size) T(value);
        ++m_size;
    }

    void push_back(T&& value) {
        if (m_size >= Capacity) {
            throw std::out_of_range("StaticVector capacity exceeded");
        }
        new (data() + m_size) T(std::move(value));
        ++m_size;
    }

    template <typename... Args>
    T& emplace_back(Args&&... args) {
        if (m_size >= Capacity) {
            throw std::out_of_range("StaticVector capacity exceeded");
        }
        T* slot = new (data() + m_size) T(std::forward<Args>(args)...);
        ++m_size;
        return *slot;
    }

    void pop_back() noexcept {
        if (m_size > 0) {
            --m_size;
            data()[m_size].~T();
        }
    }

    void resize(size_type new_size) {
        if (new_size > Capacity) {
            throw std::out_of_range("StaticVector resize exceeds capacity");
        }
        while (m_size > new_size) {
            pop_back();
        }
        while (m_size < new_size) {
            emplace_back();
        }
    }

    void resize(size_type new_size, const T& value) {
        if (new_size > Capacity) {
            throw std::out_of_range("StaticVector resize exceeds capacity");
        }
        while (m_size > new_size) {
            pop_back();
        }
        while (m_size < new_size) {
            push_back(value);
        }
    }

    /// No-op for API compatibility with std::vector in generic code.
    /// Throws if requested capacity exceeds static Capacity.
    void reserve(size_type requested) {
        if (requested > Capacity) {
            throw std::out_of_range("StaticVector reserve exceeds capacity");
        }
        // No-op: storage is always pre-allocated
    }

private:
    alignas(T) unsigned char m_storage[sizeof(T) * Capacity]{};
    size_type m_size{0};
};

} // namespace bcnp
