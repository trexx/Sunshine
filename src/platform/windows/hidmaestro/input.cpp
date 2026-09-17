/**
 * @file src/platform/windows/hidmaestro/input.cpp
 * @brief HIDMaestro virtual gamepad backend for Windows.
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
#include <ranges>

// local includes
#include "input.h"
#include "src/config.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/virtualhid_input.h"
#include "src/platform/windows/misc.h"
#include "src/utility.h"

namespace platf::hidmaestro {
  using namespace std::literals;

  namespace {
    // The platform-neutral packer mirrors Sunshine's Moonlight constants; keep them in sync.
    static_assert(report::button::dpad_up == DPAD_UP);
    static_assert(report::button::dpad_down == DPAD_DOWN);
    static_assert(report::button::dpad_left == DPAD_LEFT);
    static_assert(report::button::dpad_right == DPAD_RIGHT);
    static_assert(report::button::start == START);
    static_assert(report::button::back == BACK);
    static_assert(report::button::left_stick == LEFT_STICK);
    static_assert(report::button::right_stick == RIGHT_STICK);
    static_assert(report::button::left_button == LEFT_BUTTON);
    static_assert(report::button::right_button == RIGHT_BUTTON);
    static_assert(report::button::home == HOME);
    static_assert(report::button::a == A);
    static_assert(report::button::b == B);
    static_assert(report::button::x == X);
    static_assert(report::button::y == Y);
    static_assert(report::button::paddle1 == PADDLE1);
    static_assert(report::button::paddle2 == PADDLE2);
    static_assert(report::button::paddle3 == PADDLE3);
    static_assert(report::button::paddle4 == PADDLE4);
    static_assert(report::button::touchpad == TOUCHPAD_BUTTON);
    static_assert(report::button::misc == MISC_BUTTON);
    static_assert(report::motion::accel == LI_MOTION_TYPE_ACCEL);
    static_assert(report::motion::gyro == LI_MOTION_TYPE_GYRO);
    static_assert(report::touch_event::down == LI_TOUCH_EVENT_DOWN);
    static_assert(report::touch_event::up == LI_TOUCH_EVENT_UP);
    static_assert(report::touch_event::move == LI_TOUCH_EVENT_MOVE);
    static_assert(report::touch_event::cancel == LI_TOUCH_EVENT_CANCEL);
    static_assert(report::touch_event::cancel_all == LI_TOUCH_EVENT_CANCEL_ALL);
    static_assert(report::battery_state::charging == LI_BATTERY_STATE_CHARGING);
    static_assert(report::battery_state::discharging == LI_BATTERY_STATE_DISCHARGING);
    static_assert(report::battery_state::full == LI_BATTERY_STATE_FULL);
    static_assert(report::battery_state::not_present == LI_BATTERY_STATE_NOT_PRESENT);
    static_assert(report::battery_state::not_charging == LI_BATTERY_STATE_NOT_CHARGING);
    static_assert(report::battery_state::percentage_unknown == LI_BATTERY_PERCENTAGE_UNKNOWN);
    static_assert(shm::input_layout::GIP_SIZE == report::GIP_SIZE);
    static_assert(shm::input_layout::DATA_CAPACITY == report::BODY_CAPACITY);

    /**
     * @brief Sunshine gamepad kinds and the HIDMaestro profiles they map to.
     */
    constexpr std::array mappings {
      profile_mapping_t {"generic"sv, "generic-dinput-gamepad"sv, report::output_kind_e::none, false, false},
      profile_mapping_t {"x360"sv, "xbox-360-wired"sv, report::output_kind_e::xbox_360, false, false},
      profile_mapping_t {"xone"sv, "xbox-one-s"sv, report::output_kind_e::xbox_one_hid, false, false},
      profile_mapping_t {"xseries"sv, "xbox-series-xs"sv, report::output_kind_e::xbox_one_hid, false, false},
      profile_mapping_t {"ds4"sv, "dualshock-4-v2"sv, report::output_kind_e::ds4, true, true},
      profile_mapping_t {"ds5"sv, "dualsense"sv, report::output_kind_e::dualsense, true, true},
      profile_mapping_t {"switch"sv, "switch-pro"sv, report::output_kind_e::switch_pro, true, false},
    };

    /**
     * @brief How long the output thread sleeps between ring polls when no doorbell rings.
     */
    constexpr DWORD OUTPUT_POLL_TIMEOUT_MS = 8;

    /**
     * @brief Interval at which Sony reports are re-published so their timestamps advance.
     */
    constexpr auto KEEPALIVE_INTERVAL = 100ms;

    /**
     * @brief Initial delay before relaunching a crashed broker.
     */
    constexpr auto RESTART_BASE_DELAY = 1000ms;

    /**
     * @brief Give up relaunching the broker after this many consecutive failures.
     */
    constexpr int MAX_RESTART_ATTEMPTS = 5;

    /**
     * @brief Sensor scaling matching the neutral calibration blob HIDMaestro serves for Sony pads.
     */
    constexpr report::motion_scale_t SONY_MOTION_SCALE {.accel_per_g = 10000.0F, .gyro_per_dps = 20.0F};

    /**
     * @brief Convert Sunshine gamepad state into the packer's mirror type.
     *
     * @param state Sunshine gamepad state.
     * @return Packer state.
     */
    report::state_t to_state(const gamepad_state_t &state) {
      return report::state_t {
        .button_flags = state.buttonFlags,
        .lt = state.lt,
        .rt = state.rt,
        .ls_x = state.lsX,
        .ls_y = state.lsY,
        .rs_x = state.rsX,
        .rs_y = state.rsY,
      };
    }

    /**
     * @brief Sony button options derived from configuration.
     *
     * @return Packer options.
     */
    report::options_t sony_options() {
      return report::options_t {
        .back_as_touchpad_click = config::input.ds4_back_as_touchpad_click && virtualhid::configured_gamepad_supports_touchpad(),
      };
    }

    /**
     * @brief Name of a broker status code for logging.
     *
     * @param status Broker status.
     * @return Human-readable name.
     */
    std::string_view status_name(proto::status_e status) {
      switch (status) {
        case proto::status_e::ok:
          return "ok"sv;
        case proto::status_e::invalid_request:
          return "invalid request"sv;
        case proto::status_e::unsupported_version:
          return "unsupported protocol version"sv;
        case proto::status_e::unknown_profile:
          return "unknown profile"sv;
        case proto::status_e::driver_install_failed:
          return "driver install failed"sv;
        case proto::status_e::controller_create_failed:
          return "controller create failed"sv;
        case proto::status_e::index_in_use:
          return "index in use"sv;
        case proto::status_e::not_found:
          return "not found"sv;
        case proto::status_e::not_elevated:
          return "not elevated"sv;
        case proto::status_e::internal_error:
          return "internal error"sv;
      }
      return "unknown"sv;
    }
  }  // namespace

  const profile_mapping_t *mapping_for_kind(std::string_view kind) {
    const auto iter = std::ranges::find(mappings, kind, &profile_mapping_t::kind);
    return iter == mappings.end() ? nullptr : &*iter;
  }

  std::vector<std::string_view> supported_kinds() {
    std::vector<std::string_view> kinds;
    kinds.reserve(mappings.size());
    for (const auto &mapping : mappings) {
      kinds.push_back(mapping.kind);
    }
    return kinds;
  }

  std::string_view select_kind(const gamepad_arrival_t &metadata) {
    if (config::input.gamepad != "auto"sv) {
      if (mapping_for_kind(config::input.gamepad)) {
        return config::input.gamepad;
      }
      BOOST_LOG(warning) << "Gamepad type '"sv << config::input.gamepad << "' is not supported by HIDMaestro; using Xbox Series"sv;
      return "xseries"sv;
    }

    if (metadata.type == LI_CTYPE_PS) {
      BOOST_LOG(info) << "Gamepad will be DualSense controller (auto-selected by client-reported type)"sv;
      return "ds5"sv;
    }
    if (metadata.type == LI_CTYPE_NINTENDO) {
      BOOST_LOG(info) << "Gamepad will be Nintendo Switch Pro controller (auto-selected by client-reported type)"sv;
      return "switch"sv;
    }
    if (metadata.type == LI_CTYPE_XBOX) {
      BOOST_LOG(info) << "Gamepad will be Xbox Series controller (auto-selected by client-reported type)"sv;
      return "xseries"sv;
    }
    if (config::input.motion_as_ds4 && (metadata.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO))) {
      BOOST_LOG(info) << "Gamepad will be DualSense controller (auto-selected by motion sensor presence)"sv;
      return "ds5"sv;
    }
    if (config::input.touchpad_as_ds4 && (metadata.capabilities & LI_CCAP_TOUCHPAD)) {
      BOOST_LOG(info) << "Gamepad will be DualSense controller (auto-selected by touchpad presence)"sv;
      return "ds5"sv;
    }

    BOOST_LOG(info) << "Gamepad will be Xbox Series controller (default)"sv;
    return "xseries"sv;
  }

  hidmaestro_t::hidmaestro_t():
      broker(std::make_unique<broker_client_t>()) {
    wake_event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  }

  hidmaestro_t::~hidmaestro_t() {
    if (stop_event) {
      SetEvent(stop_event.get());
    }
    if (output_thread.joinable()) {
      output_thread.request_stop();
      output_thread.join();
    }

    {
      std::lock_guard lock {mutex};
      for (int nr = 0; nr < static_cast<int>(slots.size()); ++nr) {
        if (slots[nr].active) {
          // The broker disposes every controller on shutdown; only tear down our side
          release_slot(nr, false);
        }
      }
    }

    broker->set_exit_callback({});
    broker->stop();

    std::lock_guard lock {mutex};
    if (restart_task) {
      task_pool.cancel(restart_task);
      restart_task = nullptr;
    }
  }

  bool hidmaestro_t::platform_supported() {
    return platform_unsupported_reason().empty();
  }

  std::string hidmaestro_t::platform_unsupported_reason() {
#if !(defined(_M_X64) || defined(__x86_64__))
    return "gamepads.hidmaestro-unsupported-arch";
#else
    if (!broker_client_t::executable_available()) {
      return "gamepads.hidmaestro-not-installed";
    }
    if (is_running_as_system()) {
      return {};
    }
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
      return "gamepads.hidmaestro-requires-elevation";
    }
    const bool admin = IsUserAdmin(token);
    CloseHandle(token);
    return admin ? std::string {} : "gamepads.hidmaestro-requires-elevation";
#endif
  }

  int hidmaestro_t::init() {
    if (const auto reason = platform_unsupported_reason(); !reason.empty()) {
      BOOST_LOG(info) << "HIDMaestro gamepad backend unavailable: "sv << reason;
      return -1;
    }

    std::lock_guard lock {mutex};
    if (!broker->start()) {
      BOOST_LOG(warning) << "HIDMaestro broker could not be started; HIDMaestro gamepad support is unavailable"sv;
      return -1;
    }

    broker->set_exit_callback([this]() {
      on_broker_exit();
    });
    output_thread = std::jthread([this](std::stop_token token) {
      output_loop(token);
    });
    return 0;
  }

  bool hidmaestro_t::available() const {
    return broker && broker->running();
  }

  bool hidmaestro_t::ensure_driver() {
    if (driver_ready) {
      return true;
    }
    std::string error;
    const auto status = broker->ensure_driver(error);
    if (status == proto::status_e::ok) {
      driver_ready = true;
      return true;
    }
    BOOST_LOG(error) << "HIDMaestro driver is not available ("sv << status_name(status) << "): "sv << error;
    return false;
  }

  bool hidmaestro_t::create_slot(int nr) {
    auto &slot = slots[nr];
    if (!ensure_driver()) {
      return false;
    }

    const std::string identity = "sunshine-slot" + std::to_string(nr);
    std::string error;
    auto status = broker->create_controller(static_cast<std::uint32_t>(nr), slot.mapping->profile_id, identity, slot.plan, error);
    if (status == proto::status_e::index_in_use) {
      // A previous Sunshine instance left this slot behind in the broker; reclaim it
      broker->destroy_controller(static_cast<std::uint32_t>(nr));
      status = broker->create_controller(static_cast<std::uint32_t>(nr), slot.mapping->profile_id, identity, slot.plan, error);
    }
    if (status != proto::status_e::ok) {
      BOOST_LOG(error) << "Couldn't create HIDMaestro controller "sv << nr << " ("sv << slot.mapping->profile_id << "): "sv << status_name(status) << (error.empty() ? ""sv : ": "sv) << error;
      return false;
    }

    if (slot.plan.data_size == 0 || slot.plan.data_size > report::BODY_CAPACITY) {
      BOOST_LOG(error) << "HIDMaestro controller "sv << nr << " reported an invalid body size of "sv << slot.plan.data_size;
      broker->destroy_controller(static_cast<std::uint32_t>(nr));
      return false;
    }

    if (!slot.input.open(static_cast<std::uint32_t>(nr), slot.plan.packs_gip != 0)) {
      broker->destroy_controller(static_cast<std::uint32_t>(nr));
      return false;
    }
    slot.output.open(static_cast<std::uint32_t>(nr));
    slot.last_report_ts = std::chrono::steady_clock::now();
    return true;
  }

  void hidmaestro_t::release_slot(int nr, bool destroy_remote) {
    auto &slot = slots[nr];
    if (slot.keepalive_task) {
      task_pool.cancel(slot.keepalive_task);
      slot.keepalive_task = nullptr;
    }
    slot.input.close();
    slot.output.close();
    if (destroy_remote && broker->running()) {
      const auto status = broker->destroy_controller(static_cast<std::uint32_t>(nr));
      if (status != proto::status_e::ok && status != proto::status_e::not_found) {
        BOOST_LOG(warning) << "Couldn't destroy HIDMaestro controller "sv << nr << ": "sv << status_name(status);
      }
    }
    slot.active = false;
    slot.mapping = nullptr;
    generation++;
    SetEvent(wake_event.get());
  }

  int hidmaestro_t::alloc_gamepad(const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue) {
    std::lock_guard lock {mutex};
    const int nr = id.globalIndex;
    if (nr < 0 || nr >= static_cast<int>(slots.size())) {
      return -1;
    }

    if (!broker->running() && !broker->start()) {
      BOOST_LOG(warning) << "HIDMaestro broker is not running; cannot allocate gamepad "sv << nr;
      return -1;
    }

    auto &slot = slots[nr];
    if (slot.active) {
      BOOST_LOG(warning) << "HIDMaestro gamepad "sv << nr << " is already allocated; replacing it"sv;
      release_slot(nr, true);
    }

    const auto kind = select_kind(metadata);
    const auto *mapping = mapping_for_kind(kind);
    if (!mapping) {
      return -1;
    }

    slot.mapping = mapping;
    slot.id = id;
    slot.client_relative_index = id.clientRelativeIndex;
    {
      std::lock_guard feedback_lock {slot.feedback_mutex};
      slot.feedback_queue = std::move(feedback_queue);
    }
    slot.state = {};
    report::ds4_init(slot.ds4);
    report::dualsense_init(slot.dualsense);
    slot.switch_imu = {};
    slot.touch = {};
    slot.last_rumble.reset();
    slot.last_trigger_rumble.reset();
    slot.last_rgb_led.reset();
    slot.last_player_leds.reset();

    if (!create_slot(nr)) {
      slot.mapping = nullptr;
      return -1;
    }

    slot.active = true;
    generation++;
    SetEvent(wake_event.get());
    BOOST_LOG(info) << "Gamepad "sv << nr << " will be HIDMaestro '"sv << mapping->profile_id << "' ("sv << kind << ')';

    if (!mapping->supports_motion && (metadata.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO))) {
      BOOST_LOG(warning) << "Gamepad "sv << nr << " has motion sensors, but they are not usable when emulating "sv << mapping->profile_id;
    }
    if (!mapping->supports_touch && (metadata.capabilities & LI_CCAP_TOUCHPAD)) {
      BOOST_LOG(warning) << "Gamepad "sv << nr << " has a touchpad, but it is not usable when emulating "sv << mapping->profile_id;
    }

    if (mapping->supports_motion) {
      std::lock_guard feedback_lock {slot.feedback_mutex};
      if (slot.feedback_queue) {
        slot.feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(slot.client_relative_index, LI_MOTION_TYPE_ACCEL, 100));
        slot.feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(slot.client_relative_index, LI_MOTION_TYPE_GYRO, 100));
      }
    }

    // Publish an idle frame so the device is live before the first client packet arrives
    submit(nr);
    return 0;
  }

  int hidmaestro_t::rebind_gamepad(const gamepad_id_t &id, feedback_queue_t feedback_queue) {
    std::lock_guard lock {mutex};
    const int nr = id.globalIndex;
    if (nr < 0 || nr >= static_cast<int>(slots.size()) || !slots[nr].active) {
      return -1;
    }

    auto &slot = slots[nr];
    {
      std::lock_guard feedback_lock {slot.feedback_mutex};
      slot.feedback_queue = std::move(feedback_queue);
      slot.id = id;
      slot.client_relative_index = id.clientRelativeIndex;
      slot.last_rumble.reset();
      slot.last_trigger_rumble.reset();
      slot.last_rgb_led.reset();
      slot.last_player_leds.reset();

      if (slot.mapping && slot.mapping->supports_motion && slot.feedback_queue) {
        slot.feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(slot.client_relative_index, LI_MOTION_TYPE_ACCEL, 100));
        slot.feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(slot.client_relative_index, LI_MOTION_TYPE_GYRO, 100));
      }
    }
    return 0;
  }

  bool hidmaestro_t::has_gamepad(int nr) const {
    std::lock_guard lock {mutex};
    return nr >= 0 && nr < static_cast<int>(slots.size()) && slots[nr].active;
  }

  void hidmaestro_t::free_gamepad(int nr) {
    std::lock_guard lock {mutex};
    if (nr < 0 || nr >= static_cast<int>(slots.size()) || !slots[nr].active) {
      return;
    }
    release_slot(nr, true);
  }

  void hidmaestro_t::submit(int nr) {
    auto &slot = slots[nr];
    if (!slot.active && !slot.mapping) {
      return;
    }
    if (!slot.input.is_open()) {
      return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - slot.last_report_ts);
    slot.last_report_ts = now;

    bool sony = false;
    switch (static_cast<proto::pack_mode_e>(slot.plan.mode)) {
      case proto::pack_mode_e::sony_ds4:
        sony = true;
        report::ds4_update_state(slot.ds4, slot.state, sony_options());
        report::ds4_advance(slot.ds4, elapsed);
        slot.input.write_frame(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(&slot.ds4), sizeof(slot.ds4)), nullptr);
        break;
      case proto::pack_mode_e::sony_dualsense:
        sony = true;
        report::dualsense_update_state(slot.dualsense, slot.state, sony_options());
        report::dualsense_advance(slot.dualsense, elapsed);
        slot.input.write_frame(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(&slot.dualsense), sizeof(slot.dualsense)), nullptr);
        break;
      case proto::pack_mode_e::switch_pro:
        report::pack_switch_pro_body(slot.state, slot.switch_imu, std::span<std::uint8_t, report::SWITCH_PRO_BODY_SIZE>(slot.body.data(), report::SWITCH_PRO_BODY_SIZE));
        slot.input.write_frame(std::span<const std::uint8_t>(slot.body.data(), report::SWITCH_PRO_BODY_SIZE), nullptr);
        break;
      case proto::pack_mode_e::hid_generic:
      default:
        {
          const auto size = report::pack_generic(slot.plan, slot.state, slot.body);
          const std::uint8_t *gip = nullptr;
          if (slot.plan.packs_gip) {
            report::pack_gip(slot.state, slot.plan.y_axis_hid_down != 0, std::span<std::uint8_t, report::GIP_SIZE>(slot.gip));
            gip = slot.gip.data();
          }
          slot.input.write_frame(std::span<const std::uint8_t>(slot.body.data(), size), gip);
          break;
        }
    }

    if (sony) {
      // Real Sony pads stream continuously; keep timestamps and counters moving while idle
      if (slot.keepalive_task) {
        task_pool.cancel(slot.keepalive_task);
      }
      slot.keepalive_task = task_pool.pushDelayed(&hidmaestro_t::keepalive, KEEPALIVE_INTERVAL, this, nr, generation).task_id;
    }
  }

  void hidmaestro_t::keepalive(int nr, std::uint64_t scheduled_generation) {
    std::lock_guard lock {mutex};
    if (nr < 0 || nr >= static_cast<int>(slots.size())) {
      return;
    }
    auto &slot = slots[nr];
    slot.keepalive_task = nullptr;
    if (!slot.active || scheduled_generation != generation) {
      return;
    }
    submit(nr);
  }

  void hidmaestro_t::gamepad_update(int nr, const gamepad_state_t &state) {
    std::lock_guard lock {mutex};
    if (nr < 0 || nr >= static_cast<int>(slots.size()) || !slots[nr].active) {
      return;
    }
    slots[nr].state = to_state(state);
    submit(nr);
  }

  void hidmaestro_t::gamepad_touch(const gamepad_touch_t &touch) {
    std::lock_guard lock {mutex};
    const int nr = touch.id.globalIndex;
    if (nr < 0 || nr >= static_cast<int>(slots.size()) || !slots[nr].active) {
      return;
    }
    auto &slot = slots[nr];
    if (!slot.mapping->supports_touch) {
      return;
    }

    const report::touch_t event {
      .event_type = touch.eventType,
      .pointer_id = touch.pointerId,
      .x = touch.x,
      .y = touch.y,
    };
    bool changed = false;
    switch (static_cast<proto::pack_mode_e>(slot.plan.mode)) {
      case proto::pack_mode_e::sony_ds4:
        changed = report::ds4_update_touch(slot.ds4, slot.touch, event);
        break;
      case proto::pack_mode_e::sony_dualsense:
        changed = report::dualsense_update_touch(slot.dualsense, slot.touch, event);
        break;
      default:
        break;
    }
    if (changed) {
      submit(nr);
    }
  }

  void hidmaestro_t::gamepad_motion(const gamepad_motion_t &motion) {
    std::lock_guard lock {mutex};
    const int nr = motion.id.globalIndex;
    if (nr < 0 || nr >= static_cast<int>(slots.size()) || !slots[nr].active) {
      return;
    }
    auto &slot = slots[nr];
    if (!slot.mapping->supports_motion) {
      return;
    }

    switch (static_cast<proto::pack_mode_e>(slot.plan.mode)) {
      case proto::pack_mode_e::sony_ds4:
        report::ds4_update_motion(slot.ds4, motion.motionType, motion.x, motion.y, motion.z, SONY_MOTION_SCALE);
        break;
      case proto::pack_mode_e::sony_dualsense:
        report::dualsense_update_motion(slot.dualsense, motion.motionType, motion.x, motion.y, motion.z, SONY_MOTION_SCALE);
        break;
      case proto::pack_mode_e::switch_pro:
        report::switch_pro_update_motion(slot.switch_imu, motion.motionType, motion.x, motion.y, motion.z);
        break;
      default:
        return;
    }
    submit(nr);
  }

  void hidmaestro_t::gamepad_battery(const gamepad_battery_t &battery) {
    std::lock_guard lock {mutex};
    const int nr = battery.id.globalIndex;
    if (nr < 0 || nr >= static_cast<int>(slots.size()) || !slots[nr].active) {
      return;
    }
    auto &slot = slots[nr];

    switch (static_cast<proto::pack_mode_e>(slot.plan.mode)) {
      case proto::pack_mode_e::sony_ds4:
        report::ds4_update_battery(slot.ds4, battery.state, battery.percentage);
        break;
      case proto::pack_mode_e::sony_dualsense:
        report::dualsense_update_battery(slot.dualsense, battery.state, battery.percentage);
        break;
      default:
        return;
    }
    submit(nr);
  }

  std::vector<supported_gamepad_t> hidmaestro_t::supported_gamepads() const {
    const bool enabled = available();
    const std::string reason = enabled ? "" : "gamepads.hidmaestro-not-available";
    std::vector<supported_gamepad_t> gamepads;
    gamepads.reserve(mappings.size() + 1);
    gamepads.push_back({"auto", enabled, reason});
    for (const auto &mapping : mappings) {
      gamepads.push_back({std::string(mapping.kind), enabled, reason});
    }
    return gamepads;
  }

  void hidmaestro_t::raise_feedback(slot_t &slot, const report::feedback_t &feedback) {
    std::lock_guard feedback_lock {slot.feedback_mutex};
    if (!slot.feedback_queue) {
      return;
    }

    const auto index = slot.client_relative_index;
    switch (feedback.kind) {
      case report::feedback_t::kind_e::rumble:
        if (slot.last_rumble && slot.last_rumble->lowfreq == feedback.lowfreq && slot.last_rumble->highfreq == feedback.highfreq) {
          return;
        }
        slot.last_rumble = feedback;
        slot.feedback_queue->raise(gamepad_feedback_msg_t::make_rumble(index, feedback.lowfreq, feedback.highfreq));
        break;
      case report::feedback_t::kind_e::rumble_triggers:
        if (slot.last_trigger_rumble && slot.last_trigger_rumble->left_trigger == feedback.left_trigger && slot.last_trigger_rumble->right_trigger == feedback.right_trigger) {
          return;
        }
        slot.last_trigger_rumble = feedback;
        slot.feedback_queue->raise(gamepad_feedback_msg_t::make_rumble_triggers(index, feedback.left_trigger, feedback.right_trigger));
        break;
      case report::feedback_t::kind_e::rgb_led:
        if (slot.last_rgb_led && slot.last_rgb_led->r == feedback.r && slot.last_rgb_led->g == feedback.g && slot.last_rgb_led->b == feedback.b) {
          return;
        }
        slot.last_rgb_led = feedback;
        slot.feedback_queue->raise(gamepad_feedback_msg_t::make_rgb_led(index, feedback.r, feedback.g, feedback.b));
        break;
      case report::feedback_t::kind_e::player_leds:
        if (slot.last_player_leds && slot.last_player_leds->solid == feedback.solid && slot.last_player_leds->flashing == feedback.flashing) {
          return;
        }
        slot.last_player_leds = feedback;
        slot.feedback_queue->raise(gamepad_feedback_msg_t::make_player_leds(index, feedback.solid, feedback.flashing));
        break;
      case report::feedback_t::kind_e::adaptive_triggers:
        slot.feedback_queue->raise(gamepad_feedback_msg_t::make_adaptive_triggers(index, feedback.event_flags, feedback.type_left, feedback.type_right, feedback.left, feedback.right));
        break;
    }
  }

  void hidmaestro_t::output_loop(std::stop_token token) {
    std::vector<HANDLE> handles;
    while (!token.stop_requested()) {
      handles.clear();
      handles.push_back(stop_event.get());
      handles.push_back(wake_event.get());
      {
        std::lock_guard lock {mutex};
        for (auto &slot : slots) {
          if (slot.active && slot.output.event()) {
            handles.push_back(slot.output.event());
          }
        }
      }

      // The doorbell is auto-reset and shared with the SDK's own reader inside the broker,
      // so a short timeout bounds the delay when the other waiter consumes the signal.
      const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, OUTPUT_POLL_TIMEOUT_MS);
      if (result == WAIT_OBJECT_0 || token.stop_requested()) {
        break;
      }

      std::lock_guard lock {mutex};
      for (auto &slot : slots) {
        if (!slot.active || !slot.output.is_open() || !slot.mapping) {
          continue;
        }
        const auto kind = slot.mapping->output_kind;
        slot.output.poll([&](const shm::output_packet_t &packet) {
          for (const auto &feedback : report::decode_output(kind, packet.source, packet.report_id, packet.data)) {
            raise_feedback(slot, feedback);
          }
        });
      }
    }
  }

  void hidmaestro_t::on_broker_exit() {
    std::lock_guard lock {mutex};
    driver_ready = false;
    if (restart_pending) {
      return;
    }
    restart_pending = true;
    const auto delay = RESTART_BASE_DELAY * (1 << std::min(restart_attempts, 4));
    BOOST_LOG(info) << "Relaunching HIDMaestro broker in "sv << delay.count() << " ms"sv;
    restart_task = task_pool.pushDelayed(&hidmaestro_t::restart_broker, delay, this).task_id;
  }

  void hidmaestro_t::restart_broker() {
    std::lock_guard lock {mutex};
    restart_pending = false;
    restart_task = nullptr;
    driver_ready = false;

    broker->stop();
    if (!broker->start()) {
      restart_attempts++;
      if (restart_attempts < MAX_RESTART_ATTEMPTS) {
        restart_pending = true;
        const auto delay = RESTART_BASE_DELAY * (1 << std::min(restart_attempts, 4));
        BOOST_LOG(warning) << "HIDMaestro broker relaunch failed; retrying in "sv << delay.count() << " ms"sv;
        restart_task = task_pool.pushDelayed(&hidmaestro_t::restart_broker, delay, this).task_id;
      } else {
        BOOST_LOG(error) << "HIDMaestro broker relaunch failed "sv << restart_attempts << " times; giving up until the next gamepad arrives"sv;
      }
      return;
    }
    restart_attempts = 0;

    // The broker's fresh SDK context swept the orphaned devices; recreate every live slot
    for (int nr = 0; nr < static_cast<int>(slots.size()); ++nr) {
      auto &slot = slots[nr];
      if (!slot.active) {
        continue;
      }
      slot.input.close();
      slot.output.close();
      if (create_slot(nr)) {
        BOOST_LOG(info) << "Recreated HIDMaestro controller "sv << nr << " after broker restart"sv;
        submit(nr);
      } else {
        BOOST_LOG(error) << "Couldn't recreate HIDMaestro controller "sv << nr << " after broker restart"sv;
        release_slot(nr, false);
      }
    }
    generation++;
    SetEvent(wake_event.get());
  }

}  // namespace platf::hidmaestro
