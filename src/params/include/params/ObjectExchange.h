#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>

namespace winerose {

/**
 * @brief Hands MUTABLE objects (e.g. an effect with internal state) from the message thread to the audio
 *        thread, which then owns and uses them; replaced objects are retired back for deletion.
 *
 * Same lock-free mechanics as SnapshotExchange (pending slot + SPSC retire ring), but the adopted object is
 * mutable and used only by the audio thread. The message thread must finish constructing (and preparing)
 * an object before publish(); after that it never touches it again until it comes back through
 * collectGarbage(). Publishing nullptr removes the current object.
 */
template<typename T, std::size_t RetireCapacity = 16>
class ObjectExchange {
public:
    ObjectExchange() = default;
    ObjectExchange(const ObjectExchange&) = delete;
    ObjectExchange& operator=(const ObjectExchange&) = delete;

    /** Destroy only when the audio thread no longer calls acquire(). */
    ~ObjectExchange()
    {
        collectGarbage();
        T* pending = m_pending.load(std::memory_order_acquire);
        if (pending != marker()) delete pending;
        delete m_current;
    }

    /** Message thread. nullptr means "no object". */
    void publish(std::unique_ptr<T> object)
    {
        collectGarbage();
        T* incoming = object ? object.release() : marker();
        T* stale = m_pending.exchange(incoming, std::memory_order_acq_rel);
        if (stale != marker()) delete stale;   // never adopted by the audio thread
    }

    /** Audio thread: adopt the newest object if any; returns the current one (may be nullptr). */
    T* acquire() noexcept
    {
        T* next = m_pending.exchange(nullptr, std::memory_order_acq_rel);
        if (next != nullptr) {
            if (m_current != nullptr && !pushRetired(m_current)) assert(false && "ObjectExchange retire ring full");
            m_current = next == marker() ? nullptr : next;
        }
        return m_current;
    }

    /** The current object without adopting (audio thread, or any thread while audio is stopped). */
    T* current() noexcept { return m_current; }

    /** Message thread: delete retired objects. publish() calls it. */
    void collectGarbage()
    {
        while (T* p = popRetired()) delete p;
    }

private:
    // A unique non-null address meaning "publish nothing"; never dereferenced.
    T* marker() noexcept { return reinterpret_cast<T*>(&m_markerStorage); }

    bool pushRetired(T* p) noexcept
    {
        const std::size_t head = m_head.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) % RetireCapacity;
        if (next == m_tail.load(std::memory_order_acquire)) return false;
        m_ring[head] = p;
        m_head.store(next, std::memory_order_release);
        return true;
    }

    T* popRetired() noexcept
    {
        const std::size_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire)) return nullptr;
        T* p = m_ring[tail];
        m_tail.store((tail + 1) % RetireCapacity, std::memory_order_release);
        return p;
    }

    std::atomic<T*>                m_pending { nullptr };
    T*                             m_current = nullptr;   // audio-thread owned
    std::array<T*, RetireCapacity> m_ring {};
    std::atomic<std::size_t>       m_head { 0 };
    std::atomic<std::size_t>       m_tail { 0 };
    unsigned char                  m_markerStorage = 0;
};

} // namespace winerose
