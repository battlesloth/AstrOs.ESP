#ifndef ASTROSANIMATIONENGINE_HPP
#define ASTROSANIMATIONENGINE_HPP

#include <AnimationCommand.hpp>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace AstrOsAnimationEngine
{
    // Parses a semicolon-delimited animation script into a reversed
    // event list ready for back-to-front dispatch. Empty segments are
    // skipped. Returns an empty vector on empty input.
    std::vector<AnimationCommand> parseAnimationScript(const std::string &script);

    // Result of dispatching the next event from the script event list.
    struct NextCommandResult
    {
        std::unique_ptr<CommandTemplate> command;
        int delayMs = 0;
        bool scriptDone = true;
    };

    // Pops the last event from `events` (which is stored in reverse
    // order) and returns the command template, the delay until the next
    // event, and whether the script is now finished. The caller owns
    // the mutex around `events`.
    NextCommandResult getNextCommand(std::vector<AnimationCommand> &events);

} // namespace AstrOsAnimationEngine

// Circular buffer queue for script IDs. Pure index math — the MIXED
// adapter wraps all operations with a mutex.
template <size_t Capacity> class ScriptQueue
{
public:
    ScriptQueue() : front_(0), rear_(-1), size_(0)
    {
        for (auto &s : items_)
        {
            s = "";
        }
    }

    bool push(const std::string &id)
    {
        if (isFull())
        {
            return false;
        }
        rear_ = (rear_ + 1) % static_cast<int>(Capacity);
        items_[rear_] = id;
        size_++;
        return true;
    }

    std::string pop()
    {
        if (isEmpty())
        {
            return "";
        }
        std::string result = items_[front_];
        items_[front_] = "";
        front_ = (front_ + 1) % static_cast<int>(Capacity);
        size_--;
        return result;
    }

    bool isEmpty() const
    {
        return size_ == 0;
    }
    bool isFull() const
    {
        return size_ == static_cast<int>(Capacity);
    }
    int size() const
    {
        return size_;
    }

    void clear()
    {
        for (auto &s : items_)
        {
            s = "";
        }
        front_ = 0;
        rear_ = -1;
        size_ = 0;
    }

private:
    std::array<std::string, Capacity> items_;
    int front_;
    int rear_;
    int size_;
};

#endif
