#include <mcp/core/async.hpp>
#include <mcp/transport/stdio_client.hpp>

#include "mcp/detail/async/spawn_awaitable.hpp"
#include "mcp/detail/constants.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/execution/outstanding_work.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/prefer.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#ifdef _WIN32
#include <boost/asio/windows/stream_handle.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cwchar>
#include <cwctype>
#else
#include <boost/asio/posix/stream_descriptor.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef __APPLE__
// A shared library cannot refer to environ directly on macOS.
#include <crt_externs.h>
#define MCP_ENVIRON (*_NSGetEnviron())
#elif !defined(_WIN32)
extern char** environ;
#define MCP_ENVIRON environ
#endif

namespace mcp {
namespace {

#ifdef _WIN32
using PipeStream = boost::asio::windows::stream_handle;
#else
using PipeStream = boost::asio::posix::stream_descriptor;
#endif

/// Whether a read that failed with @p error found the end of the process's output. A Windows pipe
/// reports the end as ERROR_BROKEN_PIPE ("The pipe has been ended"), not as the end of file.
bool is_end_of_output(const boost::system::error_code& error) {
#ifdef _WIN32
    if (error == boost::asio::error::broken_pipe) {
        return true;
    }
#endif
    return error == boost::asio::error::eof;
}

/// What the thread that waits for the process, the thread that escalates a shutdown and the
/// transport share. Everything but the identifiers is guarded by #mutex.
struct ProcessState {
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool exited = false;
    bool close_requested = false;
    bool forced = false;
    ProcessExit exit;
    std::uint64_t id = 0;
    /// The process leads its own process group (POSIX) or runs in a job object (Windows) that the
    /// shutdown signals as a whole.
    bool own_tree = false;
#ifdef _WIN32
    HANDLE process = nullptr;
    HANDLE job = nullptr;
#else
    pid_t pid = -1;
#endif
};

/// The pipe ends a launch leaves with the parent; a stderr end is unused (empty) unless captured.
struct Launched {
#ifdef _WIN32
    HANDLE stdin_write = nullptr;
    HANDLE stdout_read = nullptr;
    HANDLE stderr_read = nullptr;
#else
    int stdin_write = -1;
    int stdout_read = -1;
    int stderr_read = -1;
#endif
};

#ifndef _WIN32

class UniqueFd {
   public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    ~UniqueFd() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    int release() noexcept { return std::exchange(fd_, -1); }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

   private:
    int fd_ = -1;
};

/// Serializes pipe creation, the close-on-exec setup and fork() among this library's launches, so
/// that on systems without pipe2() a launch in another thread never inherits a half-set-up pipe.
std::mutex& spawn_mutex() {
    static std::mutex s_mutex;
    return s_mutex;
}

/// A pipe whose ends are close-on-exec and above the standard descriptors, so that the launch's
/// dup2() calls never overwrite one of them and no other process inherits them. The caller holds
/// spawn_mutex().
void make_pipe(UniqueFd& read_end, UniqueFd& write_end) {
    int raw[2];
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) ||       \
    defined(__DragonFly__)
    // Both ends are close-on-exec from the moment they exist.
    if (::pipe2(raw, O_CLOEXEC) != 0) {
        throw TransportError(TransportError::Kind::eConnect,
                             std::string("Cannot create a pipe: ") + std::strerror(errno));
    }
#else
    if (::pipe(raw) != 0) {
        throw TransportError(TransportError::Kind::eConnect,
                             std::string("Cannot create a pipe: ") + std::strerror(errno));
    }
    ::fcntl(raw[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(raw[1], F_SETFD, FD_CLOEXEC);
#endif
    UniqueFd first(raw[0]);
    UniqueFd second(raw[1]);
    UniqueFd moved_first(::fcntl(first.get(), F_DUPFD_CLOEXEC, 3));
    UniqueFd moved_second(::fcntl(second.get(), F_DUPFD_CLOEXEC, 3));
    if (moved_first.get() < 0 || moved_second.get() < 0) {
        throw TransportError(TransportError::Kind::eConnect,
                             std::string("Cannot create a pipe: ") + std::strerror(errno));
    }
    read_end = std::move(moved_first);
    write_end = std::move(moved_second);
}

/// The program to execute: @p command when it names a path, else the first executable file of that
/// name in `PATH`.
std::string resolve_program(const std::string& command) {
    if (command.find('/') != std::string::npos) {
        return std::filesystem::absolute(command).string();
    }
    const char* path_env = std::getenv("PATH");
    const std::string search = path_env != nullptr ? path_env : "/usr/local/bin:/usr/bin:/bin";
    std::size_t begin = 0;
    while (begin <= search.size()) {
        auto end = search.find(':', begin);
        if (end == std::string::npos) {
            end = search.size();
        }
        std::string directory = search.substr(begin, end - begin);
        if (directory.empty()) {
            directory = ".";
        }
        const auto candidate = std::filesystem::absolute(std::filesystem::path(directory) / command);
        struct stat info{};
        if (::stat(candidate.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
            ::access(candidate.c_str(), X_OK) == 0) {
            return candidate.string();
        }
        begin = end + 1;
    }
    throw TransportError(TransportError::Kind::eConnect,
                         "Cannot find the server program '" + command + "' in PATH");
}

std::vector<std::string> environment_strings(const StdioProcessOptions& options) {
    std::map<std::string, std::string> merged;
    if (options.inherit_environment && MCP_ENVIRON != nullptr) {
        for (char** entry = MCP_ENVIRON; *entry != nullptr; ++entry) {
            const std::string text = *entry;
            const auto equals = text.find('=');
            if (equals != std::string::npos && equals > 0) {
                merged.emplace(text.substr(0, equals), text.substr(equals + 1));
            }
        }
    }
    for (const auto& variable : options.environment) {
        merged[variable.first] = variable.second;
    }
    std::vector<std::string> strings;
    strings.reserve(merged.size());
    for (const auto& variable : merged) {
        strings.push_back(variable.first + "=" + variable.second);
    }
    return strings;
}

/// Sets SIGPIPE to ignored once, unless the application chose a disposition of its own.
void ignore_broken_pipe_signal() {
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        struct sigaction current{};
        if (::sigaction(SIGPIPE, nullptr, &current) == 0 && current.sa_handler == SIG_DFL &&
            (current.sa_flags & SA_SIGINFO) == 0) {
            ::signal(SIGPIPE, SIG_IGN);
        }
    });
}

Launched launch_process(const StdioProcessOptions& options, ProcessState& state) {
    ignore_broken_pipe_signal();
    const std::string program = resolve_program(options.command);
    std::vector<std::string> argument_strings;
    argument_strings.push_back(options.command);
    argument_strings.insert(argument_strings.end(), options.arguments.begin(), options.arguments.end());
    std::vector<char*> argv;
    argv.reserve(argument_strings.size());
    for (auto& argument : argument_strings) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    auto environment = environment_strings(options);
    std::vector<char*> envp;
    envp.reserve(environment.size());
    for (auto& variable : environment) {
        envp.push_back(variable.data());
    }
    envp.push_back(nullptr);
    const std::string working_directory =
        options.working_directory ? options.working_directory->string() : std::string();

    std::unique_lock<std::mutex> spawn_lock(spawn_mutex());
    UniqueFd stdin_read;
    UniqueFd stdin_write;
    UniqueFd stdout_read;
    UniqueFd stdout_write;
    UniqueFd stderr_read;
    UniqueFd stderr_write;
    UniqueFd null_device;
    make_pipe(stdin_read, stdin_write);
    make_pipe(stdout_read, stdout_write);
    if (options.stderr_mode == StderrMode::eCapture) {
        make_pipe(stderr_read, stderr_write);
    } else if (options.stderr_mode == StderrMode::eDiscard) {
        null_device.reset(::open("/dev/null", O_WRONLY | O_CLOEXEC));
        if (null_device.get() < 0) {
            throw TransportError(TransportError::Kind::eConnect,
                                 std::string("Cannot open the null device: ") + std::strerror(errno));
        }
    }
    UniqueFd report_read;
    UniqueFd report_write;
    make_pipe(report_read, report_write);

    const bool own_tree = options.own_process_tree;
    const int stderr_target = options.stderr_mode == StderrMode::eCapture   ? stderr_write.get()
                              : options.stderr_mode == StderrMode::eDiscard ? null_device.get()
                                                                            : -1;
    // Signals stay blocked across fork() so that no handler of this process runs in the child. The
    // sigset calls are unqualified because macOS defines them as macros.
    sigset_t everything;
    sigset_t previous;
    sigfillset(&everything);
    ::pthread_sigmask(SIG_SETMASK, &everything, &previous);
    const pid_t pid = ::fork();
    if (pid == 0) {
        // Only calls that are safe between fork() and exec() from here on.
        struct sigaction default_action{};
        default_action.sa_handler = SIG_DFL;
        sigemptyset(&default_action.sa_mask);
        ::sigaction(SIGPIPE, &default_action, nullptr);
        sigset_t none;
        sigemptyset(&none);
        ::pthread_sigmask(SIG_SETMASK, &none, nullptr);
        int failure = 0;
        if (own_tree && ::setpgid(0, 0) != 0) {
            failure = errno;
        } else if (::dup2(stdin_read.get(), STDIN_FILENO) < 0 ||
                   ::dup2(stdout_write.get(), STDOUT_FILENO) < 0 ||
                   (stderr_target >= 0 && ::dup2(stderr_target, STDERR_FILENO) < 0)) {
            failure = errno;
        } else if (!working_directory.empty() && ::chdir(working_directory.c_str()) != 0) {
            failure = errno;
        } else {
            ::execve(program.c_str(), argv.data(), envp.data());
            failure = errno;
        }
        const ssize_t ignored = ::write(report_write.get(), &failure, sizeof failure);
        static_cast<void>(ignored);
        ::_exit(mcp::detail::constants::stdio_client::g_EXEC_FAILED_STATUS);
    }
    const int fork_error = errno;
    ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    if (pid > 0 && own_tree) {
        // The child does the same; whichever runs first makes the group, so a signal sent to it
        // right after the launch finds it.
        ::setpgid(pid, pid);
    }
    spawn_lock.unlock();
    if (pid < 0) {
        throw TransportError(
            TransportError::Kind::eConnect,
            std::string("Cannot start the server process: ") + std::strerror(fork_error));
    }

    stdin_read.reset();
    stdout_write.reset();
    stderr_write.reset();
    null_device.reset();
    report_write.reset();
    int failure = 0;
    ssize_t count = 0;
    do {
        count = ::read(report_read.get(), &failure, sizeof failure);
    } while (count < 0 && errno == EINTR);
    if (count > 0) {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        throw TransportError(
            TransportError::Kind::eConnect,
            "Cannot start the server process '" + options.command + "': " + std::strerror(failure));
    }
    state.pid = pid;
    state.id = static_cast<std::uint64_t>(pid);
    state.own_tree = own_tree;
#ifdef __APPLE__
    ::fcntl(stdin_write.get(), F_SETNOSIGPIPE, 1);
#endif
    Launched launched;
    launched.stdin_write = stdin_write.release();
    launched.stdout_read = stdout_read.release();
    launched.stderr_read = stderr_read.release();
    return launched;
}

/// Blocks until the process has ended, then records how. The child is reaped under the mutex, so a
/// signal sent under it never reaches a process id that has been reused.
void wait_for_process(ProcessState& state) {
    siginfo_t info{};
    int waited = 0;
    do {
        waited = ::waitid(P_PID, static_cast<id_t>(state.pid), &info, WEXITED | WNOWAIT);
    } while (waited != 0 && errno == EINTR);
    std::scoped_lock lock(state.mutex);
    if (waited != 0) {
        // The system reaped the child on its own (SIGCHLD is ignored, or another waiter took it):
        // the status is not known and the process id may already belong to another process, so
        // nothing is signalled from here on.
        state.exit.code = -1;
    } else {
        if (state.own_tree) {
            // The process is not reaped yet, so its id, which names its group, cannot be reused:
            // helpers it left behind end with it.
            ::kill(-state.pid, SIGKILL);
        }
        int status = 0;
        pid_t reaped = 0;
        do {
            reaped = ::waitpid(state.pid, &status, 0);
        } while (reaped < 0 && errno == EINTR);
        if (reaped == state.pid) {
            if (WIFSIGNALED(status)) {
                state.exit.signaled = true;
                state.exit.code = WTERMSIG(status);
            } else if (WIFEXITED(status)) {
                state.exit.code = WEXITSTATUS(status);
            }
        } else {
            state.exit.code = -1;
        }
    }
    state.exit.requested = state.close_requested;
    state.exit.forced = state.forced;
    state.exited = true;
    state.changed.notify_all();
}

/// The target of a signal for the process: its whole group when it leads one.
pid_t signal_target(const ProcessState& state) { return state.own_tree ? -state.pid : state.pid; }

/// Asks the process to exit, with the mutex held and the process not yet ended.
void send_terminate(ProcessState& state) { ::kill(signal_target(state), SIGTERM); }

/// Ends the process, with the mutex held and the process not yet ended.
void send_kill(ProcessState& state) { ::kill(signal_target(state), SIGKILL); }

void release_process(ProcessState& /*state*/) {}

#else  // _WIN32

class UniqueHandle {
   public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    ~UniqueHandle() { reset(); }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release() noexcept { return std::exchange(handle_, nullptr); }
    void reset(HANDLE handle = nullptr) noexcept {
        if (valid()) {
            ::CloseHandle(handle_);
        }
        handle_ = handle;
    }

   private:
    HANDLE handle_ = nullptr;
};

std::string windows_error(const std::string& what) {
    return what + " (Windows error " + std::to_string(::GetLastError()) + ")";
}

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        throw std::invalid_argument("The text is not valid UTF-8: " + text);
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                          wide.data(), size);
    return wide;
}

/// Appends @p argument quoted so that CommandLineToArgvW and the C runtime read it back unchanged.
void append_argument(std::wstring& command_line, const std::wstring& argument) {
    if (!command_line.empty()) {
        command_line.push_back(L' ');
    }
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        command_line += argument;
        return;
    }
    command_line.push_back(L'"');
    for (auto it = argument.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != argument.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == argument.end()) {
            command_line.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            command_line.append(backslashes * 2 + 1, L'\\');
            command_line.push_back(L'"');
        } else {
            command_line.append(backslashes, L'\\');
            command_line.push_back(*it);
        }
    }
    command_line.push_back(L'"');
}

std::wstring upper(std::wstring text) {
    for (auto& character : text) {
        character = static_cast<wchar_t>(::towupper(character));
    }
    return text;
}

/// The environment block of the process: the inherited variables unless excluded, then the
/// variables of @p options, sorted by name as CreateProcess expects.
std::wstring environment_block(const StdioProcessOptions& options) {
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> merged;
    if (options.inherit_environment) {
        wchar_t* block = ::GetEnvironmentStringsW();
        if (block != nullptr) {
            for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
                const std::wstring text = entry;
                // A name may start with '=' (such as "=C:"); the separator is the next one.
                const auto equals = text.find(L'=', 1);
                if (equals != std::wstring::npos) {
                    const auto name = text.substr(0, equals);
                    merged.emplace(upper(name), std::make_pair(name, text.substr(equals + 1)));
                }
            }
            ::FreeEnvironmentStringsW(block);
        }
    }
    for (const auto& variable : options.environment) {
        const auto name = widen(variable.first);
        merged[upper(name)] = std::make_pair(name, widen(variable.second));
    }
    std::wstring result;
    for (const auto& variable : merged) {
        result += variable.second.first + L"=" + variable.second.second;
        result.push_back(L'\0');
    }
    if (merged.empty()) {
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}

/// An overlapped pipe: the end the parent keeps is overlapped for asio, the end the child gets is
/// inheritable and synchronous.
void make_pipe(bool parent_reads, UniqueHandle& parent_end, UniqueHandle& child_end) {
    static std::atomic<std::uint64_t> counter{0};
    const std::wstring name = L"\\\\.\\pipe\\mcp-sdk-" + std::to_wstring(::GetCurrentProcessId()) +
                              L"-" + std::to_wstring(counter.fetch_add(1)) + L"-" +
                              std::to_wstring(::GetTickCount64());
    const DWORD open_mode = (parent_reads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) |
                            FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE;
    UniqueHandle parent(
        ::CreateNamedPipeW(name.c_str(), open_mode,
                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                           1, mcp::detail::constants::stdio_client::g_CHUNK_BYTES,
                           mcp::detail::constants::stdio_client::g_CHUNK_BYTES, 0, nullptr));
    if (!parent.valid()) {
        throw TransportError(TransportError::Kind::eConnect, windows_error("Cannot create a pipe"));
    }
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    UniqueHandle child(::CreateFileW(name.c_str(), parent_reads ? GENERIC_WRITE : GENERIC_READ, 0,
                                     &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!child.valid()) {
        throw TransportError(TransportError::Kind::eConnect, windows_error("Cannot open a pipe"));
    }
    parent_end = std::move(parent);
    child_end = std::move(child);
}

UniqueHandle open_null_device() {
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    UniqueHandle device(::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!device.valid()) {
        throw TransportError(TransportError::Kind::eConnect,
                             windows_error("Cannot open the null device"));
    }
    return device;
}

Launched launch_process(const StdioProcessOptions& options, ProcessState& state) {
    std::wstring command_line;
    append_argument(command_line, widen(options.command));
    for (const auto& argument : options.arguments) {
        append_argument(command_line, widen(argument));
    }
    auto environment = environment_block(options);
    const std::wstring working_directory =
        options.working_directory ? options.working_directory->wstring() : std::wstring();

    UniqueHandle stdin_write;
    UniqueHandle stdin_read;
    UniqueHandle stdout_read;
    UniqueHandle stdout_write;
    UniqueHandle stderr_read;
    UniqueHandle stderr_write;
    make_pipe(false, stdin_write, stdin_read);
    make_pipe(true, stdout_read, stdout_write);
    UniqueHandle stderr_child;
    if (options.stderr_mode == StderrMode::eCapture) {
        make_pipe(true, stderr_read, stderr_write);
    } else if (options.stderr_mode == StderrMode::eDiscard) {
        stderr_write = open_null_device();
    } else {
        // The client's own standard error, duplicated so that it is inheritable.
        HANDLE own = ::GetStdHandle(STD_ERROR_HANDLE);
        HANDLE copy = nullptr;
        if (own != nullptr && own != INVALID_HANDLE_VALUE &&
            ::DuplicateHandle(::GetCurrentProcess(), own, ::GetCurrentProcess(), &copy, 0, TRUE,
                              DUPLICATE_SAME_ACCESS)) {
            stderr_write.reset(copy);
        } else {
            stderr_write = open_null_device();
        }
    }

    // The process inherits these three handles and no others.
    std::array<HANDLE, 3> inherited{stdin_read.get(), stdout_write.get(), stderr_write.get()};
    SIZE_T list_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size);
    std::vector<char> list_storage(list_size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_storage.data());
    if (!::InitializeProcThreadAttributeList(list, 1, 0, &list_size)) {
        throw TransportError(TransportError::Kind::eConnect,
                             windows_error("Cannot prepare the process launch"));
    }
    struct ListGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~ListGuard() { ::DeleteProcThreadAttributeList(list); }
    } list_guard{list};
    if (!::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                     inherited.size() * sizeof(HANDLE), nullptr, nullptr)) {
        throw TransportError(TransportError::Kind::eConnect,
                             windows_error("Cannot prepare the process launch"));
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdin_read.get();
    startup.StartupInfo.hStdOutput = stdout_write.get();
    startup.StartupInfo.hStdError = stderr_write.get();
    startup.lpAttributeList = list;
    // The process starts suspended so that it belongs to its job before it can start a helper.
    UniqueHandle job;
    if (options.own_process_tree) {
        job.reset(::CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.valid() || !::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                                       &limits, sizeof(limits))) {
            throw TransportError(TransportError::Kind::eConnect,
                                 windows_error("Cannot create a job object"));
        }
    }
    DWORD creation_flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
    if (job.valid()) {
        creation_flags |= CREATE_SUSPENDED;
    }
    PROCESS_INFORMATION created{};
    if (!::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, creation_flags,
                          environment.data(),
                          working_directory.empty() ? nullptr : working_directory.c_str(),
                          &startup.StartupInfo, &created)) {
        throw TransportError(
            TransportError::Kind::eConnect,
            windows_error("Cannot start the server process '" + options.command + "'"));
    }
    UniqueHandle process_handle(created.hProcess);
    UniqueHandle thread_handle(created.hThread);
    if (job.valid()) {
        if (!::AssignProcessToJobObject(job.get(), created.hProcess) ||
            ::ResumeThread(created.hThread) == static_cast<DWORD>(-1)) {
            const std::string message = windows_error("Cannot start the server process '" +
                                                      options.command + "' in its job object");
            ::TerminateProcess(created.hProcess, 1);
            throw TransportError(TransportError::Kind::eConnect, message);
        }
    }
    thread_handle.reset();
    state.process = process_handle.release();
    state.job = job.release();
    state.own_tree = state.job != nullptr;
    state.id = created.dwProcessId;
    Launched launched;
    launched.stdin_write = stdin_write.release();
    launched.stdout_read = stdout_read.release();
    launched.stderr_read = stderr_read.release();
    return launched;
}

void wait_for_process(ProcessState& state) {
    ::WaitForSingleObject(state.process, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(state.process, &code);
    std::lock_guard lock(state.mutex);
    if (state.job != nullptr) {
        // Helpers the process left behind end with it.
        ::TerminateJobObject(state.job, 1);
    }
    state.exit.code = static_cast<int>(code);
    state.exit.requested = state.close_requested;
    state.exit.forced = state.forced;
    state.exited = true;
    state.changed.notify_all();
}

/// Windows has no signal that asks a process to exit; the wait after closing standard input is
/// followed by TerminateProcess.
void send_terminate(ProcessState& /*state*/) {}

void send_kill(ProcessState& state) {
    if (state.job != nullptr) {
        ::TerminateJobObject(state.job, 1);
    } else {
        ::TerminateProcess(state.process, 1);
    }
}

void release_process(ProcessState& state) {
    if (state.job != nullptr) {
        ::CloseHandle(state.job);
        state.job = nullptr;
    }
    if (state.process != nullptr) {
        ::CloseHandle(state.process);
        state.process = nullptr;
    }
}

#endif

/// Runs on its own thread after a shutdown begins: waits for the process, then ends it.
void escalate(const std::shared_ptr<ProcessState>& state, std::chrono::milliseconds stdin_wait,
              std::chrono::milliseconds terminate_wait) {
    std::unique_lock lock(state->mutex);
    const auto has_exited = [&state] { return state->exited; };
    if (state->changed.wait_for(lock, stdin_wait, has_exited)) {
        return;
    }
    if constexpr (mcp::detail::constants::stdio_client::g_HAS_TERMINATE_STEP) {
        state->forced = true;
        send_terminate(*state);
        if (state->changed.wait_for(lock, terminate_wait, has_exited)) {
            return;
        }
    }
    state->forced = true;
    send_kill(*state);
    state->changed.wait(lock, has_exited);
}

}  // namespace

struct StdioClientTransport::Impl : std::enable_shared_from_this<Impl> {
    Impl(const boost::asio::any_io_executor& executor, StdioProcessOptions process_options)
        : strand(boost::asio::make_strand(executor)),
          options(std::move(process_options)),
          proc(std::make_shared<ProcessState>()),
          stdin_pipe(strand),
          stdout_pipe(strand),
          stderr_pipe(strand),
          chunk(mcp::detail::constants::stdio_client::g_CHUNK_BYTES) {}

    ~Impl() { finish(false); }

    /// Starts the process and the thread that waits for it.
    void start() {
        const auto launched = launch_process(options, *proc);
        {
            std::scoped_lock lock(proc->mutex);
            proc->started = true;
        }
        stdin_pipe.assign(launched.stdin_write);
        stdout_pipe.assign(launched.stdout_read);
#ifdef _WIN32
        const bool has_stderr = launched.stderr_read != nullptr;
#else
        const bool has_stderr = launched.stderr_read >= 0;
#endif
        if (has_stderr) {
            stderr_pipe.assign(launched.stderr_read);
        }
        const std::weak_ptr<Impl> weak = weak_from_this();
        reaper = std::thread([state = proc, strand_copy = strand, weak] {
            wait_for_process(*state);
            boost::asio::post(strand_copy, [weak] {
                if (const auto self = weak.lock()) {
                    self->handle_exit();
                }
            });
        });
        if (has_stderr) {
            auto self = shared_from_this();
            boost::asio::co_spawn(strand, read_stderr(self), boost::asio::detached);
        }
    }

    /// Runs on the strand once the process has ended.
    void handle_exit() {
        ProcessExit exit;
        boost::asio::any_io_executor work;
        {
            std::scoped_lock lock(proc->mutex);
            exit = proc->exit;
            work = std::move(exit_work);
            exit_work = boost::asio::any_io_executor();
        }
        exit_handled = true;
        if (closed.load() && stdout_pipe.is_open()) {
            // Output that remains after a requested shutdown is discarded; a helper that still
            // holds the pipe open must not keep the reader waiting.
            boost::system::error_code ignored;
            stdout_pipe.cancel(ignored);
        }
        for (const auto& waiter : waiters) {
            waiter->cancel();
        }
        waiters.clear();
        if (options.on_exit) {
            try {
                options.on_exit(exit);
            } catch (...) {
                // The callback observes; a failure of its own has no listener.
            }
        }
    }

    /// Closes the standard input and wakes the reader. Runs on the strand, or with nothing else
    /// running when @p drain_output is false (the destructor).
    void close_now(bool drain_output = true) {
        boost::system::error_code ignored;
        if (stdin_pipe.is_open()) {
            stdin_pipe.close(ignored);
        }
        if (stdout_pipe.is_open()) {
            stdout_pipe.cancel(ignored);
            if (drain_output && !draining) {
                draining = true;
                auto self = shared_from_this();
                boost::asio::co_spawn(strand, drain(self), boost::asio::detached);
            }
        }
    }

    /// @param can_post False in the destructor, where nothing may refer back to this object.
    void begin_close(bool can_post = true) {
        if (closed.exchange(true)) {
            return;
        }
        bool running = false;
        {
            std::scoped_lock lock(proc->mutex);
            running = proc->started && !proc->exited;
            // A process whose output has already ended is on its way out by itself.
            proc->close_requested = proc->started && !output_ended.load();
            if (running && can_post) {
                exit_work = boost::asio::prefer(strand.get_inner_executor(),
                                                boost::asio::execution::outstanding_work_t::tracked);
            }
        }
        if (!can_post) {
            close_now(false);
        } else if (strand.running_in_this_thread()) {
            close_now();
        } else {
            boost::asio::post(strand, [self = shared_from_this()] { self->close_now(); });
        }
        if (running) {
            std::scoped_lock lock(thread_mutex);
            if (!escalator.joinable()) {
                escalator =
                    std::thread(escalate, proc, options.stdin_close_wait, options.terminate_wait);
            }
        }
    }

    /// Whether the executor is an io_context's and that io_context has stopped running handlers.
    /// False for any other kind of executor, which cannot be asked.
    bool context_stopped() const {
        // A copy: target() points into the executor it is called on.
        const auto inner = strand.get_inner_executor();
        const auto* io = inner.target<boost::asio::io_context::executor_type>();
        return io != nullptr && io->context().stopped();
    }

    /// Ends the process and joins the threads; the destructor's part of the shutdown.
    void finish(bool can_post) {
        begin_close(can_post);
        if (can_post && context_stopped() && !strand.running_in_this_thread()) {
            // Nothing can run the strand any more, so nothing else touches the pipe and the
            // process sees its standard input close at once. With a context that is still
            // running, or one that cannot be asked, the strand closes the pipe itself; if it never
            // does, the process is ended by the shutdown waits.
            boost::system::error_code ignored;
            if (stdin_pipe.is_open()) {
                stdin_pipe.close(ignored);
            }
        }
        std::thread to_join;
        {
            std::scoped_lock lock(thread_mutex);
            to_join = std::move(escalator);
        }
        if (to_join.joinable()) {
            to_join.join();
        }
        if (reaper.joinable()) {
            reaper.join();
        }
        {
            std::scoped_lock lock(proc->mutex);
            exit_work = boost::asio::any_io_executor();
        }
        release_process(*proc);
    }

    static Task<std::string> read(std::shared_ptr<Impl> self) {
        if (self->reading) {
            throw std::logic_error("A read is already outstanding");
        }
        self->reading = true;
        struct Reset {
            bool& flag;
            ~Reset() { flag = false; }
        } reset{self->reading};
        // Shuts the process down: a server that writes a line past the limit is not read further.
        const auto too_long = [&self] {
            self->begin_close();
            return TransportError(TransportError::Kind::eProtocol,
                                  "The server wrote a line longer than the limit of " +
                                      std::to_string(self->options.max_message_bytes) + " bytes");
        };
        for (;;) {
            if (self->closed.load()) {
                throw TransportError(TransportError::Kind::eClosed, "transport closed");
            }
            // A line that arrived before the end of the output is still returned.
            // The limit counts the message without its line ending, so a line that ends in CRLF, as
            // a Windows program's text-mode output does, fits the same limit as one ending in LF.
            const auto newline = self->pending.find('\n');
            if (newline != std::string::npos) {
                std::string line = self->pending.substr(0, newline);
                self->pending.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line.size() > self->options.max_message_bytes) {
                    throw too_long();
                }
                if (line.empty()) {
                    continue;
                }
                co_return line;
            }
            if (self->pending.size() > self->options.max_message_bytes &&
                (self->pending.size() - 1 != self->options.max_message_bytes ||
                 self->pending.back() != '\r')) {
                throw too_long();
            }
            if (self->read_error) {
                if (is_end_of_output(self->read_error)) {
                    if (!self->pending.empty()) {
                        // The output ended in the middle of a line: the text is the last message.
                        std::string line = std::move(self->pending);
                        self->pending.clear();
                        if (line.back() == '\r') {
                            line.pop_back();
                        }
                        if (line.size() > self->options.max_message_bytes) {
                            throw too_long();
                        }
                        if (!line.empty()) {
                            co_return line;
                        }
                    }
                    throw TransportError(TransportError::Kind::eClosed,
                                         "The server process closed its standard output");
                }
                throw TransportError(TransportError::Kind::eIo,
                                     "Cannot read from the server: " + self->read_error.message(),
                                     self->read_error);
            }
            boost::system::error_code error;
            const auto count = co_await self->stdout_pipe.async_read_some(
                boost::asio::buffer(self->chunk),
                boost::asio::redirect_error(boost::asio::use_awaitable, error));
            self->pending.append(self->chunk.data(), count);
            self->read_error = error;
            if (error && !self->closed.load()) {
                self->output_ended = true;
            }
        }
    }

    static Task<void> write(std::shared_ptr<Impl> self, std::shared_ptr<const std::string> line) {
        if (self->closed.load() || !self->stdin_pipe.is_open()) {
            throw TransportError(TransportError::Kind::eClosed, "transport closed");
        }
        if (self->writing) {
            throw std::logic_error("A write is already outstanding");
        }
        self->writing = true;
        struct Reset {
            bool& flag;
            ~Reset() { flag = false; }
        } reset{self->writing};
        boost::system::error_code error;
        co_await boost::asio::async_write(
            self->stdin_pipe, boost::asio::buffer(*line),
            boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (error) {
            if (self->closed.load()) {
                throw TransportError(TransportError::Kind::eClosed, "transport closed");
            }
            throw TransportError(TransportError::Kind::eIo,
                                 "Cannot write to the server: " + error.message(), error);
        }
    }

    /// Reads and drops what the process writes after a shutdown began, until its output ends.
    static Task<void> drain(std::shared_ptr<Impl> self) {
        for (;;) {
            boost::system::error_code error;
            co_await self->stdout_pipe.async_read_some(
                boost::asio::buffer(self->chunk),
                boost::asio::redirect_error(boost::asio::use_awaitable, error));
            if (error) {
                break;
            }
        }
        boost::system::error_code ignored;
        self->stdout_pipe.close(ignored);
    }

    /// Passes the lines of the process's standard error to the callback until it ends.
    static Task<void> read_stderr(std::shared_ptr<Impl> self) {
        auto deliver = [&self](std::string_view line) {
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            if (self->options.on_stderr_line) {
                try {
                    self->options.on_stderr_line(line);
                } catch (...) {
                    // The callback observes; a failure of its own must not stop the reading.
                }
            }
        };
        std::vector<char> buffer(mcp::detail::constants::stdio_client::g_CHUNK_BYTES);
        for (;;) {
            boost::system::error_code error;
            const auto count = co_await self->stderr_pipe.async_read_some(
                boost::asio::buffer(buffer),
                boost::asio::redirect_error(boost::asio::use_awaitable, error));
            self->stderr_pending.append(buffer.data(), count);
            std::size_t line_start = 0;
            for (auto newline = self->stderr_pending.find('\n', line_start);
                 newline != std::string::npos; newline = self->stderr_pending.find('\n', line_start)) {
                deliver(
                    std::string_view(self->stderr_pending).substr(line_start, newline - line_start));
                line_start = newline + 1;
            }
            self->stderr_pending.erase(0, line_start);
            if (error) {
                break;
            }
        }
        if (!self->stderr_pending.empty()) {
            deliver(self->stderr_pending);
            self->stderr_pending.clear();
        }
        boost::system::error_code ignored;
        self->stderr_pipe.close(ignored);
    }

    /// Resolves once the process has ended and handle_exit() has run.
    static Task<ProcessExit> wait_for_exit(std::shared_ptr<Impl> self) {
        while (!self->exit_handled) {
            auto timer = std::make_shared<boost::asio::steady_timer>(self->strand);
            timer->expires_at(std::chrono::steady_clock::time_point::max());
            self->waiters.push_back(timer);
            boost::system::error_code ignored;
            co_await timer->async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
        }
        std::scoped_lock lock(self->proc->mutex);
        co_return self->proc->exit;
    }

    boost::asio::strand<boost::asio::any_io_executor> strand;
    StdioProcessOptions options;
    std::shared_ptr<ProcessState> proc;
    PipeStream stdin_pipe;
    PipeStream stdout_pipe;
    PipeStream stderr_pipe;
    std::vector<char> chunk;
    std::string pending;                   ///< Output read but not yet returned.
    std::string stderr_pending;            ///< Standard error text without its newline yet.
    boost::system::error_code read_error;  ///< Why the output ended, once it has.
    std::atomic<bool> closed{false};
    std::atomic<bool> output_ended{false};  ///< The process's output ended before a shutdown began.
    bool reading = false;
    bool writing = false;
    bool draining = false;
    bool exit_handled = false;
    /// From a requested shutdown of a running process until handle_exit(), keeps the loop from
    /// running out of work, so that it is still there to call on_exit. Guarded by proc->mutex.
    boost::asio::any_io_executor exit_work;
    std::vector<std::shared_ptr<boost::asio::steady_timer>> waiters;
    std::thread reaper;
    std::mutex thread_mutex;
    std::thread escalator;
};

StdioClientTransport::StdioClientTransport(const Executor& executor, StdioProcessOptions options) {
    if (options.command.empty()) {
        throw std::invalid_argument("StdioProcessOptions::command must not be empty");
    }
    impl_ = std::make_shared<Impl>(executor.get(), std::move(options));
    impl_->start();
}

StdioClientTransport::~StdioClientTransport() { impl_->finish(true); }

Task<std::string> StdioClientTransport::read_message() {
    auto impl = impl_;
    return detail::spawn_awaitable(impl->strand, Impl::read(impl));
}

Task<void> StdioClientTransport::write_message(std::string_view message) {
    if (message.find('\n') != std::string_view::npos) {
        throw std::invalid_argument("A stdio message must not contain a newline");
    }
    auto line = std::make_shared<std::string>(message);
    line->push_back('\n');
    auto impl = impl_;
    std::shared_ptr<const std::string> text = std::move(line);
    co_await detail::spawn_awaitable(impl->strand, Impl::write(impl, std::move(text)));
}

void StdioClientTransport::close() { impl_->begin_close(); }

Task<ProcessExit> StdioClientTransport::shutdown() {
    auto impl = impl_;
    impl->begin_close();
    co_return co_await detail::spawn_awaitable(impl->strand, Impl::wait_for_exit(impl));
}

std::optional<ProcessExit> StdioClientTransport::exit_status() const {
    std::scoped_lock lock(impl_->proc->mutex);
    if (!impl_->proc->exited) {
        return std::nullopt;
    }
    return impl_->proc->exit;
}

std::uint64_t StdioClientTransport::process_id() const noexcept { return impl_->proc->id; }

}  // namespace mcp
