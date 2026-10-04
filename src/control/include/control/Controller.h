#pragma once

#include "control/EditHistory.h"
#include "control/IController.h"

#include "params/ParamListener.h"

#include <memory>
#include <string>

namespace winerose {
class ConfigManager;
class Engine;
class ParamRegistry;
}

namespace winerose::control {

/**
 * @class Controller
 * @brief The concrete IController over one Engine + its ConfigManager.
 *
 * Constructed by whichever host adapter owns the engine (the JUCE plugin today, a standalone/Electron
 * shell later). UIs only ever see it as IController&. Constructing one needs the engine/params headers,
 * which only host adapters link — a UI target can't instantiate it, by design.
 */
class Controller final : public IController, private IParamListener {
public:
    Controller(Engine& engine, std::shared_ptr<ConfigManager> config);
    ~Controller() override;

    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    /** The host adapter's gesture receiver (may be null, e.g. headless tests). Not owned. */
    void setGestureSink(IGestureSink* sink) { m_gestureSink = sink; }

    // IController
    std::vector<ParamSchema> schema() const override;
    ParamValue  get(std::string_view nsKey) const override;
    bool        set(std::string_view nsKey, const ParamValue& value) override;
    bool        modify(std::string_view nsKey, std::string_view text) override;
    double      toNormalized(std::string_view nsKey, double plain) const override;
    double      fromNormalized(std::string_view nsKey, double normalized) const override;
    std::string format(std::string_view nsKey, double plain) const override;
    std::string label(std::string_view nsKey) const override;
    void        beginGesture(std::string_view nsKey) override;
    void        endGesture(std::string_view nsKey) override;
    std::string saveState() const override;
    Result      loadState(const std::string& state) override;
    Result      loadPreset(std::span<const std::uint8_t> bytes) override;
    bool        undo() override;
    bool        redo() override;
    bool        canUndo() const override { return m_history.canUndo(); }
    bool        canRedo() const override { return m_history.canRedo(); }
    Subscription  onChange(std::function<void(const ParamChange&)> callback) override;
    MeterSnapshot meters() const override;

private:
    struct Subscribers;

    // IParamListener
    void onParamChanged(const std::string& nsKey) override;
    void onBatchEnd() override;

    ParamRegistry* resolve(std::string_view nsKey, std::string& keyOut) const;
    bool write(std::string_view nsKey, const ParamValue& value, bool recordHistory);
    void dispatch(const ParamChange& change);

    Engine&                        m_engine;
    std::shared_ptr<ConfigManager> m_config;
    IGestureSink*                  m_gestureSink = nullptr;
    EditHistory                    m_history;
    std::shared_ptr<Subscribers>   m_subscribers;   // shared so a Subscription outliving us is harmless
};

} // namespace winerose::control
