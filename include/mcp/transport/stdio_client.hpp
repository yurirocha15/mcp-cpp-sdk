#pragma once

#include <mcp/core/executor.hpp>
#include <mcp/core/export.hpp>
#include <mcp/transport/transport.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp {

/**
 * @brief What a launched server's standard error stream is connected to.
 */
enum class StderrMode : std::uint8_t {
    eInherit,  ///< The client process's own standard error.
    eCapture,  ///< Read line by line and passed to StdioProcessOptions::on_stderr_line.
    eDiscard,  ///< The null device.
};

/**
 * @brief How a launched server process ended.
 */
struct ProcessExit {
    /// The process was ended by a signal (POSIX only); #code is then the signal number.
    bool signaled = false;
    /// The exit status, or the signal number when #signaled. On Windows, the process exit code. -1
    /// when the system did not report a status (POSIX, SIGCHLD ignored by the application).
    int code = 0;
    /// The transport had begun shutting the process down (close() or shutdown()) before it ended.
    /// False means the process ended on its own: an unexpected termination.
    bool requested = false;
    /// The process outlived the wait after its standard input closed, and was ended with SIGTERM or
    /// SIGKILL (TerminateProcess on Windows).
    bool forced = false;
};

/**
 * @brief How StdioClientTransport launches and supervises a server process.
 */
struct StdioProcessOptions {
    /// The program to run. A name without a directory is searched for in `PATH`.
    std::string command;
    /// Arguments after the program name.
    std::vector<std::string> arguments;
    /// Variables set for the process; they replace inherited variables of the same name.
    std::map<std::string, std::string> environment;
    /// Start from this process's environment plus #environment. False starts from #environment alone.
    bool inherit_environment = true;
    /// The directory the process starts in; unset keeps this process's.
    std::optional<std::filesystem::path> working_directory;
    /// What standard error is connected to.
    StderrMode stderr_mode = StderrMode::eInherit;
    /// Receives each line the server writes to standard error when #stderr_mode is
    /// StderrMode::eCapture, without its line ending. A line ends at a newline; text left when the
    /// stream ends is delivered as a last line. Runs on the transport's executor and must not block.
    std::function<void(std::string_view)> on_stderr_line;
    /// Invoked once when the process has ended, whoever ended it; ProcessExit::requested tells an
    /// unexpected termination from a shutdown. Runs on the transport's executor and must not block.
    std::function<void(const ProcessExit&)> on_exit;
    /// How long the process may take to exit after its standard input is closed, before it is sent
    /// SIGTERM (on Windows, before TerminateProcess).
    std::chrono::milliseconds stdin_close_wait{std::chrono::seconds(2)};
    /// How long it may take to exit after SIGTERM, before it is sent SIGKILL. Not used on Windows.
    std::chrono::milliseconds terminate_wait{std::chrono::seconds(2)};
    /// The longest line read from the process, in bytes, without its line ending. A longer one
    /// fails the read and shuts the process down.
    std::size_t max_message_bytes = std::size_t{16} * 1024 * 1024;
    /// Run the server as the leader of its own process group (POSIX) or in a job object that ends
    /// with it (Windows), and send the shutdown signals, or terminate, the whole group or job. Helper
    /// processes the server starts, such as the node process behind an `npx` launcher, then do not
    /// outlive it: whatever remains of the group when the server has ended is killed. On POSIX the
    /// server no longer receives the terminal's keyboard signals (Ctrl-C) directly. False signals the
    /// server process alone. On POSIX, an application that ignores SIGCHLD makes the exit status
    /// unknown and leaves leftover helpers alone.
    bool own_process_tree = true;
};

/**
 * @brief When a stdio server is started again after it ends on its own, by a StdioClientSession or
 *        a Client built from Endpoint::stdio().
 *
 * @details The specification asks a client to restart a server that exits unexpectedly. Restarts
 * are off by default; max_restarts turns them on.
 */
struct StdioRestartPolicy {
    /// How many times the server is restarted over the life of the session or client; zero never
    /// restarts it.
    std::size_t max_restarts = 0;
    /// How long to wait before each start.
    std::chrono::milliseconds delay{std::chrono::milliseconds(500)};
    /// Also restart a server that ends on its own with status 0. By default only an abnormal end (a
    /// non-zero status or a signal) restarts it: a server that exits cleanly of its own accord is
    /// shutting down by choice.
    bool restart_after_clean_exit = false;
};

/**
 * @brief A client transport that launches an MCP server as a child process and speaks to it over
 * the process's standard input and output (the stdio transport of the specification).
 *
 * @details The constructor starts the process. Messages are newline-delimited JSON: write_message()
 * writes one line to the process's standard input and read_message() reads one line from its
 * standard output. What the process writes to standard error goes where
 * StdioProcessOptions::stderr_mode says.
 *
 * Shutdown follows the specification: close() (or shutdown()) closes the process's standard input,
 * waits StdioProcessOptions::stdin_close_wait for it to exit, then sends SIGTERM and waits
 * StdioProcessOptions::terminate_wait, then SIGKILL. On Windows the wait is followed by
 * TerminateProcess. The process's standard output is read and discarded until it ends, so a server
 * that writes while it exits is not blocked.
 *
 * A process that ends on its own makes read_message() throw std::runtime_error, which a Client
 * reports as a closed connection: its pending requests fail and its subscriptions end. The
 * StdioProcessOptions::on_exit callback reports how the process ended. See StdioClientSession for
 * restarting the server.
 *
 * On POSIX systems the first transport created in a process sets SIGPIPE to ignored there when it
 * is still at its default, so a write to a server that has exited fails instead of ending the
 * client. The server itself starts with the default disposition.
 *
 * The destructor closes the standard input and waits, up to the two waits above plus the time the
 * process needs to die, for the process to be gone, so it never leaves one behind. The pipe is
 * closed by the executor's strand, so the destructor never touches it while a write may be in
 * progress; it closes the pipe itself only when the executor is an io_context that has stopped. With
 * any other executor that no longer runs handlers, the process is ended by the waits above.
 *
 * Commands are started directly, not through a shell. On Windows only an `.exe` can be started that
 * way: a `.cmd` or `.bat` script, which is what `npx` and `npm` are, does not start. Launch it
 * explicitly through `cmd.exe`, for example `command = "cmd.exe"` with `arguments = {"/c", "npx.cmd",
 * "-y", "some-server"}`; `cmd.exe` then interprets the arguments itself, so pass it only values
 * the application trusts. The SDK does not wrap commands in `cmd.exe` on its own.
 *
 * Each of read_message() and write_message() may have one call outstanding at a time; Client
 * serializes its writes itself.
 */
class MCP_API StdioClientTransport final : public ITransport {
   public:
    /**
     * @brief Launch the server.
     *
     * @param executor The executor for the transport's asynchronous operations. A multithreaded
     *        io_context is supported.
     * @param options How to launch the process.
     * @throws std::runtime_error If the process cannot be started: the program is not found or not
     *         executable, the working directory is missing, or the system refuses.
     * @throws std::invalid_argument If #StdioProcessOptions::command is empty.
     */
    StdioClientTransport(const Executor& executor, StdioProcessOptions options);

    ~StdioClientTransport() override;

    StdioClientTransport(const StdioClientTransport&) = delete;
    StdioClientTransport& operator=(const StdioClientTransport&) = delete;
    StdioClientTransport(StdioClientTransport&&) = delete;
    StdioClientTransport& operator=(StdioClientTransport&&) = delete;

    /**
     * @brief Read the next line from the server's standard output.
     * @throws std::runtime_error When the output ends (the server exited or closed it), the line is
     *         longer than StdioProcessOptions::max_message_bytes, or the transport is closed.
     * @throws std::logic_error When another read is outstanding.
     */
    Task<std::string> read_message() override;

    /**
     * @brief Write @p message and a newline to the server's standard input.
     * @throws std::invalid_argument If @p message contains a newline.
     * @throws std::runtime_error If the transport is closed or the server no longer reads.
     * @throws std::logic_error When another write is outstanding.
     */
    Task<void> write_message(std::string_view message) override;

    /**
     * @brief Begin shutting the process down and return without waiting for it.
     *
     * @details Closes the server's standard input, wakes a pending read and starts the escalation
     * described above on a thread of the transport's own. Safe to call more than once and from any
     * thread; shutdown() waits for the end.
     */
    void close() override;

    /**
     * @brief Shut the process down and wait until it has ended.
     * @return How it ended.
     */
    Task<ProcessExit> shutdown();

    /// How the process ended, or nothing while it runs.
    [[nodiscard]] std::optional<ProcessExit> exit_status() const;

    /// The operating system's identifier of the process.
    [[nodiscard]] std::uint64_t process_id() const noexcept;

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace mcp
