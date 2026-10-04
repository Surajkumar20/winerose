#include "control/EditHistory.h"

#include <algorithm>

namespace winerose::control {

void EditHistory::beginGroup()
{
    ++m_depth;
}

void EditHistory::endGroup()
{
    if (m_depth == 0) return;
    if (--m_depth == 0 && !m_open.empty()) {
        push(std::move(m_open));
        m_open.clear();
    }
}

void EditHistory::record(Edit edit)
{
    if (edit.before == edit.after) return;
    if (m_depth == 0) {
        push(Step{std::move(edit)});
        return;
    }
    const auto it = std::find_if(m_open.begin(), m_open.end(),
                                 [&](const Edit& e) { return e.nsKey == edit.nsKey; });
    if (it != m_open.end()) it->after = std::move(edit.after);
    else                    m_open.push_back(std::move(edit));
}

void EditHistory::push(Step step)
{
    // A group whose edits all net out to no change isn't worth an undo step.
    step.erase(std::remove_if(step.begin(), step.end(), [](const Edit& e) { return e.before == e.after; }), step.end());
    if (step.empty()) return;
    m_undo.push_back(std::move(step));
    if (m_undo.size() > m_maxSteps) m_undo.pop_front();
    m_redo.clear();
}

std::optional<EditHistory::Step> EditHistory::undo()
{
    if (m_undo.empty()) return std::nullopt;
    Step step = std::move(m_undo.back());
    m_undo.pop_back();
    m_redo.push_back(step);
    return step;
}

std::optional<EditHistory::Step> EditHistory::redo()
{
    if (m_redo.empty()) return std::nullopt;
    Step step = std::move(m_redo.back());
    m_redo.pop_back();
    m_undo.push_back(step);
    return step;
}

void EditHistory::clear()
{
    m_undo.clear();
    m_redo.clear();
    m_open.clear();
    m_depth = 0;
}

} // namespace winerose::control
