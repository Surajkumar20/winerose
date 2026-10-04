#pragma once

#include "control/ControlTypes.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace winerose::control {

/** RAII handle for an onChange() subscription. Unsubscribes when destroyed or reset. Move-only. */
class Subscription {
public:
    Subscription() = default;
    explicit Subscription(std::function<void()> unsubscribe) : m_unsubscribe(std::move(unsubscribe)) {}
    ~Subscription() { reset(); }

    Subscription(Subscription&& other) noexcept : m_unsubscribe(std::move(other.m_unsubscribe)) { other.m_unsubscribe = nullptr; }
    Subscription& operator=(Subscription&& other) noexcept
    {
        if (this != &other) { reset(); m_unsubscribe = std::move(other.m_unsubscribe); other.m_unsubscribe = nullptr; }
        return *this;
    }
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    void reset()
    {
        if (m_unsubscribe) { auto f = std::move(m_unsubscribe); m_unsubscribe = nullptr; f(); }
    }

private:
    std::function<void()> m_unsubscribe;
};

/**
 * @class IController
 * @brief The ONLY interface a UI talks to (PLAN.md §1.4). No JUCE types, no params/engine types.
 *
 * Today the JUCE editor calls it in-process. Later a WebView editor (inside the plugin window) or an
 * Electron standalone app reaches the same calls through a JSON bridge — every argument and return type
 * has a JSON mapping in control/Json.h.
 *
 * Threading: call from the message/UI thread only. onChange callbacks are delivered on that thread.
 */
class IController {
public:
    virtual ~IController() = default;

    // --- Schema / values ---
    virtual std::vector<ParamSchema> schema() const = 0;
    virtual ParamValue get(std::string_view nsKey) const = 0;

    /** Typed write (clamped to range). false if nsKey is unknown or the value type doesn't fit. */
    virtual bool set(std::string_view nsKey, const ParamValue& value) = 0;

    /** Text write, interpreted per the param's type (ParamRegistry::modify). */
    virtual bool modify(std::string_view nsKey, std::string_view text) = 0;

    // --- Value math for drawing controls (curve-aware) ---
    virtual double toNormalized(std::string_view nsKey, double plain) const = 0;
    virtual double fromNormalized(std::string_view nsKey, double normalized) const = 0;
    virtual std::string format(std::string_view nsKey, double plain) const = 0;

    // --- Gestures: bracket a continuous edit (knob drag) so the host records one automation move
    //     and undo records one step. ---
    virtual void beginGesture(std::string_view nsKey) = 0;
    virtual void endGesture(std::string_view nsKey) = 0;

    // --- State / presets ---
    virtual std::string saveState() const = 0;                       // own format (JSON)
    virtual Result      loadState(const std::string& state) = 0;
    /** Any supported preset container (own format today; .SerumPreset / .fxp in feature/presets). */
    virtual Result      loadPreset(std::span<const std::uint8_t> bytes) = 0;

    // --- Undo ---
    virtual bool undo() = 0;
    virtual bool redo() = 0;
    virtual bool canUndo() const = 0;
    virtual bool canRedo() const = 0;

    // --- Observation ---
    [[nodiscard]] virtual Subscription onChange(std::function<void(const ParamChange&)> callback) = 0;
    virtual MeterSnapshot meters() const = 0;
};

/**
 * @brief Implemented by a host adapter (the JUCE plugin) to receive gesture brackets.
 *        Keeps the control layer free of any host API.
 */
class IGestureSink {
public:
    virtual ~IGestureSink() = default;
    virtual void beginGesture(const std::string& nsKey) = 0;
    virtual void endGesture(const std::string& nsKey) = 0;
};

} // namespace winerose::control
