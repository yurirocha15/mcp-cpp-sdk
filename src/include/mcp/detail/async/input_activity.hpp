#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/prefer.hpp>
#include <boost/asio/query.hpp>
#include <boost/asio/require.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace mcp::detail {

/**
 * @brief What the handler of a multi round-trip request is doing, from which the request tells
 *        when every part of the handler waits.
 *
 * @details Counts the questions the handler's Context has open, and, for a handler that runs on a
 * CountingExecutor, the functions that executor was given and has not finished. Both counts share
 * one atomic word, so that a reader sees them as they were at one moment.
 */
class InputActivity {
   public:
    /// A question of the handler's Context, from its call until its answer or failure.
    void open_question() noexcept { state_.fetch_add(g_question, std::memory_order_acq_rel); }
    void close_question() noexcept { state_.fetch_sub(g_question, std::memory_order_acq_rel); }

    /// A function of the handler, from when its executor is given it until it has run or is
    /// destroyed unrun.
    void begin_work() noexcept { state_.fetch_add(g_work, std::memory_order_acq_rel); }
    void end_work() noexcept { state_.fetch_sub(g_work, std::memory_order_acq_rel); }

    /// Whether no function of the handler runs or waits to run and no more than @p asked of its
    /// questions are open, so that every question the handler has open is among those @p asked.
    [[nodiscard]] bool settled(std::size_t asked) const noexcept {
        const auto state = state_.load(std::memory_order_acquire);
        return (state & g_work_mask) == 0 && (state >> g_question_shift) <= asked;
    }

   private:
    static constexpr unsigned g_question_shift = 32;
    static constexpr std::uint64_t g_work = 1;
    static constexpr std::uint64_t g_question = std::uint64_t{1} << g_question_shift;
    static constexpr std::uint64_t g_work_mask = g_question - 1;

    std::atomic<std::uint64_t> state_{0};
};

/// Counts a question of a Context in its InputActivity while the question is open; does nothing
/// without one.
class OpenQuestion {
   public:
    explicit OpenQuestion(std::shared_ptr<InputActivity> activity) noexcept
        : activity_(std::move(activity)) {
        if (activity_) {
            activity_->open_question();
        }
    }
    OpenQuestion(const OpenQuestion&) = delete;
    OpenQuestion& operator=(const OpenQuestion&) = delete;
    OpenQuestion(OpenQuestion&&) = delete;
    OpenQuestion& operator=(OpenQuestion&&) = delete;
    ~OpenQuestion() {
        if (activity_) {
            activity_->close_question();
        }
    }

   private:
    std::shared_ptr<InputActivity> activity_;
};

/**
 * @brief An executor that runs what it is given on another executor, counted in an InputActivity
 *        until it has run.
 *
 * @details The handler of a multi round-trip request runs on one when it runs on the handler
 * executor. Every coroutine the handler starts on its own executor, such as the operands of
 * Boost.Asio's awaitable operators, and every completion that resumes one, passes through it, so
 * a count of zero means that no part of the handler runs or waits to run there.
 */
class CountingExecutor {
   public:
    CountingExecutor(boost::asio::any_io_executor inner,
                     std::shared_ptr<InputActivity> activity) noexcept
        : inner_(std::move(inner)), activity_(std::move(activity)) {}

    template <typename Property>
    [[nodiscard]] auto query(const Property& property) const
        -> decltype(boost::asio::query(std::declval<const boost::asio::any_io_executor&>(), property)) {
        return boost::asio::query(inner_, property);
    }

    template <typename Property, typename = decltype(boost::asio::require(
                                     std::declval<const boost::asio::any_io_executor&>(),
                                     std::declval<const Property&>()))>
    [[nodiscard]] CountingExecutor require(const Property& property) const {
        return {boost::asio::require(inner_, property), activity_};
    }

    template <typename Property, typename = decltype(boost::asio::prefer(
                                     std::declval<const boost::asio::any_io_executor&>(),
                                     std::declval<const Property&>()))>
    [[nodiscard]] CountingExecutor prefer(const Property& property) const {
        return {boost::asio::prefer(inner_, property), activity_};
    }

    template <typename Function>
    void execute(Function&& function) const {
        inner_.execute(Counted<std::decay_t<Function>>(activity_, std::forward<Function>(function)));
    }

    friend bool operator==(const CountingExecutor& first, const CountingExecutor& second) noexcept {
        return first.inner_ == second.inner_ && first.activity_ == second.activity_;
    }
    friend bool operator!=(const CountingExecutor& first, const CountingExecutor& second) noexcept {
        return !(first == second);
    }

   private:
    /// A function given to the executor, counted from its construction until it is destroyed,
    /// whether it ran or not.
    template <typename Function>
    class Counted {
       public:
        template <typename Given>
        Counted(std::shared_ptr<InputActivity> activity, Given&& function)
            : function_(std::forward<Given>(function)), activity_(std::move(activity)) {
            activity_->begin_work();
        }
        // The function moves first: should that throw, @p other still counts it and ends it.
        Counted(Counted&& other) noexcept(std::is_nothrow_move_constructible_v<Function>)
            : function_(std::move(other.function_)), activity_(std::move(other.activity_)) {}
        Counted(const Counted&) = delete;
        Counted& operator=(const Counted&) = delete;
        Counted& operator=(Counted&&) = delete;
        ~Counted() {
            if (activity_) {
                activity_->end_work();
            }
        }

        void operator()() { function_(); }

       private:
        Function function_;
        std::shared_ptr<InputActivity> activity_;
    };

    boost::asio::any_io_executor inner_;
    std::shared_ptr<InputActivity> activity_;
};

}  // namespace mcp::detail
