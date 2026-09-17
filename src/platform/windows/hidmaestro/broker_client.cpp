/**
 * @file src/platform/windows/hidmaestro/broker_client.cpp
 * @brief Launches and talks to the HIDMaestro broker helper process.
 */
#ifndef DOXYGEN
  #define WINVER 0x0A00
  #define _WIN32_WINNT 0x0A00
#endif

// platform includes
#include <Windows.h>

// standard includes
#include <algorithm>
#include <cstring>
#include <system_error>

// local includes
#include "broker_client.h"
#include "src/logging.h"
#include "src/platform/windows/misc.h"
#include "src/utility.h"

namespace platf::hidmaestro {
  using namespace std::literals;

  namespace {
    /**
     * @brief Time allowed for the .NET broker to start and answer `hello`.
     */
    constexpr auto CONNECT_TIMEOUT = 20000ms;

    /**
     * @brief Timeout for cheap requests.
     */
    constexpr auto SHORT_TIMEOUT = 5000ms;

    /**
     * @brief Timeout for driver installation and controller creation.
     */
    constexpr auto LONG_TIMEOUT = 90000ms;

    /**
     * @brief Build the full pipe path for a pipe name.
     *
     * @param name Pipe name.
     * @return `\\.\pipe\<name>`.
     */
    std::wstring pipe_path(const std::wstring &name) {
      return L"\\\\.\\pipe\\" + name;
    }

    /**
     * @brief Copy a string into a fixed-size NUL-terminated field.
     *
     * @param dst Destination field.
     * @param src Source string.
     */
    template<std::size_t N>
    void copy_fixed(char (&dst)[N], std::string_view src) {
      const auto n = std::min(src.size(), N - 1);
      std::memcpy(dst, src.data(), n);
      dst[n] = '\0';
    }

    /**
     * @brief Read a fixed-size NUL-terminated field.
     *
     * @param src Source field.
     * @return String contents up to the first NUL.
     */
    template<std::size_t N>
    std::string read_fixed(const char (&src)[N]) {
      return std::string(src, strnlen(src, N));
    }
  }  // namespace

  broker_client_t::broker_client_t():
      executable(default_executable_path()),
      pipe_name(L"sunshine-hidmaestro-" + std::to_wstring(GetCurrentProcessId())) {
    stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  }

  broker_client_t::~broker_client_t() {
    stop();
  }

  std::filesystem::path broker_client_t::default_executable_path() {
    wchar_t module_path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, module_path, MAX_PATH) == 0) {
      return {};
    }
    return std::filesystem::path(module_path).parent_path() / "tools" / "sunshine-hidmaestro-broker.exe";
  }

  bool broker_client_t::executable_available() {
    std::error_code ec;
    return std::filesystem::is_regular_file(default_executable_path(), ec);
  }

  void broker_client_t::set_exit_callback(std::function<void()> callback) {
    std::lock_guard lock {mutex};
    exit_callback = std::move(callback);
  }

  std::optional<proto::hello_response_t> broker_client_t::last_hello() const {
    std::lock_guard lock {mutex};
    return hello_response;
  }

  bool broker_client_t::running() const {
    std::lock_guard lock {mutex};
    return process && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT;
  }

  bool broker_client_t::launch() {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(executable, ec)) {
      BOOST_LOG(warning) << "HIDMaestro broker executable not found at "sv << executable.string();
      return false;
    }

    job.reset(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
      BOOST_LOG(error) << "Couldn't create job object for HIDMaestro broker ["sv << util::hex(GetLastError()).to_string_view() << ']';
      return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
      BOOST_LOG(warning) << "Couldn't configure job object for HIDMaestro broker ["sv << util::hex(GetLastError()).to_string_view() << ']';
    }

    // Route the broker's log lines into our own stderr (the service log file or the console)
    FILE *log_file = nullptr;
    if (HANDLE err = GetStdHandle(STD_ERROR_HANDLE); err && err != INVALID_HANDLE_VALUE) {
      log_file = stderr;
    }

    HANDLE job_handle = job.get();
    auto startup_info = create_startup_info(log_file, &job_handle, ec);
    if (ec) {
      BOOST_LOG(error) << "Couldn't create startup info for HIDMaestro broker: "sv << ec.message();
      return false;
    }
    auto attr_guard = util::fail_guard([&startup_info]() {
      free_proc_thread_attr_list(startup_info.lpAttributeList);
    });

    std::wstring command = L"\"" + executable.wstring() + L"\" --pipe " + pipe_name + L" --parent-pid " + std::to_wstring(GetCurrentProcessId());

    PROCESS_INFORMATION process_info = {};
    const BOOL created = CreateProcessW(
      executable.c_str(),
      command.data(),
      nullptr,
      nullptr,
      !!(startup_info.StartupInfo.dwFlags & STARTF_USESTDHANDLES),
      CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
      nullptr,
      nullptr,
      (LPSTARTUPINFOW) &startup_info,
      &process_info
    );
    if (!created) {
      BOOST_LOG(error) << "Couldn't start HIDMaestro broker ["sv << util::hex(GetLastError()).to_string_view() << ']';
      job.reset();
      return false;
    }

    CloseHandle(process_info.hThread);
    process.reset(process_info.hProcess);
    process_id = process_info.dwProcessId;
    expected_exit = false;
    BOOST_LOG(info) << "Started HIDMaestro broker (pid "sv << process_id << ')';
    return true;
  }

  bool broker_client_t::start() {
    std::lock_guard lock {mutex};
    if (process && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT) {
      return true;
    }

    process.reset();
    job.reset();
    hello_response.reset();
    ResetEvent(stop_event.get());

    if (!launch()) {
      return false;
    }

    proto::hello_request_t request {
      .header = proto::make_header<proto::hello_request_t>(proto::request_type_e::hello),
      .protocol_version = proto::PROTOCOL_VERSION,
    };
    proto::hello_response_t response = {};
    if (!transact(&request, sizeof(request), &response, sizeof(response), CONNECT_TIMEOUT)) {
      BOOST_LOG(error) << "HIDMaestro broker did not answer the handshake"sv;
      expected_exit = true;
      TerminateProcess(process.get(), 1);
      process.reset();
      job.reset();
      return false;
    }
    if (response.header.status != static_cast<std::uint32_t>(proto::status_e::ok)) {
      BOOST_LOG(error) << "HIDMaestro broker rejected the handshake (status "sv << response.header.status << ')';
      expected_exit = true;
      TerminateProcess(process.get(), 1);
      process.reset();
      job.reset();
      return false;
    }

    hello_response = response;
    BOOST_LOG(info) << "HIDMaestro broker ready: SDK "sv << read_fixed(response.sdk_version)
                    << ", driver "sv << (response.driver_installed ? "installed"sv : "not installed"sv)
                    << ", "sv << (response.elevated ? "elevated"sv : "not elevated"sv);

    supervisor = std::jthread([this](std::stop_token token) {
      supervise(token);
    });
    return true;
  }

  void broker_client_t::stop() {
    std::jthread thread;
    {
      std::lock_guard lock {mutex};
      expected_exit = true;
      SetEvent(stop_event.get());
      thread = std::move(supervisor);
    }
    if (thread.joinable()) {
      thread.request_stop();
      thread.join();
    }

    std::lock_guard lock {mutex};
    if (process) {
      if (WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT) {
        proto::shutdown_request_t request {
          .header = proto::make_header<proto::shutdown_request_t>(proto::request_type_e::shutdown),
        };
        proto::simple_response_t response = {};
        transact(&request, sizeof(request), &response, sizeof(response), SHORT_TIMEOUT);
        if (WaitForSingleObject(process.get(), 5000) != WAIT_OBJECT_0) {
          BOOST_LOG(warning) << "HIDMaestro broker did not exit on request; terminating"sv;
          TerminateProcess(process.get(), 1);
        }
      }
      process.reset();
    }
    job.reset();
    process_id = 0;
    hello_response.reset();
  }

  void broker_client_t::supervise(std::stop_token token) {
    HANDLE handles[] = {process.get(), stop_event.get()};
    const DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (result != WAIT_OBJECT_0 || token.stop_requested()) {
      return;
    }

    bool expected;
    std::function<void()> callback;
    {
      std::lock_guard lock {mutex};
      expected = expected_exit;
      callback = exit_callback;
    }
    if (expected) {
      return;
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(process.get(), &exit_code);
    BOOST_LOG(warning) << "HIDMaestro broker exited unexpectedly (code "sv << exit_code << ')';
    if (callback) {
      callback();
    }
  }

  unique_handle_t broker_client_t::connect(std::chrono::milliseconds timeout) {
    const auto path = pipe_path(pipe_name);
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (true) {
      unique_handle_t pipe {CreateFileW(path.c_str(), GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)};
      if (pipe.get() != INVALID_HANDLE_VALUE) {
        ULONG server_pid = 0;
        if (GetNamedPipeServerProcessId(pipe.get(), &server_pid) && server_pid != process_id) {
          BOOST_LOG(error) << "HIDMaestro broker pipe is owned by an unexpected process (pid "sv << server_pid << ')';
          return {};
        }
        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr);
        return pipe;
      }
      pipe.release();  // INVALID_HANDLE_VALUE must not be closed

      const DWORD last_error = GetLastError();
      if (last_error == ERROR_PIPE_BUSY) {
        WaitNamedPipeW(path.c_str(), 50);
      } else if (last_error == ERROR_FILE_NOT_FOUND) {
        if (!process || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) {
          BOOST_LOG(warning) << "HIDMaestro broker exited before its pipe appeared"sv;
          return {};
        }
        Sleep(25);
      } else {
        BOOST_LOG(error) << "Couldn't open HIDMaestro broker pipe ["sv << util::hex(last_error).to_string_view() << ']';
        return {};
      }

      if (std::chrono::steady_clock::now() >= deadline) {
        BOOST_LOG(warning) << "Timed out connecting to the HIDMaestro broker pipe"sv;
        return {};
      }
    }
  }

  bool broker_client_t::transact(const void *request, std::size_t request_size, void *response, std::size_t response_size, std::chrono::milliseconds timeout) {
    auto pipe = connect(timeout);
    if (!pipe) {
      return false;
    }

    unique_handle_t event {CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event) {
      return false;
    }
    OVERLAPPED overlapped = {};
    overlapped.hEvent = event.get();

    DWORD bytes = 0;
    if (!TransactNamedPipe(pipe.get(), const_cast<void *>(request), static_cast<DWORD>(request_size), response, static_cast<DWORD>(response_size), &bytes, &overlapped)) {
      const DWORD last_error = GetLastError();
      if (last_error != ERROR_IO_PENDING) {
        BOOST_LOG(warning) << "HIDMaestro broker transaction failed ["sv << util::hex(last_error).to_string_view() << ']';
        return false;
      }
      if (WaitForSingleObject(event.get(), static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
        CancelIoEx(pipe.get(), &overlapped);
        GetOverlappedResult(pipe.get(), &overlapped, &bytes, TRUE);
        BOOST_LOG(warning) << "HIDMaestro broker transaction timed out"sv;
        return false;
      }
      if (!GetOverlappedResult(pipe.get(), &overlapped, &bytes, FALSE)) {
        BOOST_LOG(warning) << "HIDMaestro broker transaction failed ["sv << util::hex(GetLastError()).to_string_view() << ']';
        return false;
      }
    }

    if (bytes < sizeof(proto::response_header_t)) {
      BOOST_LOG(warning) << "HIDMaestro broker returned a truncated response ("sv << bytes << " bytes)"sv;
      return false;
    }
    const auto *header = static_cast<const proto::response_header_t *>(response);
    if (header->version != proto::PROTOCOL_VERSION) {
      BOOST_LOG(error) << "HIDMaestro broker protocol version mismatch (broker "sv << header->version << ", ours "sv << proto::PROTOCOL_VERSION << ')';
      return false;
    }
    if (bytes != response_size || header->size != bytes) {
      BOOST_LOG(warning) << "HIDMaestro broker returned an unexpected response (status "sv << header->status << ", "sv << bytes << " bytes)"sv;
      return false;
    }
    return true;
  }

  proto::status_e broker_client_t::ensure_driver(std::string &error) {
    std::lock_guard lock {mutex};
    proto::ensure_driver_request_t request {
      .header = proto::make_header<proto::ensure_driver_request_t>(proto::request_type_e::ensure_driver),
    };
    proto::ensure_driver_response_t response = {};
    if (!transact(&request, sizeof(request), &response, sizeof(response), LONG_TIMEOUT)) {
      error = "HIDMaestro broker unreachable";
      return proto::status_e::internal_error;
    }
    error = read_fixed(response.error);
    return static_cast<proto::status_e>(response.header.status);
  }

  proto::status_e broker_client_t::create_controller(std::uint32_t index, std::string_view profile_id, std::string_view identity_key, proto::packing_plan_t &plan, std::string &error) {
    std::lock_guard lock {mutex};
    proto::create_controller_request_t request = {};
    request.header = proto::make_header<proto::create_controller_request_t>(proto::request_type_e::create_controller);
    request.index = index;
    copy_fixed(request.profile_id, profile_id);
    copy_fixed(request.identity_key, identity_key);

    proto::create_controller_response_t response = {};
    if (!transact(&request, sizeof(request), &response, sizeof(response), LONG_TIMEOUT)) {
      error = "HIDMaestro broker unreachable";
      return proto::status_e::internal_error;
    }
    error = read_fixed(response.error);
    if (response.header.status == static_cast<std::uint32_t>(proto::status_e::ok)) {
      plan = response.plan;
    }
    return static_cast<proto::status_e>(response.header.status);
  }

  proto::status_e broker_client_t::destroy_controller(std::uint32_t index) {
    std::lock_guard lock {mutex};
    proto::destroy_controller_request_t request {
      .header = proto::make_header<proto::destroy_controller_request_t>(proto::request_type_e::destroy_controller),
      .index = index,
    };
    proto::simple_response_t response = {};
    if (!transact(&request, sizeof(request), &response, sizeof(response), SHORT_TIMEOUT)) {
      return proto::status_e::internal_error;
    }
    return static_cast<proto::status_e>(response.header.status);
  }

  std::optional<proto::status_response_t> broker_client_t::status() {
    std::lock_guard lock {mutex};
    proto::status_request_t request {
      .header = proto::make_header<proto::status_request_t>(proto::request_type_e::status),
    };
    proto::status_response_t response = {};
    if (!transact(&request, sizeof(request), &response, sizeof(response), SHORT_TIMEOUT)) {
      return std::nullopt;
    }
    return response;
  }

}  // namespace platf::hidmaestro
