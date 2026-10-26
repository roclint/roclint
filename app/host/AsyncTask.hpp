#pragma once
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>
#include <stdexcept>
#ifdef ROCLINT_ENABLE_TEST_MOCKS
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#endif

namespace roclint::host {
// Eager tasks execute and resume only on their caller's event-loop thread. Suspended
// frames own their arguments. Shared results let draft durability barriers have multiple waiters.
template<class T = void> class AsyncTask;
template<class T> struct TaskState {
    using Value = std::conditional_t<std::is_void_v<T>, std::monostate, T>;
    std::optional<Value> value;
    std::exception_ptr error;
    bool done{false};
    std::vector<std::function<void()>> continuations;
};
template<class T> struct TaskReturn {
    std::shared_ptr<TaskState<T>> state = std::make_shared<TaskState<T>>();
    template<class U> void return_value(U&& value) { state->value.emplace(std::forward<U>(value)); }
};
template<> struct TaskReturn<void> {
    std::shared_ptr<TaskState<void>> state = std::make_shared<TaskState<void>>();
    void return_void() { state->value.emplace(); }
};
template<class T> class AsyncTask {
public:
    struct promise_type : TaskReturn<T> {
        AsyncTask get_return_object() { return AsyncTask(this->state); }
        std::suspend_never initial_suspend() noexcept { return {}; }
        struct Finish {
            bool await_ready() noexcept { return false; }
            bool await_suspend(std::coroutine_handle<promise_type> handle) noexcept {
                const auto state = handle.promise().state;
                state->done = true;
                auto callbacks = std::move(state->continuations);
                for (auto& callback : callbacks) callback();
                return false;
            }
            void await_resume() noexcept {}
        };
        Finish final_suspend() noexcept { return {}; }
        void unhandled_exception() { this->state->error = std::current_exception(); }
    };
    bool await_ready() const noexcept { return state_->done; }
    void await_suspend(std::coroutine_handle<> continuation) { state_->continuations.push_back([continuation]() { continuation.resume(); }); }
    auto await_resume() {
        if (state_->error) std::rethrow_exception(state_->error);
        if constexpr (!std::is_void_v<T>) return *state_->value;
    }
    void then(std::function<void()> continuation) {
        if (state_->done) continuation(); else state_->continuations.push_back(std::move(continuation));
    }
    bool done() const { return state_->done; }
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    auto waitForTesting(int timeoutMs = 15000) {
        QElapsedTimer timer; timer.start();
        while (!done() && timer.elapsed() < timeoutMs) { QCoreApplication::processEvents(); QThread::msleep(1); }
        if (!done()) throw std::runtime_error("Asynchronous storage test timed out");
        return await_resume();
    }
#endif
private:
    explicit AsyncTask(std::shared_ptr<TaskState<T>> state) : state_(std::move(state)) {}
    std::shared_ptr<TaskState<T>> state_;
};
} // namespace roclint::host
