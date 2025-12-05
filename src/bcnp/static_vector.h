#pragma once

/**
 * @file static_vector.h
 * @brief Fixed-capacity vector with stack allocation (no heap).
 * 
 * Provides a std::vector-like interface with compile-time fixed capacity.
 * Ideal for real-time systems where heap allocation is undesirable.
 */

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
 * Stores elements in-place using aligned storage.
 * 
 * @tparam T Element type
 * @tparam Capacity Maximum number of elements
 */
template <typename T, std::size_t Capacity>
class StaticVector {
public:
    using value_type = T;              ///< Element type
    using size_type = std::size_t;      ///< Size/index type
    using iterator = T*;                ///< Mutable iterator type
    using const_iterator = const T*;   ///< Const iterator type

    /** @brief Default constructor - creates empty vector. */
    StaticVector() noexcept = default;

    /**
     * @brief Construct from initializer list.
     * @param init Elements to copy into the vector
     * @throws std::out_of_range if init.size() > Capacity
     */
    StaticVector(std::initializer_list<T> init) {
        if (init.size() > Capacity) {
            throw std::out_of_range("StaticVector initializer list too large");
        }
        for (const auto& value : init) {
            push_back(value);
        }
    }

    /**
     * @brief Copy constructor - deep copies all elements.
     * @param other Vector to copy from
     */
    StaticVector(const StaticVector& other) : m_size(0) {
        for (size_type i = 0; i < other.m_size; ++i) {
            push_back(other[i]);
        }
    }

    /**
     * @brief Move constructor - moves all elements.
     * @param other Vector to move from (left empty after move)
     */
    StaticVector(StaticVector&& other) noexcept(std::is_nothrow_move_constructible_v<T>) 
        : m_size(0) {
        for (size_type i = 0; i < other.m_size; ++i) {
            emplace_back(std::move(other[i]));
        }
        other.clear();
    }

    /**
     * @brief Copy assignment operator.
     * @param other Vector to copy from
     * @return Reference to this
     */
    StaticVector& operator=(const StaticVector& other) {
        if (this != &other) {
            clear();
            for (size_type i = 0; i < other.m_size; ++i) {
                push_back(other[i]);
            }
        }
        return *this;
    }

    /**
     * @brief Move assignment operator.
     * @param other Vector to move from (left empty after move)
     * @return Reference to this
     */
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

    /** @brief Destructor - properly destroys all elements. */
    ~StaticVector() { clear(); }

    /** @brief Get current number of elements. */
    size_type size() const noexcept { return m_size; }
    
    /** @brief Get maximum capacity (compile-time constant). */
    constexpr size_type capacity() const noexcept { return Capacity; }
    
    /** @brief Check if vector is empty. */
    bool empty() const noexcept { return m_size == 0; }

    /** @name Iterators
     * @{ */
    iterator begin() noexcept { return data(); }
    iterator end() noexcept { return data() + m_size; }
    const_iterator begin() const noexcept { return data(); }
    const_iterator end() const noexcept { return data() + m_size; }
    const_iterator cbegin() const noexcept { return data(); }
    const_iterator cend() const noexcept { return data() + m_size; }
    /** @} */

    /**
     * @brief Access element by index (no bounds checking).
     * @param index Element index
     * @return Reference to element
     */
    T& operator[](size_type index) noexcept { return data()[index]; }
    
    /** @copydoc operator[]() */
    const T& operator[](size_type index) const noexcept { return data()[index]; }

    /**
     * @brief Access element with bounds checking.
     * @param index Element index
     * @return Reference to element
     * @throws std::out_of_range if index >= size()
     */
    T& at(size_type index) {
        if (index >= m_size) {
            throw std::out_of_range("StaticVector index out of range");
        }
        return data()[index];
    }

    /** @copydoc at() */
    const T& at(size_type index) const {
        if (index >= m_size) {
            throw std::out_of_range("StaticVector index out of range");
        }
        return data()[index];
    }

    /** @brief Access first element (undefined if empty). */
    T& front() noexcept { return data()[0]; }
    /** @copydoc front() */
    const T& front() const noexcept { return data()[0]; }

    /** @brief Access last element (undefined if empty). */
    T& back() noexcept { return data()[m_size - 1]; }
    /** @copydoc back() */
    const T& back() const noexcept { return data()[m_size - 1]; }

    /** @brief Get pointer to underlying storage. */
    T* data() noexcept { return std::launder(reinterpret_cast<T*>(m_storage)); }
    /** @copydoc data() */
    const T* data() const noexcept { return std::launder(reinterpret_cast<const T*>(m_storage)); }

    /**
     * @brief Remove all elements.
     * 
     * Destroys elements in reverse order for proper cleanup of
     * interdependent objects.
     */
    void clear() noexcept {
        T* storage = data();
        for (size_type i = m_size; i > 0; --i) {
            storage[i - 1].~T();
        }
        m_size = 0;
    }

    /**
     * @brief Add element by copy.
     * @param value Element to copy
     * @throws std::out_of_range if size() >= Capacity
     */
    void push_back(const T& value) {
        if (m_size >= Capacity) {
            throw std::out_of_range("StaticVector capacity exceeded");
        }
        new (data() + m_size) T(value);
        ++m_size;
    }

    /**
     * @brief Add element by move.
     * @param value Element to move from
     * @throws std::out_of_range if size() >= Capacity
     */
    void push_back(T&& value) {
        if (m_size >= Capacity) {
            throw std::out_of_range("StaticVector capacity exceeded");
        }
        new (data() + m_size) T(std::move(value));
        ++m_size;
    }

    /**
     * @brief Construct element in-place at end.
     * @tparam Args Constructor argument types
     * @param args Arguments forwarded to T's constructor
     * @return Reference to newly constructed element
     * @throws std::out_of_range if size() >= Capacity
     */
    template <typename... Args>
    T& emplace_back(Args&&... args) {
        if (m_size >= Capacity) {
            throw std::out_of_range("StaticVector capacity exceeded");
        }
        T* slot = new (data() + m_size) T(std::forward<Args>(args)...);
        ++m_size;
        return *slot;
    }

    /**
     * @brief Remove last element.
     * 
     * Does nothing if vector is empty.
     */
    void pop_back() noexcept {
        if (m_size > 0) {
            --m_size;
            data()[m_size].~T();
        }
    }

    /**
     * @brief Resize the vector.
     * 
     * Shrinking destroys excess elements. Growing default-constructs new ones.
     * 
     * @param new_size Target size
     * @throws std::out_of_range if new_size > Capacity
     */
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

    /**
     * @brief Resize with fill value.
     * @param new_size Target size
     * @param value Value to copy when growing
     * @throws std::out_of_range if new_size > Capacity
     */
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

    /**
     * @brief Reserve capacity (no-op for StaticVector).
     * 
     * Provided for API compatibility with std::vector in generic code.
     * Throws if requested capacity exceeds compile-time Capacity.
     * 
     * @param requested Requested capacity
     * @throws std::out_of_range if requested > Capacity
     */
    void reserve(size_type requested) {
        if (requested > Capacity) {
            throw std::out_of_range("StaticVector reserve exceeds capacity");
        }
        // No-op: storage is always pre-allocated
    }

private:
    alignas(T) unsigned char m_storage[sizeof(T) * Capacity]{};  ///< Raw aligned storage
    size_type m_size{0};  ///< Current element count
};

} // namespace bcnp