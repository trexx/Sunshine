/**
 * @file src/platform/windows/hidmaestro/input.h
 * @brief HIDMaestro virtual gamepad backend for Windows.
 *
 * Controller lifecycle goes through the broker helper process (`broker_client_t`); the
 * input hot path and rumble feedback use the driver's shared memory directly
 * (`shm::input_channel_t`, `shm::output_channel_t`).
 */
#pragma once

// platform includes
#include <Windows.h>

// standard includes
#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// local includes
#include "src/platform/common.h"
#include "src/platform/hidmaestro/protocol.h"
#include "src/platform/hidmaestro/report.h"
#include "src/platform/windows/hidmaestro/broker_client.h"
#include "src/platform/windows/hidmaestro/shm.h"
#include "src/thread_pool.h"

namespace platf::hidmaestro {

  /**
   * @brief How a Sunshine gamepad kind maps onto a HIDMaestro profile.
   */
  struct profile_mapping_t {
    std::string_view kind;  ///< Sunshine config value (`x360`, `ds5`, ...).
    std::string_view profile_id;  ///< HIDMaestro profile id.
    report::output_kind_e output_kind;  ///< Output protocol used for feedback decoding.
    bool supports_motion;  ///< Whether the profile carries motion sensors.
    bool supports_touch;  ///< Whether the profile carries a touchpad.
  };

  /**
   * @brief Look up the HIDMaestro mapping for a Sunshine gamepad kind.
   *
   * @param kind Sunshine gamepad kind.
   * @return Mapping, or `nullptr` when the kind is unknown.
   */
  const profile_mapping_t *mapping_for_kind(std::string_view kind);

  /**
   * @brief Choose the Sunshine gamepad kind for a client controller.
   *
   * Honors the configured `gamepad` value, otherwise applies the same automatic rules
   * as the libvirtualhid backend.
   *
   * @param metadata Client-reported controller metadata.
   * @return Sunshine gamepad kind.
   */
  std::string_view select_kind(const gamepad_arrival_t &metadata);

  /**
   * @brief All gamepad kinds the HIDMaestro backend can emulate, in config order.
   *
   * @return Kind names.
   */
  std::vector<std::string_view> supported_kinds();

  /**
   * @brief Per-slot state of one virtual controller.
   */
  struct slot_t {
    bool active = false;  ///< Whether a controller exists in this slot.
    const profile_mapping_t *mapping = nullptr;  ///< Profile mapping in use.
    proto::packing_plan_t plan {};  ///< Packing plan from the broker.
    shm::input_channel_t input;  ///< Input shared-memory writer.
    shm::output_channel_t output;  ///< Output ring reader.

    gamepad_id_t id {};  ///< Sunshine gamepad identifiers.
    std::uint8_t client_relative_index = 0;  ///< Index used when talking back to the client.
    feedback_queue_t feedback_queue;  ///< Client feedback queue.
    std::mutex feedback_mutex;  ///< Guards `feedback_queue` across rebinds.

    report::state_t state {};  ///< Last gamepad state from the client.
    report::ds4_report_t ds4 {};  ///< DualShock 4 report cache.
    report::dualsense_report_t dualsense {};  ///< DualSense report cache.
    report::switch_pro_state_t switch_imu {};  ///< Switch Pro IMU cache.
    report::touch_state_t touch {};  ///< Touchpad pointer bookkeeping.
    std::array<std::uint8_t, report::BODY_CAPACITY> body {};  ///< Packed report body.
    std::array<std::uint8_t, report::GIP_SIZE> gip {};  ///< Packed XUSB companion buffer.

    std::chrono::steady_clock::time_point last_report_ts {};  ///< Time of the last published frame.
    thread_pool_util::ThreadPool::task_id_t keepalive_task {};  ///< Pending keepalive task, if any.

    std::optional<report::feedback_t> last_rumble;  ///< Last rumble sent to the client.
    std::optional<report::feedback_t> last_trigger_rumble;  ///< Last trigger rumble sent to the client.
    std::optional<report::feedback_t> last_rgb_led;  ///< Last RGB LED sent to the client.
    std::optional<report::feedback_t> last_player_leds;  ///< Last player LEDs sent to the client.
  };

  /**
   * @brief HIDMaestro backend: broker lifecycle plus direct shared-memory input/output.
   */
  class hidmaestro_t {
  public:
    hidmaestro_t();
    ~hidmaestro_t();

    hidmaestro_t(const hidmaestro_t &) = delete;
    hidmaestro_t &operator=(const hidmaestro_t &) = delete;

    /**
     * @brief Check whether this process can drive HIDMaestro at all.
     *
     * Requires a 64-bit build, the broker executable next to Sunshine, and SYSTEM or
     * administrator rights (the driver's shared sections are writable only by those).
     *
     * @return `true` when the backend may be initialized.
     */
    static bool platform_supported();

    /**
     * @brief Machine-readable reason why `platform_supported()` is false.
     *
     * @return Locale key, or empty when supported.
     */
    static std::string platform_unsupported_reason();

    /**
     * @brief Start the broker and complete the handshake.
     *
     * @return 0 on success, otherwise -1.
     */
    int init();

    /**
     * @brief Check whether the backend is ready to allocate gamepads.
     *
     * @return `true` when the broker is running.
     */
    bool available() const;

    /**
     * @brief Allocate a virtual controller for a client gamepad.
     *
     * @param id Sunshine gamepad identifiers.
     * @param metadata Client-reported controller metadata.
     * @param feedback_queue Queue used to return feedback to the client.
     * @return 0 on success, otherwise -1.
     */
    int alloc_gamepad(const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue);

    /**
     * @brief Rebind an existing controller's feedback to a resumed client session.
     *
     * @param id Sunshine gamepad identifiers for the resumed client.
     * @param feedback_queue Queue used to return feedback to the resumed client.
     * @return 0 when the gamepad exists and was rebound; otherwise -1.
     */
    int rebind_gamepad(const gamepad_id_t &id, feedback_queue_t feedback_queue);

    /**
     * @brief Check whether a slot holds a controller.
     *
     * @param nr Gamepad slot index.
     * @return `true` when active.
     */
    bool has_gamepad(int nr) const;

    /**
     * @brief Dispose the controller in a slot.
     *
     * @param nr Gamepad slot index.
     */
    void free_gamepad(int nr);

    /**
     * @brief Publish a full gamepad state.
     *
     * @param nr Gamepad slot index.
     * @param state Sunshine gamepad state.
     */
    void gamepad_update(int nr, const gamepad_state_t &state);

    /**
     * @brief Publish a touchpad event.
     *
     * @param touch Sunshine touch event.
     */
    void gamepad_touch(const gamepad_touch_t &touch);

    /**
     * @brief Publish a motion sample.
     *
     * @param motion Sunshine motion event.
     */
    void gamepad_motion(const gamepad_motion_t &motion);

    /**
     * @brief Publish battery metadata.
     *
     * @param battery Sunshine battery event.
     */
    void gamepad_battery(const gamepad_battery_t &battery);

    /**
     * @brief Gamepad choices offered by this backend and their availability.
     *
     * @return Supported gamepad list.
     */
    std::vector<supported_gamepad_t> supported_gamepads() const;

    /**
     * @brief Re-publish a Sony/Switch slot so timestamps and counters keep advancing.
     *
     * Scheduled on `task_pool`; public so the pool can call it.
     *
     * @param nr Gamepad slot index.
     * @param generation Slot generation the task was scheduled for.
     */
    void keepalive(int nr, std::uint64_t generation);

    /**
     * @brief Relaunch the broker after an unexpected exit and recreate live controllers.
     *
     * Scheduled on `task_pool`; public so the pool can call it.
     */
    void restart_broker();

  private:
    /**
     * @brief Pack the slot's cached state and write it to shared memory.
     *
     * Caller holds `mutex`.
     *
     * @param nr Gamepad slot index.
     */
    void submit(int nr);

    /**
     * @brief Ask the broker for a controller and open its shared memory.
     *
     * Caller holds `mutex`.
     *
     * @param nr Gamepad slot index.
     * @return `true` on success.
     */
    bool create_slot(int nr);

    /**
     * @brief Release a slot's shared memory and ask the broker to dispose the controller.
     *
     * Caller holds `mutex`.
     *
     * @param nr Gamepad slot index.
     * @param destroy_remote Whether to send `destroy_controller` to the broker.
     */
    void release_slot(int nr, bool destroy_remote);

    /**
     * @brief Ensure the driver is installed (once per broker lifetime).
     *
     * Caller holds `mutex`.
     *
     * @return `true` when the driver is ready.
     */
    bool ensure_driver();

    /**
     * @brief Raise decoded feedback to the client, dropping duplicates.
     *
     * @param slot Slot that produced the feedback.
     * @param feedback Decoded feedback.
     */
    void raise_feedback(slot_t &slot, const report::feedback_t &feedback);

    /**
     * @brief Output reader thread body.
     *
     * @param token Stop token.
     */
    void output_loop(std::stop_token token);

    /**
     * @brief Called from the broker supervisor thread when the broker dies.
     */
    void on_broker_exit();

    std::unique_ptr<broker_client_t> broker;  ///< Broker process client.
    std::array<slot_t, MAX_GAMEPADS> slots;  ///< Controller slots.
    std::uint64_t generation = 0;  ///< Incremented whenever a slot is created or released.
    bool driver_ready = false;  ///< Whether `ensure_driver` succeeded for the current broker.
    bool restart_pending = false;  ///< Whether a broker restart is scheduled.
    int restart_attempts = 0;  ///< Consecutive failed broker restarts.
    thread_pool_util::ThreadPool::task_id_t restart_task {};  ///< Pending restart task, if any.
    mutable std::recursive_mutex mutex;  ///< Guards slots and broker state.
    unique_handle_t wake_event;  ///< Wakes the output thread when the slot set changes.
    unique_handle_t stop_event;  ///< Stops the output thread.
    std::jthread output_thread;  ///< Output ring reader.
  };

}  // namespace platf::hidmaestro
