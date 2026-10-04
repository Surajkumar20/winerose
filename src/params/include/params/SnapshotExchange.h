#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>

namespace winerose {

/**
 * @brief Hands immutable snapshots from the message thread to the audio thread without locks,
 *        and retires old ones back to the message thread for deletion (SPEC §5.3).
 *
 * - publish()        message thread: queue a new snapshot (replaces one the audio thread hasn't adopted yet).
 * - acquire()        audio thread, once per block: adopt the newest snapshot if any; returns the current one.
 * - collectGarbage() message thread: delete snapshots the audio thread has retired. publish() calls it.
 *
 * The audio thread never allocates or frees: retired snapshots go through a fixed-size SPSC ring.
 * The ring cannot overflow in practice — the audio thread retires at most one snapshot per adoption, and
 * every publish drains the ring first — so a full ring indicates a logic error (asserted; the old snapshot
 * is then leaked rather than freed on the audio thread).
 */
template<typename T, std::size_t RetireCapacity = 64>
class SnapshotExchange {
public:
    SnapshotExchange() = default;
    SnapshotExchange(const SnapshotExchange&) = delete;
    SnapshotExchange& operator=(const SnapshotExchange&) = delete;

    /** Destroy only when the audio thread is no longer calling acquire(). */
    ~SnapshotExchange()
    {
        collectGarbage();
        delete m_pending.load(std::memory_order_acquire);
        delete m_current;
    }

    void publish(std::unique_ptr<T> snapshot)
    {
        collectGarbage();
        T* stale = m_pending.exchange(snapshot.release(), std::memory_order_acq_rel);
        delete stale;   // never seen by the audio thread: its exchange() either took it or didn't
    }

    const T* acquire() noexcept
    {
        if (T* next = m_pending.exchange(nullptr, std::memory_order_acq_rel)) {
            if (m_current != nullptr && !pushRetired(m_current)) {
                assert(false && "SnapshotExchange retire ring full");
            }
            m_current = next;
        }
        return m_current;
    }

    void collectGarbage()
    {
        while (T* p = popRetired()) delete p;
    }

private:
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

    std::atomic<T*>                 m_pending { nullptr };
    T*                              m_current = nullptr;   // audio-thread owned
    std::array<T*, RetireCapacity>  m_ring {};
    std::atomic<std::size_t>        m_head { 0 };          // written by audio thread
    std::atomic<std::size_t>        m_tail { 0 };          // written by message thread
};

} // namespace winerose
