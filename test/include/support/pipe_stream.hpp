#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <streambuf>
#include <string>
#include <string_view>

/// A blocking byte pipe for tests that run a stdio client and server in one process: reads wait for
/// data, and end of file follows close().
class PipeBuffer : public std::streambuf {
   public:
    void feed(std::string_view data) {
        {
            std::lock_guard lock(mutex_);
            buffer_.append(data);
        }
        condition_.notify_all();
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        condition_.notify_all();
    }

   protected:
    int_type underflow() override {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return position_ < buffer_.size() || closed_; });
        if (position_ == buffer_.size()) {
            return traits_type::eof();
        }
        return traits_type::to_int_type(buffer_[position_]);
    }

    int_type uflow() override {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return position_ < buffer_.size() || closed_; });
        if (position_ == buffer_.size()) {
            return traits_type::eof();
        }
        return traits_type::to_int_type(buffer_[position_++]);
    }

   private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::string buffer_;
    std::size_t position_{0};
    bool closed_{false};
};

/// Output that lands in a PipeBuffer.
class PipeWriter : public std::streambuf {
   public:
    explicit PipeWriter(PipeBuffer& target) : target_(target) {}

   protected:
    int_type overflow(int_type ch) override {
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
            const char byte = traits_type::to_char_type(ch);
            target_.feed(std::string_view(&byte, 1));
        }
        return traits_type::not_eof(ch);
    }

    std::streamsize xsputn(const char* data, std::streamsize count) override {
        target_.feed(std::string_view(data, static_cast<std::size_t>(count)));
        return count;
    }

   private:
    PipeBuffer& target_;
};
