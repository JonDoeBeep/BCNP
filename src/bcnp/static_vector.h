#pragma once

#include <cstddef>
#include <initializer_list>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bcnp {

template <typename T, std::size_t Capacity>
class StaticVector {
public:
    using value_type = T;
    using size_type = std::size_t;
    using iterator = T*;
    using const_iterator = const T*;

    StaticVector() = default;
    StaticVector(std::initializer_list<T> init) {
        if (init.size() > Capacity) {
            throw std::out_of_range("StaticVector initializer list too large");
        }
        for (const auto& value : init) {
            push_back(value);
        }
    }

    ~StaticVector() { clear(); }

    size_type size() const noexcept { return m_size; }
    constexpr size_type capacity() const noexcept { return Capacity; }
    bool empty() const noexcept { return m_size == 0; }

    iterator begin() noexcept { return m_storage; }
    iterator end() noexcept { return m_storage + m_size; }
    const_iterator begin() const noexcept { return m_storage; }
    const_iterator end() const noexcept { return m_storage + m_size; }
    const_iterator cbegin() const noexcept { return m_storage; }
    const_iterator cend() const noexcept { return m_storage + m_size; }

    T& operator[](size_type index) noexcept { return m_storage[index]; }
    const T& operator[](size_type index) const noexcept { return m_storage[index]; }

    T& front() noexcept { return m_storage[0]; }
    const T& front() const noexcept { return m_storage[0]; }

    T& back() noexcept { return m_storage[m_size - 1]; }
    const T& back() const noexcept { return m_storage[m_size - 1]; }

    T* data() noexcept { return m_storage; }
    const T* data() const noexcept { return m_storage; }

    void clear() noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (size_type i = 0; i < m_size; ++i) {
                m_storage[i].~T();
            }
        }
        m_size = 0;
    }

    void push_back(const T& value) {
        if (m_size >= Capacity) {
            throw std::out_of_range("StaticVector capacity exceeded");
        }
        m_storage[m_size++] = value;
    }

    void push_back(T&& value) {
        if (m_size >= Capacity) {
            throw std::out_of_range("StaticVector capacity exceeded");
        }
        m_storage[m_size++] = std::move(value);
    }

    template <typename... Args>
    T& emplace_back(Args&&... args) {
        if (m_size >= Capacity) {
            throw std::out_of_range("StaticVector capacity exceeded");
        }
        T* slot = &m_storage[m_size++];
        new (slot) T(std::forward<Args>(args)...);
        return *slot;
    }

private:
    T m_storage[Capacity]{};
    size_type m_size{0};
};

} // namespace bcnp
