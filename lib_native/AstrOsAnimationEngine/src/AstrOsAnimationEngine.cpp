#include "AstrOsAnimationEngine.hpp"

#include <AstrOsStringUtils.hpp>

#include <algorithm>

namespace AstrOsAnimationEngine
{
    std::vector<AnimationCommand> parseAnimationScript(const std::string &script)
    {
        std::vector<AnimationCommand> events;

        auto parts = AstrOsStringUtils::splitString(script, ';');

        for (auto &part : parts)
        {
            if (part.empty())
            {
                continue;
            }
            events.emplace_back(std::move(part));
        }

        std::reverse(events.begin(), events.end());
        return events;
    }

    NextCommandResult getNextCommand(std::vector<AnimationCommand> &events)
    {
        NextCommandResult result;

        if (events.empty())
        {
            result.command = std::make_unique<CommandTemplate>(MODULE_TYPE::NONE, 0, "");
            result.delayMs = 0;
            result.scriptDone = true;
            return result;
        }

        const int duration = events.back().duration;
        result.delayMs = duration < 10 ? 10 : duration;
        result.command = events.back().GetCommandTemplatePtr();
        events.pop_back();
        result.scriptDone = events.empty();
        return result;
    }

} // namespace AstrOsAnimationEngine
