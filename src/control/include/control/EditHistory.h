#pragma once

#include "control/ControlTypes.h"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace winerose::control {

/**
 * @brief UI-agnostic undo/redo of parameter edits (replaces JUCE's UndoManager so the JUCE editor and a
 *        future web UI share one history — PLAN.md §1.2).
 *
 * Edits recorded between beginGroup()/endGroup() (a knob drag) become ONE undo step; repeated edits of the
 * same key inside a group collapse to (first before, last after). Groups nest; only the outermost closes.
 */
class EditHistory {
public:
    struct Edit {
        std::string nsKey;
        ParamValue  before;
        ParamValue  after;
    };
    using Step = std::vector<Edit>;

    explicit EditHistory(std::size_t maxSteps = 200) : m_maxSteps(maxSteps) {}

    void beginGroup();
    void endGroup();
    void record(Edit edit);

    /** Step to revert (apply each edit's `before` in reverse order), moved onto the redo stack. */
    std::optional<Step> undo();
    /** Step to re-apply (apply each edit's `after` in order), moved back onto the undo stack. */
    std::optional<Step> redo();

    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    void clear();

private:
    void push(Step step);

    std::size_t      m_maxSteps;
    std::deque<Step> m_undo;
    std::deque<Step> m_redo;
    Step             m_open;
    int              m_depth = 0;
};

} // namespace winerose::control
