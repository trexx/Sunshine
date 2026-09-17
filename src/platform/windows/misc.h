/**
 * @file src/platform/windows/misc.h
 * @brief Miscellaneous declarations for Windows.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

// platform includes
#include <Windows.h>
#include <winnt.h>

namespace platf {
  /**
   * @brief Write status details to the log.
   *
   * @param prefix Text prefix used when formatting the message.
   * @param status Native status code returned by the platform API.
   */
  void print_status(const std::string_view &prefix, HRESULT status);
  /**
   * @brief Synchronize thread desktop.
   *
   * @return true when the thread desktop was synchronized successfully.
   */
  HDESK syncThreadDesktop();

  /**
   * @brief Read the current Windows high-resolution performance counter.
   *
   * @return Raw QPC tick value from `QueryPerformanceCounter`.
   */
  int64_t qpc_counter();

  /**
   * @brief Convert the difference between two QPC readings to nanoseconds.
   *
   * @param performance_counter1 Newer performance-counter reading.
   * @param performance_counter2 Older performance-counter reading.
   * @return Duration represented by the difference between two QPC values.
   */
  std::chrono::nanoseconds qpc_time_difference(int64_t performance_counter1, int64_t performance_counter2);

  /**
   * @brief Get file version information from a Windows executable or driver file.
   * @param file_path Path to the file to query.
   * @param version_str Output parameter for version string in format "major.minor.build.revision".
   * @return true if version info was successfully extracted, false otherwise.
   */
  bool getFileVersionInfo(const std::filesystem::path &file_path, std::string &version_str);

  /**
   * @brief Check whether the current process runs as the LocalSystem account.
   *
   * @return `true` if the current process has system-level privileges, `false` otherwise.
   */
  bool is_running_as_system();

  /**
   * @brief Check whether a token belongs to the local Administrators group.
   *
   * @param user_token Windows access token to inspect.
   * @return True when the inspected token has administrator privileges.
   */
  bool IsUserAdmin(HANDLE user_token);

  /**
   * @brief Build an extended startup info block for a child process.
   *
   * The returned attribute list must be released with `free_proc_thread_attr_list`.
   *
   * @param file Optional log file whose handle becomes the child's stdout/stderr.
   * @param job Optional job object the child is atomically inserted into.
   * @param ec Set on allocation failure.
   * @return A structure that contains information about how to launch the new process.
   */
  STARTUPINFOEXW create_startup_info(FILE *file, HANDLE *job, std::error_code &ec);

  /**
   * @brief Release an attribute list allocated by `create_startup_info`.
   *
   * @param list Attribute list to release.
   */
  void free_proc_thread_attr_list(LPPROC_THREAD_ATTRIBUTE_LIST list);
}  // namespace platf
