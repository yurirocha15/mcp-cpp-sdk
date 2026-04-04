#include <mcp/server.hpp>
#include <mcp/transport/http_server.hpp>
#include <mcp/transport/stdio.hpp>

#if __has_include(<lldb/API/LLDB.h>)
#include <lldb/API/LLDB.h>
#include <lldb/API/SBHostOS.h>
#else
namespace lldb {
using pid_t = unsigned long long;
enum StateType {
    eStateInvalid,
    eStateUnloaded,
    eStateConnected,
    eStateAttaching,
    eStateLaunching,
    eStateStopped,
    eStateRunning,
    eStateStepping,
    eStateCrashed,
    eStateDetached,
    eStateExited,
    eStateSuspended
};
enum StopReason {
    eStopReasonInvalid,
    eStopReasonNone,
    eStopReasonTrace,
    eStopReasonBreakpoint,
    eStopReasonWatchpoint,
    eStopReasonSignal,
    eStopReasonException,
    eStopReasonExec,
    eStopReasonPlanComplete,
    eStopReasonThreadExiting,
    eStopReasonInstrumentation
};
enum RunMode {
    eOnlyDuringStepping
};
enum LaunchFlags {
    eLaunchFlagStopAtEntry = 1
};
class SBError {
   public:
    bool Fail() const { return false; }
    const char* GetCString() const { return ""; }
};
class SBCommandReturnObject {
   public:
    const char* GetOutput() const { return ""; }
    const char* GetError() const { return ""; }
};
class SBCommandInterpreter {
   public:
    bool IsValid() const { return true; }
    bool HandleCommand(const char*, SBCommandReturnObject&) { return true; }
};
class SBFileSpec {
   public:
    const char* GetFilename() const { return ""; }
};
class SBLineEntry {
   public:
    bool IsValid() const { return true; }
    SBFileSpec GetFileSpec() const { return {}; }
    unsigned int GetLine() const { return 0; }
};
class SBFunction {
   public:
    bool IsValid() const { return true; }
    const char* GetName() const { return ""; }
};
class SBAddress {
   public:
    bool IsValid() const { return true; }
    SBFunction GetFunction() const { return {}; }
    SBLineEntry GetLineEntry() const { return {}; }
};
class SBBreakpointLocation {
   public:
    bool IsValid() const { return true; }
    SBAddress GetAddress() const { return {}; }
};
class SBBreakpoint {
   public:
    bool IsValid() const { return true; }
    unsigned int GetID() const { return 0; }
    bool IsEnabled() const { return true; }
    unsigned int GetHitCount() const { return 0; }
    unsigned int GetNumLocations() const { return 0; }
    SBBreakpointLocation GetLocationAtIndex(unsigned int) const { return {}; }
    void SetCondition(const char*) {}
};
class SBModule {
   public:
    bool IsValid() const { return true; }
    SBFileSpec GetFileSpec() const { return {}; }
};
class SBValue {
   public:
    bool IsValid() const { return true; }
    SBError GetError() const { return {}; }
    const char* GetValue() const { return ""; }
    const char* GetTypeName() const { return ""; }
    const char* GetSummary() const { return ""; }
};
class SBFrame {
   public:
    bool IsValid() const { return true; }
    const char* GetFunctionName() const { return ""; }
    SBLineEntry GetLineEntry() const { return {}; }
    SBModule GetModule() const { return {}; }
    SBValue EvaluateExpression(const char*) const { return {}; }
};
class SBThread {
   public:
    bool IsValid() const { return true; }
    unsigned long long GetThreadID() const { return 0; }
    const char* GetName() const { return ""; }
    StopReason GetStopReason() const { return eStopReasonNone; }
    const char* GetQueueName() const { return ""; }
    void GetStopDescription(char* out, unsigned long long) const {
        if (out) {
            *out = '\0';
        }
    }
    unsigned int GetNumFrames() const { return 0; }
    SBFrame GetFrameAtIndex(unsigned int) const { return {}; }
    void StepOver(RunMode) {}
    void StepInto() {}
    void StepOut() {}
};
class SBProcess {
   public:
    bool IsValid() const { return true; }
    unsigned long long GetProcessID() const { return 0; }
    StateType GetState() const { return eStateStopped; }
    SBError Continue() { return {}; }
    unsigned int GetNumThreads() const { return 0; }
    SBThread GetThreadAtIndex(unsigned int) const { return {}; }
};
class SBLaunchInfo {
   public:
    explicit SBLaunchInfo(const char* const*) {}
    void SetLaunchFlags(unsigned int) {}
};
class SBAttachInfo {
   public:
    explicit SBAttachInfo(pid_t) {}
};
class SBListener {
   public:
    bool IsValid() const { return true; }
};
class SBTarget {
   public:
    bool IsValid() const { return true; }
    SBProcess Launch(SBLaunchInfo&, SBError&) { return {}; }
    SBProcess Attach(SBAttachInfo&, SBError&) { return {}; }
    SBProcess AttachToProcessWithName(SBListener&, const char*, bool, SBError&) { return {}; }
    SBProcess GetProcess() const { return {}; }
    SBBreakpoint BreakpointCreateByLocation(const char*, unsigned int) { return {}; }
    SBBreakpoint BreakpointCreateByName(const char*) { return {}; }
    unsigned int GetNumBreakpoints() const { return 0; }
    SBBreakpoint GetBreakpointAtIndex(unsigned int) const { return {}; }
    const char* GetTriple() const { return ""; }
    SBFileSpec GetExecutable() const { return {}; }
};
class SBDebugger {
   public:
    static void Initialize() {}
    static void Terminate() {}
    static SBDebugger Create(bool) { return {}; }
    bool IsValid() const { return true; }
    SBCommandInterpreter GetCommandInterpreter() const { return {}; }
    SBTarget CreateTarget(const char*) const { return {}; }
    SBListener GetListener() const { return {}; }
    unsigned int GetNumTargets() const { return 0; }
    SBTarget GetTargetAtIndex(unsigned int) const { return {}; }
};
}  // namespace lldb
#endif

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace asio = boost::asio;

#if __has_include(<lldb/API/LLDB.h>)
static void set_env(const char* key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    setenv(key, value.c_str(), 1);
#endif
}

static void setup_lldb_server_path() {
    // Check if LLDB_DEBUGSERVER_PATH is already set
    if (std::getenv("LLDB_DEBUGSERVER_PATH") != nullptr) {
        return;  // Already set, skip auto-detection
    }

    // Extract version from lldb::SBDebugger::GetVersionString()
    // Format: "lldb version X.Y.Z" -> extract "X.Y.Z"
    const char* version_str = lldb::SBDebugger::GetVersionString();
    std::string version;
    if (version_str != nullptr) {
        std::string full_str(version_str);
        // Find the last space and take everything after it
        size_t last_space = full_str.rfind(' ');
        if (last_space != std::string::npos && last_space + 1 < full_str.size()) {
            version = full_str.substr(last_space + 1);
        }
    }

    // Platform-specific filesystem search.
    // SBHostOS::GetLLDBPath() would be ideal here but crashes (SIGSEGV in pthread_once)
    // when called before SBDebugger::Initialize(), so we use direct path probing instead.
#ifdef __APPLE__
    // macOS: Search Xcode and Homebrew paths for debugserver
    std::vector<std::filesystem::path> macos_candidates = {
        "/Applications/Xcode.app/Contents/SharedFrameworks/LLDB.framework/Resources/debugserver",
        "/opt/homebrew/opt/llvm/bin/debugserver",  // Homebrew ARM64
        "/usr/local/opt/llvm/bin/debugserver"      // Homebrew Intel
    };
    for (const auto& candidate : macos_candidates) {
        if (std::filesystem::exists(candidate)) {
            set_env("LLDB_DEBUGSERVER_PATH", candidate.string());
            return;
        }
    }

#elif defined(__linux__)
    // Linux: Glob /usr/lib/llvm-*/bin/lldb-server-<version>
    if (!version.empty()) {
        try {
            std::filesystem::path llvm_base("/usr/lib");
            if (std::filesystem::exists(llvm_base)) {
                for (const auto& entry : std::filesystem::directory_iterator(llvm_base)) {
                    if (entry.is_directory()) {
                        std::string dir_name = entry.path().filename().string();
                        // Check if it matches llvm-*
                        if (dir_name.find("llvm-") == 0) {
                            std::string lldb_server_name = "lldb-server-";
                            lldb_server_name += version;
                            std::filesystem::path candidate = entry.path() / "bin" / lldb_server_name;
                            if (std::filesystem::exists(candidate)) {
                                set_env("LLDB_DEBUGSERVER_PATH", candidate.string());
                                return;
                            }
                        }
                    }
                }
            }
        } catch (...) {
            // Silently ignore filesystem errors
        }
    }

#elif defined(_WIN32)
    // Windows: Search C:\Program Files\LLVM\bin\lldb-server.exe
    std::vector<std::filesystem::path> windows_candidates = {
        "C:\\Program Files\\LLVM\\bin\\lldb-server.exe"};
    for (const auto& candidate : windows_candidates) {
        if (std::filesystem::exists(candidate)) {
            set_env("LLDB_DEBUGSERVER_PATH", candidate.string());
            return;
        }
    }
#endif

    // Candidate 3: Plain lldb-server on $PATH (already works, no action needed)
}
#endif

struct LLDBGuard {
    LLDBGuard() {
#if __has_include(<lldb/API/LLDB.h>)
        setup_lldb_server_path();
#endif
        lldb::SBDebugger::Initialize();
    }
    ~LLDBGuard() { lldb::SBDebugger::Terminate(); }
    LLDBGuard(const LLDBGuard&) = delete;
    LLDBGuard& operator=(const LLDBGuard&) = delete;
};

struct AppOptions {
    std::string transport = "stdio";
    std::string host = "127.0.0.1";
    unsigned short port = 9695;
};

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --transport=<stdio|http>  Transport to use (default: stdio)\n"
              << "  --host=<addr>             HTTP listen address (default: 127.0.0.1)\n"
              << "  --port=<n>                HTTP listen port (default: 9695)\n"
              << "  --help                    Show this help\n";
}

enum class ParseResult {
    Ok,
    Help,
    Error
};

static ParseResult parse_args(int argc, char** argv, AppOptions& opts) {
    using namespace std::literals;
    constexpr auto transport_prefix = "--transport="sv;
    constexpr auto host_prefix = "--host="sv;
    constexpr auto port_prefix = "--port="sv;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help"sv) {
            print_usage(argv[0]);
            return ParseResult::Help;
        }

        if (arg.rfind(transport_prefix, 0) == 0) {
            opts.transport = std::string(arg.substr(transport_prefix.size()));
            if (opts.transport != "stdio" && opts.transport != "http") {
                std::cerr << "Invalid transport: " << opts.transport << "\n";
                print_usage(argv[0]);
                return ParseResult::Error;
            }
        } else if (arg.rfind(host_prefix, 0) == 0) {
            opts.host = std::string(arg.substr(host_prefix.size()));
        } else if (arg.rfind(port_prefix, 0) == 0) {
            opts.port =
                static_cast<unsigned short>(std::stoi(std::string(arg.substr(port_prefix.size()))));
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return ParseResult::Error;
        }
    }

    return ParseResult::Ok;
}

static nlohmann::json make_text_result(std::string_view text) {
    return nlohmann::json{{"content", nlohmann::json::array({nlohmann::json{
                                          {"type", "text"}, {"text", std::string(text)}}})}};
}

static nlohmann::json make_error_result(std::string_view msg) {
    return nlohmann::json{
        {"content",
         nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", std::string(msg)}}})},
        {"isError", true}};
}

static std::string safe_cstr(const char* p, std::string_view fallback = "<unknown>") {
    return p ? std::string(p) : std::string(fallback);
}

static std::string state_to_string(lldb::StateType state) {
    switch (state) {
        case lldb::eStateInvalid:
            return "invalid";
        case lldb::eStateUnloaded:
            return "unloaded";
        case lldb::eStateConnected:
            return "connected";
        case lldb::eStateAttaching:
            return "attaching";
        case lldb::eStateLaunching:
            return "launching";
        case lldb::eStateStopped:
            return "stopped";
        case lldb::eStateRunning:
            return "running";
        case lldb::eStateStepping:
            return "stepping";
        case lldb::eStateCrashed:
            return "crashed";
        case lldb::eStateDetached:
            return "detached";
        case lldb::eStateExited:
            return "exited";
        case lldb::eStateSuspended:
            return "suspended";
    }
    return "unknown";
}

static std::string stop_reason_to_string(lldb::StopReason reason) {
    switch (reason) {
        case lldb::eStopReasonInvalid:
            return "invalid";
        case lldb::eStopReasonNone:
            return "none";
        case lldb::eStopReasonTrace:
            return "trace";
        case lldb::eStopReasonBreakpoint:
            return "breakpoint";
        case lldb::eStopReasonWatchpoint:
            return "watchpoint";
        case lldb::eStopReasonSignal:
            return "signal";
        case lldb::eStopReasonException:
            return "exception";
        case lldb::eStopReasonExec:
            return "exec";
        case lldb::eStopReasonPlanComplete:
            return "plan_complete";
        case lldb::eStopReasonThreadExiting:
            return "thread_exiting";
        case lldb::eStopReasonInstrumentation:
            return "instrumentation";
    }
    return "unknown";
}

struct CommandInput {
    std::string command;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CommandInput, command)

struct LaunchInput {
    std::string program;
    std::optional<std::vector<std::string>> args;
    std::optional<bool> stop_at_entry;
};
inline void to_json(nlohmann::json& j, const LaunchInput& x) {
    j = nlohmann::json{{"program", x.program}};
    if (x.args) {
        j["args"] = *x.args;
    }
    if (x.stop_at_entry) {
        j["stop_at_entry"] = *x.stop_at_entry;
    }
}
inline void from_json(const nlohmann::json& j, LaunchInput& x) {
    j.at("program").get_to(x.program);
    if (j.contains("args")) {
        x.args = j.at("args").get<std::vector<std::string>>();
    }
    if (j.contains("stop_at_entry")) {
        x.stop_at_entry = j.at("stop_at_entry").get<bool>();
    }
}

struct AttachInput {
    std::optional<int> pid;
    std::optional<std::string> name;
};
inline void to_json(nlohmann::json& j, const AttachInput& x) {
    j = nlohmann::json::object();
    if (x.pid) {
        j["pid"] = *x.pid;
    }
    if (x.name) {
        j["name"] = *x.name;
    }
}
inline void from_json(const nlohmann::json& j, AttachInput& x) {
    if (j.contains("pid")) {
        x.pid = j.at("pid").get<int>();
    }
    if (j.contains("name")) {
        x.name = j.at("name").get<std::string>();
    }
}

struct BreakpointInput {
    std::optional<std::string> file;
    std::optional<int> line;
    std::optional<std::string> function_name;
    std::optional<std::string> condition;
};
inline void to_json(nlohmann::json& j, const BreakpointInput& x) {
    j = nlohmann::json::object();
    if (x.file) {
        j["file"] = *x.file;
    }
    if (x.line) {
        j["line"] = *x.line;
    }
    if (x.function_name) {
        j["function_name"] = *x.function_name;
    }
    if (x.condition) {
        j["condition"] = *x.condition;
    }
}
inline void from_json(const nlohmann::json& j, BreakpointInput& x) {
    if (j.contains("file")) {
        x.file = j.at("file").get<std::string>();
    }
    if (j.contains("line")) {
        x.line = j.at("line").get<int>();
    }
    if (j.contains("function_name")) {
        x.function_name = j.at("function_name").get<std::string>();
    }
    if (j.contains("condition")) {
        x.condition = j.at("condition").get<std::string>();
    }
}

struct StepInput {
    std::optional<int> thread_index;
    std::optional<std::string> type;
};
inline void to_json(nlohmann::json& j, const StepInput& x) {
    j = nlohmann::json::object();
    if (x.thread_index) {
        j["thread_index"] = *x.thread_index;
    }
    if (x.type) {
        j["type"] = *x.type;
    }
}
inline void from_json(const nlohmann::json& j, StepInput& x) {
    if (j.contains("thread_index")) {
        x.thread_index = j.at("thread_index").get<int>();
    }
    if (j.contains("type")) {
        x.type = j.at("type").get<std::string>();
    }
}

struct EvalInput {
    std::string expression;
    std::optional<int> thread_index;
    std::optional<int> frame_index;
};
inline void to_json(nlohmann::json& j, const EvalInput& x) {
    j = nlohmann::json{{"expression", x.expression}};
    if (x.thread_index) {
        j["thread_index"] = *x.thread_index;
    }
    if (x.frame_index) {
        j["frame_index"] = *x.frame_index;
    }
}
inline void from_json(const nlohmann::json& j, EvalInput& x) {
    j.at("expression").get_to(x.expression);
    if (j.contains("thread_index")) {
        x.thread_index = j.at("thread_index").get<int>();
    }
    if (j.contains("frame_index")) {
        x.frame_index = j.at("frame_index").get<int>();
    }
}

struct BacktraceInput {
    std::optional<int> thread_index;
    std::optional<int> count;
};
inline void to_json(nlohmann::json& j, const BacktraceInput& x) {
    j = nlohmann::json::object();
    if (x.thread_index) {
        j["thread_index"] = *x.thread_index;
    }
    if (x.count) {
        j["count"] = *x.count;
    }
}
inline void from_json(const nlohmann::json& j, BacktraceInput& x) {
    if (j.contains("thread_index")) {
        x.thread_index = j.at("thread_index").get<int>();
    }
    if (j.contains("count")) {
        x.count = j.at("count").get<int>();
    }
}

int main(int argc, char** argv) {
    LLDBGuard lldb_guard;
    using namespace mcp;

    AppOptions opts;
    const auto parse_result = parse_args(argc, argv, opts);
    if (parse_result == ParseResult::Help) {
        return EXIT_SUCCESS;
    }
    if (parse_result == ParseResult::Error) {
        return EXIT_FAILURE;
    }

    ServerCapabilities caps;
    caps.tools = ServerCapabilities::ToolsCapability{.listChanged = true};
    caps.resources = ServerCapabilities::ResourcesCapability{.listChanged = true};
    caps.prompts = ServerCapabilities::PromptsCapability{.listChanged = true};
    caps.logging = nlohmann::json::object();

    Implementation info;
    info.name = "lldb-mcp-server";
    info.version = "1.0.0";
    info.description = "MCP adapter for LLDB SB API";

    Server server(std::move(info), std::move(caps));

    boost::asio::io_context io_ctx;
    asio::any_io_executor io_executor = io_ctx.get_executor();

    auto debugger_ptr = std::make_shared<lldb::SBDebugger>(lldb::SBDebugger::Create(false));
    auto target_ptr = std::make_shared<std::optional<lldb::SBTarget>>(std::nullopt);
    auto listener_ptr = std::make_shared<lldb::SBListener>(lldb::SBListener("mcp-process-listener"));

    nlohmann::json command_schema = {{"type", "object"},
                                     {"properties", {{"command", {{"type", "string"}}}}},
                                     {"required", nlohmann::json::array({"command"})}};
    server.add_tool<CommandInput, nlohmann::json>(
        "lldb_command", "Run raw LLDB command", std::move(command_schema),
        [io_executor, debugger_ptr](CommandInput args) -> mcp::Task<nlohmann::json> {
            (void)io_executor;
            try {
                if (!debugger_ptr->IsValid()) {
                    co_return make_error_result("Debugger is not valid");
                }
                auto interp = debugger_ptr->GetCommandInterpreter();
                if (!interp.IsValid()) {
                    co_return make_error_result("Command interpreter is not valid");
                }
                lldb::SBCommandReturnObject result_obj;
                bool ok = interp.HandleCommand(args.command.c_str(), result_obj);
                if (!ok) {
                    co_return make_error_result(safe_cstr(result_obj.GetError(), "Command failed"));
                }
                co_return make_text_result(safe_cstr(result_obj.GetOutput(), ""));
            } catch (const std::exception& e) {
                co_return make_error_result(e.what());
            }
        });

    nlohmann::json launch_schema = {{"type", "object"},
                                    {"properties",
                                     {{"program", {{"type", "string"}}},
                                      {"args", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                                      {"stop_at_entry", {{"type", "boolean"}}}}},
                                    {"required", nlohmann::json::array({"program"})}};
    server.add_tool<LaunchInput, nlohmann::json>(
        "launch", "Launch program under LLDB", std::move(launch_schema),
        [io_executor, debugger_ptr, target_ptr,
         listener_ptr](LaunchInput args) -> mcp::Task<nlohmann::json> {
            (void)io_executor;
            try {
                if (!debugger_ptr->IsValid()) {
                    co_return make_error_result("Debugger is not valid");
                }
                auto target = debugger_ptr->CreateTarget(args.program.c_str());
                if (!target.IsValid()) {
                    co_return make_error_result("Failed to create target");
                }

                std::vector<std::string> argv_storage;
                if (args.args) {
                    argv_storage = *args.args;
                }
                std::vector<const char*> argv;
                argv.reserve(argv_storage.size() + 1);
                for (const auto& a : argv_storage) {
                    argv.push_back(a.c_str());
                }
                argv.push_back(nullptr);

                *listener_ptr = lldb::SBListener("mcp-process-listener");
                lldb::SBLaunchInfo launch_info(argv.data());
                launch_info.SetLaunchFlags(
                    args.stop_at_entry.value_or(true) ? lldb::eLaunchFlagStopAtEntry : 0);
                launch_info.SetListener(*listener_ptr);
                lldb::SBError error;
                auto process = target.Launch(launch_info, error);
                if (error.Fail() || !process.IsValid()) {
                    co_return make_error_result(safe_cstr(error.GetCString(), "Launch failed"));
                }

                *target_ptr = target;
                lldb::SBEvent drain_ev;
                while (listener_ptr->GetNextEvent(drain_ev)) {
                }
                nlohmann::json out;
                out["pid"] = process.GetProcessID();
                out["state"] = state_to_string(process.GetState());
                out["description"] = "Program launched";
                co_return make_text_result(out.dump());
            } catch (const std::exception& e) {
                co_return make_error_result(e.what());
            }
        });

    nlohmann::json attach_schema = {
        {"type", "object"},
        {"properties", {{"pid", {{"type", "integer"}}}, {"name", {{"type", "string"}}}}}};
    server.add_tool<AttachInput, nlohmann::json>(
        "attach", "Attach to existing process", std::move(attach_schema),
        [io_executor, debugger_ptr, target_ptr,
         listener_ptr](AttachInput args) -> mcp::Task<nlohmann::json> {
            (void)io_executor;
            try {
                if (!debugger_ptr->IsValid()) {
                    co_return make_error_result("Debugger is not valid");
                }
                if (!args.pid && !args.name) {
                    co_return make_error_result("Must provide pid or name");
                }
                auto target = debugger_ptr->CreateTarget("");
                if (!target.IsValid()) {
                    co_return make_error_result("Failed to create target");
                }

                *listener_ptr = lldb::SBListener("mcp-process-listener");
                lldb::SBError error;
                lldb::SBProcess process;
                if (args.pid) {
                    lldb::SBAttachInfo info(static_cast<lldb::pid_t>(*args.pid));
                    info.SetListener(*listener_ptr);
                    process = target.Attach(info, error);
                } else {
                    if (!listener_ptr->IsValid()) {
                        co_return make_error_result("Listener is not valid");
                    }
                    process =
                        target.AttachToProcessWithName(*listener_ptr, args.name->c_str(), false, error);
                }
                if (error.Fail() || !process.IsValid()) {
                    co_return make_error_result(safe_cstr(error.GetCString(), "Attach failed"));
                }

                *target_ptr = target;
                lldb::SBEvent drain_ev;
                while (listener_ptr->GetNextEvent(drain_ev)) {
                }
                nlohmann::json out;
                out["pid"] = process.GetProcessID();
                out["state"] = state_to_string(process.GetState());
                co_return make_text_result(out.dump());
            } catch (const std::exception& e) {
                co_return make_error_result(e.what());
            }
        });

    nlohmann::json bp_schema = {{"type", "object"},
                                {"properties",
                                 {{"file", {{"type", "string"}}},
                                  {"line", {{"type", "integer"}}},
                                  {"function_name", {{"type", "string"}}},
                                  {"condition", {{"type", "string"}}}}}};
    server.add_tool<BreakpointInput, nlohmann::json>(
        "set_breakpoint", "Set file/line or function breakpoint", std::move(bp_schema),
        [io_executor, target_ptr](BreakpointInput args) -> mcp::Task<nlohmann::json> {
            (void)io_executor;
            try {
                if (!target_ptr->has_value() || !target_ptr->value().IsValid()) {
                    co_return make_error_result("No valid current target");
                }
                auto target = target_ptr->value();

                lldb::SBBreakpoint bp;
                if (args.file && args.line) {
                    bp = target.BreakpointCreateByLocation(args.file->c_str(),
                                                           static_cast<unsigned>(*args.line));
                } else if (args.function_name) {
                    bp = target.BreakpointCreateByName(args.function_name->c_str());
                } else {
                    co_return make_error_result("Must provide file+line or function_name");
                }
                if (!bp.IsValid()) {
                    co_return make_error_result("Failed to create breakpoint");
                }
                if (args.condition) {
                    bp.SetCondition(args.condition->c_str());
                }

                nlohmann::json out;
                out["id"] = bp.GetID();
                out["is_valid"] = bp.IsValid();
                out["is_enabled"] = bp.IsEnabled();
                co_return make_text_result(out.dump());
            } catch (const std::exception& e) {
                co_return make_error_result(e.what());
            }
        });

    nlohmann::json continue_schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
    server.add_tool<nlohmann::json, nlohmann::json>(
        "continue_process", "Continue current process", std::move(continue_schema),
        [io_executor, target_ptr, listener_ptr](nlohmann::json) -> mcp::Task<nlohmann::json> {
            (void)io_executor;
            try {
                if (!target_ptr->has_value() || !target_ptr->value().IsValid()) {
                    co_return make_error_result("No valid current target");
                }
                auto process = target_ptr->value().GetProcess();
                if (!process.IsValid()) {
                    co_return make_error_result("No valid process");
                }
                auto current_state = process.GetState();
                if (current_state == lldb::eStateExited || current_state == lldb::eStateDetached) {
                    co_return make_error_result("Process has already " +
                                                state_to_string(current_state));
                }
                lldb::SBEvent event;
                while (listener_ptr->GetNextEvent(event)) {
                }
                uint32_t stop0 = process.GetStopID();
                auto err = process.Continue();
                if (err.Fail()) {
                    co_return make_error_result(safe_cstr(err.GetCString(), "Continue failed"));
                }
                lldb::StateType waited_state = process.GetState();
                while (listener_ptr->WaitForEvent(30, event)) {
                    auto st = lldb::SBProcess::GetStateFromEvent(event);
                    waited_state = st;
                    if (st == lldb::eStateRunning || st == lldb::eStateStepping) {
                        continue;
                    }
                    if (st == lldb::eStateStopped && process.GetStopID() <= stop0) {
                        continue;
                    }
                    break;
                }
                nlohmann::json out;
                if (waited_state == lldb::eStateExited || waited_state == lldb::eStateDetached) {
                    out["state"] = state_to_string(waited_state);
                } else {
                    out["state"] = state_to_string(process.GetState());
                }
                co_return make_text_result(out.dump());
            } catch (const std::exception& e) {
                co_return make_error_result(e.what());
            }
        });

    nlohmann::json step_schema = {
        {"type", "object"},
        {"properties",
         {{"thread_index", {{"type", "integer"}}},
          {"type", {{"type", "string"}, {"enum", nlohmann::json::array({"over", "into", "out"})}}}}}};
    server.add_tool<StepInput, nlohmann::json>(
        "step", "Step selected thread", std::move(step_schema),
        [io_executor, target_ptr, listener_ptr](StepInput args) -> mcp::Task<nlohmann::json> {
            (void)io_executor;
            try {
                if (!target_ptr->has_value() || !target_ptr->value().IsValid()) {
                    co_return make_error_result("No valid current target");
                }
                auto process = target_ptr->value().GetProcess();
                if (!process.IsValid()) {
                    co_return make_error_result("No valid process");
                }
                auto current_state = process.GetState();
                if (current_state == lldb::eStateExited || current_state == lldb::eStateDetached) {
                    co_return make_error_result("Process has already " +
                                                state_to_string(current_state));
                }

                int idx = args.thread_index.value_or(0);
                if (idx < 0 || static_cast<unsigned>(idx) >= process.GetNumThreads()) {
                    co_return make_error_result("Invalid thread_index");
                }
                auto thread = process.GetThreadAtIndex(static_cast<unsigned>(idx));
                if (!thread.IsValid()) {
                    co_return make_error_result("Thread is not valid");
                }

                const std::string type = args.type.value_or("over");
                lldb::SBEvent event;
                while (listener_ptr->GetNextEvent(event)) {
                }
                uint32_t stop0 = process.GetStopID();
                if (type == "over") {
                    thread.StepOver(lldb::eOnlyDuringStepping);
                } else if (type == "into") {
                    thread.StepInto();
                } else if (type == "out") {
                    thread.StepOut();
                } else {
                    co_return make_error_result("Invalid step type");
                }

                while (listener_ptr->WaitForEvent(30, event)) {
                    auto st = lldb::SBProcess::GetStateFromEvent(event);
                    if (st == lldb::eStateRunning || st == lldb::eStateStepping) {
                        continue;
                    }
                    if (st == lldb::eStateStopped && process.GetStopID() <= stop0) {
                        continue;
                    }
                    break;
                }

                auto frame = thread.GetFrameAtIndex(0);
                if (!frame.IsValid()) {
                    co_return make_error_result("Top frame is not valid");
                }
                auto le = frame.GetLineEntry();

                nlohmann::json out;
                out["state"] = state_to_string(process.GetState());
                out["thread_index"] = idx;
                out["function"] = safe_cstr(frame.GetFunctionName(), "<unknown>");
                out["file"] = le.IsValid() ? safe_cstr(le.GetFileSpec().GetFilename(), "<unknown>")
                                           : std::string("<unknown>");
                out["line"] = le.IsValid() ? le.GetLine() : 0;
                co_return make_text_result(out.dump());
            } catch (const std::exception& e) {
                co_return make_error_result(e.what());
            }
        });

    nlohmann::json eval_schema = {{"type", "object"},
                                  {"properties",
                                   {{"expression", {{"type", "string"}}},
                                    {"thread_index", {{"type", "integer"}}},
                                    {"frame_index", {{"type", "integer"}}}}},
                                  {"required", nlohmann::json::array({"expression"})}};
    server.add_tool<EvalInput, nlohmann::json>(
        "evaluate", "Evaluate expression", std::move(eval_schema),
        [io_executor, target_ptr](EvalInput args) -> mcp::Task<nlohmann::json> {
            (void)io_executor;
            try {
                if (!target_ptr->has_value() || !target_ptr->value().IsValid()) {
                    co_return make_error_result("No valid current target");
                }
                auto process = target_ptr->value().GetProcess();
                if (!process.IsValid()) {
                    co_return make_error_result("No valid process");
                }

                int thread_index = args.thread_index.value_or(0);
                if (thread_index < 0 ||
                    static_cast<unsigned>(thread_index) >= process.GetNumThreads()) {
                    co_return make_error_result("Invalid thread_index");
                }
                auto thread = process.GetThreadAtIndex(static_cast<unsigned>(thread_index));
                if (!thread.IsValid()) {
                    co_return make_error_result("Thread is not valid");
                }

                int frame_index = args.frame_index.value_or(0);
                if (frame_index < 0 || static_cast<unsigned>(frame_index) >= thread.GetNumFrames()) {
                    co_return make_error_result("Invalid frame_index");
                }
                auto frame = thread.GetFrameAtIndex(static_cast<unsigned>(frame_index));
                if (!frame.IsValid()) {
                    co_return make_error_result("Frame is not valid");
                }

                auto val = frame.EvaluateExpression(args.expression.c_str());
                if (!val.IsValid()) {
                    co_return make_error_result("Evaluation returned invalid value");
                }
                auto err = val.GetError();

                nlohmann::json out;
                out["expression"] = args.expression;
                out["value"] = safe_cstr(val.GetValue(), "");
                out["type"] = safe_cstr(val.GetTypeName(), "<unknown>");
                out["summary"] = safe_cstr(val.GetSummary(), "");
                out["has_error"] = err.Fail();
                out["error"] = safe_cstr(err.GetCString(), "");
                co_return make_text_result(out.dump());
            } catch (const std::exception& e) {
                co_return make_error_result(e.what());
            }
        });

    nlohmann::json bt_schema = {
        {"type", "object"},
        {"properties", {{"thread_index", {{"type", "integer"}}}, {"count", {{"type", "integer"}}}}}};
    server.add_tool<BacktraceInput, nlohmann::json>(
        "backtrace", "Collect thread backtrace", std::move(bt_schema),
        [io_executor, target_ptr](BacktraceInput args) -> mcp::Task<nlohmann::json> {
            (void)io_executor;
            try {
                if (!target_ptr->has_value() || !target_ptr->value().IsValid()) {
                    co_return make_error_result("No valid current target");
                }
                auto process = target_ptr->value().GetProcess();
                if (!process.IsValid()) {
                    co_return make_error_result("No valid process");
                }

                int thread_index = args.thread_index.value_or(0);
                if (thread_index < 0 ||
                    static_cast<unsigned>(thread_index) >= process.GetNumThreads()) {
                    co_return make_error_result("Invalid thread_index");
                }
                auto thread = process.GetThreadAtIndex(static_cast<unsigned>(thread_index));
                if (!thread.IsValid()) {
                    co_return make_error_result("Thread is not valid");
                }

                unsigned total = thread.GetNumFrames();
                int req = args.count.value_or(20);
                unsigned max_frames =
                    req <= 0
                        ? 0
                        : (total < static_cast<unsigned>(req) ? total : static_cast<unsigned>(req));

                nlohmann::json frames = nlohmann::json::array();
                for (unsigned i = 0; i < max_frames; ++i) {
                    auto frame = thread.GetFrameAtIndex(i);
                    if (!frame.IsValid()) {
                        continue;
                    }
                    auto le = frame.GetLineEntry();
                    auto module = frame.GetModule();

                    std::string file = "<unknown>";
                    unsigned line = 0;
                    if (le.IsValid()) {
                        file = safe_cstr(le.GetFileSpec().GetFilename(), "<unknown>");
                        line = le.GetLine();
                    }

                    std::string mod = "<unknown>";
                    if (module.IsValid()) {
                        mod = safe_cstr(module.GetFileSpec().GetFilename(), "<unknown>");
                    }

                    frames.push_back({{"frame_index", i},
                                      {"function", safe_cstr(frame.GetFunctionName(), "<unknown>")},
                                      {"file", file},
                                      {"line", line},
                                      {"module", mod}});
                }

                nlohmann::json out;
                out["thread_index"] = thread_index;
                out["frames"] = std::move(frames);
                co_return make_text_result(out.dump());
            } catch (const std::exception& e) {
                co_return make_error_result(e.what());
            }
        });

    mcp::Resource targets;
    targets.uri = "lldb://targets";
    targets.name = "Targets";
    targets.description = "All debugger targets";
    targets.mimeType = "application/json";
    server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        std::move(targets),
        [debugger_ptr](mcp::ReadResourceRequestParams params) -> mcp::Task<mcp::ReadResourceResult> {
            try {
                if (!debugger_ptr->IsValid()) {
                    mcp::TextResourceContents err;
                    err.uri = params.uri;
                    err.text = "Error: Debugger is not valid";
                    err.mimeType = "text/plain";
                    mcp::ReadResourceResult res;
                    res.contents.push_back(std::move(err));
                    co_return res;
                }
                nlohmann::json rows = nlohmann::json::array();
                unsigned n = debugger_ptr->GetNumTargets();
                for (unsigned i = 0; i < n; ++i) {
                    auto t = debugger_ptr->GetTargetAtIndex(i);
                    if (!t.IsValid()) {
                        continue;
                    }
                    rows.push_back(
                        {{"index", i},
                         {"triple", safe_cstr(t.GetTriple(), "<unknown>")},
                         {"executable", safe_cstr(t.GetExecutable().GetFilename(), "<unknown>")}});
                }
                mcp::TextResourceContents c;
                c.uri = params.uri;
                c.text = rows.dump();
                c.mimeType = "application/json";
                mcp::ReadResourceResult res;
                res.contents.push_back(std::move(c));
                co_return res;
            } catch (const std::exception& e) {
                mcp::TextResourceContents err;
                err.uri = params.uri;
                err.text = std::string("Error: ") + e.what();
                err.mimeType = "text/plain";
                mcp::ReadResourceResult res;
                res.contents.push_back(std::move(err));
                co_return res;
            }
        });

    mcp::Resource threads;
    threads.uri = "lldb://threads";
    threads.name = "Threads";
    threads.description = "Threads in current process";
    threads.mimeType = "application/json";
    server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        std::move(threads),
        [target_ptr](mcp::ReadResourceRequestParams params) -> mcp::Task<mcp::ReadResourceResult> {
            try {
                if (!target_ptr->has_value() || !target_ptr->value().IsValid()) {
                    mcp::TextResourceContents err;
                    err.uri = params.uri;
                    err.text = "Error: No valid current target";
                    err.mimeType = "text/plain";
                    mcp::ReadResourceResult res;
                    res.contents.push_back(std::move(err));
                    co_return res;
                }
                auto process = target_ptr->value().GetProcess();
                if (!process.IsValid()) {
                    mcp::TextResourceContents err;
                    err.uri = params.uri;
                    err.text = "Error: No valid process";
                    err.mimeType = "text/plain";
                    mcp::ReadResourceResult res;
                    res.contents.push_back(std::move(err));
                    co_return res;
                }
                nlohmann::json rows = nlohmann::json::array();
                unsigned n = process.GetNumThreads();
                for (unsigned i = 0; i < n; ++i) {
                    auto th = process.GetThreadAtIndex(i);
                    if (!th.IsValid()) {
                        continue;
                    }
                    char stop_buf[256] = {};
                    th.GetStopDescription(stop_buf, sizeof(stop_buf));
                    rows.push_back({{"index", i},
                                    {"thread_id", th.GetThreadID()},
                                    {"name", safe_cstr(th.GetName(), "<unnamed>")},
                                    {"stop_reason", stop_reason_to_string(th.GetStopReason())},
                                    {"stop_description", safe_cstr(stop_buf, "")},
                                    {"queue", safe_cstr(th.GetQueueName(), "")}});
                }
                mcp::TextResourceContents c;
                c.uri = params.uri;
                c.text = rows.dump();
                c.mimeType = "application/json";
                mcp::ReadResourceResult res;
                res.contents.push_back(std::move(c));
                co_return res;
            } catch (const std::exception& e) {
                mcp::TextResourceContents err;
                err.uri = params.uri;
                err.text = std::string("Error: ") + e.what();
                err.mimeType = "text/plain";
                mcp::ReadResourceResult res;
                res.contents.push_back(std::move(err));
                co_return res;
            }
        });

    mcp::Resource breakpoints;
    breakpoints.uri = "lldb://breakpoints";
    breakpoints.name = "Breakpoints";
    breakpoints.description = "Breakpoints in current target";
    breakpoints.mimeType = "application/json";
    server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        std::move(breakpoints),
        [target_ptr](mcp::ReadResourceRequestParams params) -> mcp::Task<mcp::ReadResourceResult> {
            try {
                if (!target_ptr->has_value() || !target_ptr->value().IsValid()) {
                    mcp::TextResourceContents err;
                    err.uri = params.uri;
                    err.text = "Error: No valid current target";
                    err.mimeType = "text/plain";
                    mcp::ReadResourceResult res;
                    res.contents.push_back(std::move(err));
                    co_return res;
                }
                auto target = target_ptr->value();
                nlohmann::json rows = nlohmann::json::array();
                unsigned n = target.GetNumBreakpoints();
                for (unsigned i = 0; i < n; ++i) {
                    auto bp = target.GetBreakpointAtIndex(i);
                    if (!bp.IsValid()) {
                        continue;
                    }
                    std::string desc = "<unknown>";
                    if (bp.GetNumLocations() > 0) {
                        auto loc = bp.GetLocationAtIndex(0);
                        if (loc.IsValid()) {
                            auto addr = loc.GetAddress();
                            if (addr.IsValid()) {
                                auto fn = addr.GetFunction();
                                if (fn.IsValid()) {
                                    desc = safe_cstr(fn.GetName(), "<unknown>");
                                } else {
                                    auto le = addr.GetLineEntry();
                                    if (le.IsValid()) {
                                        desc = safe_cstr(le.GetFileSpec().GetFilename(), "<unknown>") +
                                               ":" + std::to_string(le.GetLine());
                                    }
                                }
                            }
                        }
                    }
                    rows.push_back({{"id", bp.GetID()},
                                    {"enabled", bp.IsEnabled()},
                                    {"hit_count", bp.GetHitCount()},
                                    {"description", desc}});
                }
                mcp::TextResourceContents c;
                c.uri = params.uri;
                c.text = rows.dump();
                c.mimeType = "application/json";
                mcp::ReadResourceResult res;
                res.contents.push_back(std::move(c));
                co_return res;
            } catch (const std::exception& e) {
                mcp::TextResourceContents err;
                err.uri = params.uri;
                err.text = std::string("Error: ") + e.what();
                err.mimeType = "text/plain";
                mcp::ReadResourceResult res;
                res.contents.push_back(std::move(err));
                co_return res;
            }
        });

    mcp::ResourceTemplate thread_tmpl;
    thread_tmpl.uriTemplate = "lldb://thread/{id}";
    thread_tmpl.name = "Thread Details";
    thread_tmpl.description = "Detailed info for a specific thread by ID";
    thread_tmpl.mimeType = "application/json";
    server.add_resource_template(std::move(thread_tmpl));

    mcp::ResourceTemplate frame_tmpl;
    frame_tmpl.uriTemplate = "lldb://frame/{thread_id}/{frame_index}";
    frame_tmpl.name = "Stack Frame";
    frame_tmpl.description = "Specific frame in a thread's call stack";
    frame_tmpl.mimeType = "application/json";
    server.add_resource_template(std::move(frame_tmpl));

    mcp::Prompt dbg_prompt;
    dbg_prompt.name = "debug_session";
    dbg_prompt.description = "Generate a debugging session prompt for the given program";

    mcp::PromptArgument prog_arg;
    prog_arg.name = "program";
    prog_arg.description = "Path to the program to debug";
    prog_arg.required = true;

    mcp::PromptArgument issue_arg;
    issue_arg.name = "issue";
    issue_arg.description = "Description of the issue to investigate (optional)";
    issue_arg.required = false;

    dbg_prompt.arguments = {prog_arg, issue_arg};
    server.add_prompt<mcp::GetPromptRequestParams, mcp::GetPromptResult>(
        std::move(dbg_prompt), [](mcp::GetPromptRequestParams params) -> mcp::GetPromptResult {
            std::string program = "<program>";
            std::string issue_text;
            if (params.arguments) {
                if (auto it = params.arguments->find("program"); it != params.arguments->end()) {
                    program = it->second;
                }
                if (auto it = params.arguments->find("issue"); it != params.arguments->end()) {
                    issue_text = "\n\nIssue to investigate: " + it->second;
                }
            }
            mcp::PromptMessage msg;
            msg.role = mcp::Role::User;
            mcp::TextContent tc;
            tc.text = "You are debugging the program: " + program + issue_text +
                      "\n\nUse the available LLDB tools to:\n"
                      "1. Launch or attach to the program\n"
                      "2. Set breakpoints at relevant locations\n"
                      "3. Step through the code and inspect variables\n"
                      "4. Use backtrace to understand the call stack\n"
                      "5. Evaluate expressions to inspect state";
            msg.content = std::move(tc);
            mcp::GetPromptResult result;
            result.description = "LLDB debugging session prompt";
            result.messages.push_back(std::move(msg));
            return result;
        });

    if (opts.transport == "stdio") {
        auto transport = std::make_unique<StdioTransport>(io_ctx.get_executor());
        boost::asio::co_spawn(
            io_ctx,
            [&, transport = std::move(transport)]() mutable -> Task<void> {
                co_await server.run(std::move(transport), io_ctx.get_executor());
            },
            boost::asio::detached);
    } else {
        std::cerr << "Listening on http://" << opts.host << ":" << std::to_string(opts.port)
                  << "/mcp\n";
        auto http_transport =
            std::make_unique<mcp::HttpServerTransport>(io_ctx.get_executor(), opts.host, opts.port);
        auto* http_ptr = http_transport.get();
        boost::asio::co_spawn(
            io_ctx,
            [&, transport = std::move(http_transport)]() mutable -> mcp::Task<void> {
                boost::asio::co_spawn(io_ctx, http_ptr->listen(), boost::asio::detached);
                co_await server.run(std::move(transport), io_ctx.get_executor());
            },
            boost::asio::detached);
    }

    io_ctx.run();
    return EXIT_SUCCESS;
}
