/**
 * @file src/platform/windows/hidmaestro/broker_client.h
 * @brief Launches and talks to the HIDMaestro broker helper process.
 */
#pragma once

// platform includes
#include <Windows.h>

// standard includes
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

// local includes
#include "src/platform/hidmaestro/protocol.h"

namespace platf::hidmaestro {

  /**
   * @brief Deleter that closes a Win32 handle.
   */
  struct handle_deleter_t {
    /**
     * @brief Close the handle when it is valid.
     *
     * @param handle Handle to close.
     */
    void operator()(HANDLE handle) const {
      if (handle && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
      }
    }
  };

  /**
   * @brief Owning Win32 handle.
   */
  using unique_handle_t = std::unique_ptr<void, handle_deleter_t>;

  /**
   * @brief Owns the broker child process and performs pipe transactions with it.
   *
   * The broker is the only place the HIDMaestro .NET SDK runs. It is started as a child of
   * Sunshine with Sunshine's own token, placed in a kill-on-close job object, and watched by
   * a supervisor thread that reports unexpected exits through `set_exit_callback`.
   */
  class broker_client_t {
  public:
    broker_client_t();
    ~broker_client_t();

    broker_client_t(const broker_client_t &) = delete;
    broker_client_t &operator=(const broker_client_t &) = delete;

    /**
     * @brief Path of the broker executable next to Sunshine (`tools/sunshine-hidmaestro-broker.exe`).
     *
     * @return Absolute executable path.
     */
    static std::filesystem::path default_executable_path();

    /**
     * @brief Check whether the broker executable exists on disk.
     *
     * @return `true` when the executable is present.
     */
    static bool executable_available();

    /**
     * @brief Launch the broker and complete the `hello` handshake.
     *
     * @return `true` when the broker is running and speaks our protocol version.
     */
    bool start();

    /**
     * @brief Ask the broker to shut down, then terminate it if it does not comply.
     */
    void stop();

    /**
     * @brief Check whether the broker process is alive.
     *
     * @return `true` while the child process has not exited.
     */
    bool running() const;

    /**
     * @brief Register a callback invoked from the supervisor thread when the broker exits unexpectedly.
     *
     * @param callback Callback to invoke.
     */
    void set_exit_callback(std::function<void()> callback);

    /**
     * @brief Result of the last successful `hello` handshake.
     *
     * @return Hello response, if any.
     */
    std::optional<proto::hello_response_t> last_hello() const;

    /**
     * @brief Install the HIDMaestro driver if it is missing.
     *
     * @param error Receives a human-readable failure description.
     * @return Broker status code.
     */
    proto::status_e ensure_driver(std::string &error);

    /**
     * @brief Create a virtual controller pinned to a slot index.
     *
     * @param index HIDMaestro controller index (== Sunshine slot).
     * @param profile_id HIDMaestro profile id.
     * @param identity_key Stable identity key for device paths.
     * @param plan Receives the packing plan on success.
     * @param error Receives a human-readable failure description.
     * @return Broker status code.
     */
    proto::status_e create_controller(std::uint32_t index, std::string_view profile_id, std::string_view identity_key, proto::packing_plan_t &plan, std::string &error);

    /**
     * @brief Dispose the virtual controller at a slot index.
     *
     * @param index HIDMaestro controller index.
     * @return Broker status code.
     */
    proto::status_e destroy_controller(std::uint32_t index);

    /**
     * @brief Query driver and controller status.
     *
     * @return Status response, if the broker answered.
     */
    std::optional<proto::status_response_t> status();

  private:
    /**
     * @brief Launch the child process.
     *
     * @return `true` when the process was created.
     */
    bool launch();

    /**
     * @brief Connect to the broker pipe, retrying while the broker starts up.
     *
     * @param timeout Overall connect timeout.
     * @return Connected pipe handle, or empty on failure.
     */
    unique_handle_t connect(std::chrono::milliseconds timeout);

    /**
     * @brief Perform one request/response transaction on a fresh pipe connection.
     *
     * @param request Request bytes.
     * @param request_size Request size in bytes.
     * @param response Response buffer.
     * @param response_size Expected response size in bytes.
     * @param timeout Transaction timeout.
     * @return `true` when a full, well-formed response arrived.
     */
    bool transact(const void *request, std::size_t request_size, void *response, std::size_t response_size, std::chrono::milliseconds timeout);

    /**
     * @brief Typed wrapper around `transact`.
     *
     * @tparam Request Request structure type.
     * @tparam Response Response structure type.
     * @param request Request.
     * @param response Response.
     * @param timeout Transaction timeout.
     * @return `true` when a full, well-formed response arrived.
     */
    template<typename Request, typename Response>
    bool call(const Request &request, Response &response, std::chrono::milliseconds timeout) {
      return transact(&request, sizeof(request), &response, sizeof(response), timeout);
    }

    /**
     * @brief Supervisor thread body: waits for the child to exit.
     *
     * @param token Stop token.
     */
    void supervise(std::stop_token token);

    std::filesystem::path executable;  ///< Broker executable path.
    std::wstring pipe_name;  ///< Pipe name without the `\\.\pipe\` prefix.
    unique_handle_t process;  ///< Child process handle.
    unique_handle_t job;  ///< Kill-on-close job object holding the child.
    DWORD process_id = 0;  ///< Child process id.
    unique_handle_t stop_event;  ///< Signaled to stop the supervisor thread.
    std::jthread supervisor;  ///< Supervisor thread.
    std::function<void()> exit_callback;  ///< Unexpected-exit callback.
    std::optional<proto::hello_response_t> hello_response;  ///< Last hello response.
    bool expected_exit = false;  ///< Set while `stop()` is tearing the broker down.
    mutable std::mutex mutex;  ///< Guards process state and transactions.
  };

}  // namespace platf::hidmaestro
