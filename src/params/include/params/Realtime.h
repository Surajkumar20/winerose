#pragma once

#include <cassert>

namespace winerose::realtime {

// Set for the duration of Engine::process(). ConfigManager / ParamRegistry accessors assert it is
// NOT set: they take locks and touch strings, which the audio thread must never do (SPEC §5.0).
// The audio thread reads parameters through ParamHandle instead.
inline thread_local bool t_active = false;

class Scope {
public:
    Scope() noexcept : m_prev(t_active) { t_active = true; }
    ~Scope() { t_active = m_prev; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    bool m_prev;
};

inline bool isActive() noexcept { return t_active; }

} // namespace winerose::realtime

#define WINEROSE_ASSERT_NOT_REALTIME() \
    assert(!::winerose::realtime::isActive() && "ConfigManager/ParamRegistry called from the audio thread — use ParamHandle")
