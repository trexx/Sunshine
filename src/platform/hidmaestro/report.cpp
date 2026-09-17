/**
 * @file src/platform/hidmaestro/report.cpp
 * @brief Platform-neutral input report packing and output report decoding for HIDMaestro.
 */
// standard includes
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

// local includes
#include "src/platform/hidmaestro/report.h"

namespace platf::hidmaestro::report {

  namespace {
    /**
     * @brief Standard gravity used to convert Moonlight m/s² into g.
     */
    constexpr float EARTH_G = 9.80665F;

    /**
     * @brief Clamp and round a float into an int16.
     *
     * @param value Value to convert.
     * @return Rounded value clamped to the int16 range.
     */
    std::int16_t to_int16(float value) {
      const auto rounded = std::lround(value);
      return static_cast<std::int16_t>(std::clamp<long>(rounded, std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()));
    }

    /**
     * @brief Expand an 8-bit intensity to 16 bits (255 → 65535).
     *
     * @param value 8-bit intensity.
     * @return 16-bit intensity.
     */
    constexpr std::uint16_t expand8(std::uint8_t value) {
      return static_cast<std::uint16_t>(value * 257U);
    }

    /**
     * @brief Expand a 0..100 percentage to 16 bits.
     *
     * @param value Percentage; values above 100 are clamped.
     * @return 16-bit intensity.
     */
    constexpr std::uint16_t expand_percent(std::uint8_t value) {
      const std::uint32_t clamped = std::min<std::uint32_t>(value, 100U);
      return static_cast<std::uint16_t>(clamped * 65535U / 100U);
    }

    /**
     * @brief Convert Moonlight motion units into raw sensor counts.
     *
     * @param motion_type `motion::accel` or `motion::gyro`.
     * @param value Component in m/s² or deg/s.
     * @param scale Sensor scaling.
     * @return Raw sensor count.
     */
    std::int16_t motion_raw(std::uint8_t motion_type, float value, const motion_scale_t &scale) {
      if (motion_type == motion::accel) {
        return to_int16((value / EARTH_G) * scale.accel_per_g);
      }
      return to_int16(value * scale.gyro_per_dps);
    }

    /**
     * @brief Convert a signed stick value to an unsigned 8-bit axis (0 = negative end).
     *
     * @param value Stick value.
     * @return 0..255.
     */
    constexpr std::uint8_t stick_to_u8(std::int16_t value) {
      return static_cast<std::uint8_t>((static_cast<std::int32_t>(value) + 32768) / 257);
    }

    /**
     * @brief Convert a signed stick value to an unsigned 8-bit axis with 0 at the top.
     *
     * @param value Stick value, up positive.
     * @return 0..255 with 0 meaning fully up.
     */
    constexpr std::uint8_t stick_to_u8_down(std::int16_t value) {
      return static_cast<std::uint8_t>(255 - stick_to_u8(value));
    }

    /**
     * @brief Pack a 12-bit X/Y pair into three bytes (Sony touch and Switch stick format).
     *
     * @param dst Three destination bytes.
     * @param x 12-bit X.
     * @param y 12-bit Y.
     */
    void pack_12bit_pair(std::uint8_t *dst, std::uint16_t x, std::uint16_t y) {
      dst[0] = static_cast<std::uint8_t>(x & 0xFF);
      dst[1] = static_cast<std::uint8_t>(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
      dst[2] = static_cast<std::uint8_t>(y >> 4);
    }

    /**
     * @brief Shared touch slot allocation for the two-finger Sony touchpads.
     *
     * @param touch_state Pointer bookkeeping.
     * @param touch Touch event.
     * @param[out] finger Assigned finger slot (0 or 1).
     * @param[out] down Whether the finger is down after this event.
     * @param[out] cancel_all Whether every finger was lifted.
     * @return `true` when the event should be applied to the report.
     */
    bool resolve_touch_slot(touch_state_t &touch_state, const touch_t &touch, std::uint8_t &finger, bool &down, bool &cancel_all) {
      cancel_all = false;
      down = false;
      finger = 0;

      if (touch.event_type == touch_event::down) {
        if (touch_state.available_pointers & 0x1) {
          finger = 0;
        } else if (touch_state.available_pointers & 0x2) {
          finger = 1;
        } else {
          return false;
        }
        touch_state.pointer_id_map[touch.pointer_id] = finger;
        touch_state.available_pointers &= static_cast<std::uint8_t>(~(1U << finger));
        down = true;
        return true;
      }

      if (touch.event_type == touch_event::cancel_all) {
        touch_state.pointer_id_map.clear();
        touch_state.available_pointers = 0x3;
        cancel_all = true;
        return true;
      }

      const auto iter = touch_state.pointer_id_map.find(touch.pointer_id);
      if (iter == touch_state.pointer_id_map.end()) {
        return false;
      }
      finger = iter->second;

      if (touch.event_type == touch_event::up || touch.event_type == touch_event::cancel) {
        touch_state.pointer_id_map.erase(iter);
        touch_state.available_pointers |= static_cast<std::uint8_t>(1U << finger);
        down = false;
        return true;
      }

      if (touch.event_type == touch_event::move) {
        down = true;
        return true;
      }

      return false;
    }

    /**
     * @brief Decode a Switch HD rumble block into an 8-bit amplitude (port of the SDK decoder).
     *
     * @param block Four rumble bytes.
     * @return Amplitude 0..255.
     */
    std::uint8_t switch_rumble_amplitude(std::span<const std::uint8_t> block) {
      if (block.size() < 4) {
        return 0;
      }
      const int hf = block[1] & 0xFE;
      const int hf_norm = std::clamp(hf * 255 / 0xC8, 0, 255);
      const int msb = (block[2] & 0x80) != 0 ? 1 : 0;
      const int lo = block[3];
      int lf_norm = 0;
      if (lo >= 0x40 && lo <= 0x72) {
        const int index = (lo - 0x40) * 2 + msb;
        lf_norm = std::clamp(index * 255 / 101, 0, 255);
      }
      return static_cast<std::uint8_t>(std::max(hf_norm, lf_norm));
    }
  }  // namespace

  int hat_direction(std::uint32_t button_flags) {
    if (button_flags & button::dpad_up) {
      if (button_flags & button::dpad_right) {
        return 1;
      }
      if (button_flags & button::dpad_left) {
        return 7;
      }
      return 0;
    }
    if (button_flags & button::dpad_down) {
      if (button_flags & button::dpad_right) {
        return 3;
      }
      if (button_flags & button::dpad_left) {
        return 5;
      }
      return 4;
    }
    if (button_flags & button::dpad_right) {
      return 2;
    }
    if (button_flags & button::dpad_left) {
      return 6;
    }
    return -1;
  }

  void write_bits(std::span<std::uint8_t> body, std::uint32_t bit_offset, std::uint32_t bit_size, std::uint32_t value) {
    for (std::uint32_t i = 0; i < bit_size; ++i) {
      const std::uint32_t bit = bit_offset + i;
      const std::size_t byte = bit / 8;
      if (byte >= body.size()) {
        return;
      }
      const std::uint8_t mask = static_cast<std::uint8_t>(1U << (bit % 8));
      if ((value >> i) & 1U) {
        body[byte] |= mask;
      } else {
        body[byte] &= static_cast<std::uint8_t>(~mask);
      }
    }
  }

  std::uint32_t read_bits(std::span<const std::uint8_t> body, std::uint32_t bit_offset, std::uint32_t bit_size) {
    std::uint32_t value = 0;
    for (std::uint32_t i = 0; i < bit_size; ++i) {
      const std::uint32_t bit = bit_offset + i;
      const std::size_t byte = bit / 8;
      if (byte >= body.size()) {
        break;
      }
      if (body[byte] & (1U << (bit % 8))) {
        value |= (1U << i);
      }
    }
    return value;
  }

  std::int32_t scale_axis(std::int16_t value, const proto::field_t &field, bool invert) {
    const std::int64_t range = static_cast<std::int64_t>(field.logical_max) - field.logical_min;
    const std::int64_t forward = field.logical_min + ((static_cast<std::int64_t>(value) + 32768) * range + 32767) / 65535;
    if (!invert) {
      return static_cast<std::int32_t>(forward);
    }
    // Mirror within the logical range so full deflection lands exactly on the opposite bound
    return static_cast<std::int32_t>(field.logical_min + field.logical_max - forward);
  }

  std::int32_t scale_trigger(std::uint8_t value, const proto::field_t &field) {
    const std::int64_t range = static_cast<std::int64_t>(field.logical_max) - field.logical_min;
    return static_cast<std::int32_t>(field.logical_min + (static_cast<std::int64_t>(value) * range + 127) / 255);
  }

  std::size_t pack_generic(const proto::packing_plan_t &plan, const state_t &state, std::span<std::uint8_t> body) {
    const std::size_t size = std::min<std::size_t>(plan.data_size, body.size());
    std::fill_n(body.begin(), size, static_cast<std::uint8_t>(0));
    auto out = body.first(size);

    const auto put = [&](const proto::field_t &field, std::int32_t value) {
      if (field.present && field.bit_size > 0) {
        write_bits(out, field.bit_offset, field.bit_size, static_cast<std::uint32_t>(value));
      }
    };

    const bool y_down = plan.y_axis_hid_down != 0;
    put(plan.lx, scale_axis(state.ls_x, plan.lx, false));
    put(plan.ly, scale_axis(state.ls_y, plan.ly, y_down));
    put(plan.rx, scale_axis(state.rs_x, plan.rx, false));
    put(plan.ry, scale_axis(state.rs_y, plan.ry, y_down));
    put(plan.lt, scale_trigger(state.lt, plan.lt));
    put(plan.rt, scale_trigger(state.rt, plan.rt));

    if (plan.combined_z.present) {
      const std::int64_t range = static_cast<std::int64_t>(plan.combined_z.logical_max) - plan.combined_z.logical_min;
      const std::int64_t combined = plan.combined_z.logical_min + ((255 + static_cast<std::int64_t>(state.rt) - state.lt) * range + 255) / 510;
      put(plan.combined_z, static_cast<std::int32_t>(combined));
    }

    if (plan.hat.present) {
      int dir = hat_direction(state.button_flags);
      std::int32_t value = plan.hat_null;
      if (dir >= 0) {
        if (plan.hat_positions == 4) {
          dir = (dir & 1) ? dir - 1 : dir;  // round diagonals to the preceding cardinal
          value = plan.hat.logical_min + dir / 2;
        } else {
          value = plan.hat.logical_min + dir;
        }
      }
      put(plan.hat, value);
    }

    const auto pressed = [&](proto::button_role_e role) -> bool {
      const auto flags = state.button_flags;
      switch (role) {
        case proto::button_role_e::a:
          return flags & button::a;
        case proto::button_role_e::b:
          return flags & button::b;
        case proto::button_role_e::x:
          return flags & button::x;
        case proto::button_role_e::y:
          return flags & button::y;
        case proto::button_role_e::left_bumper:
          return flags & button::left_button;
        case proto::button_role_e::right_bumper:
          return flags & button::right_button;
        case proto::button_role_e::back:
          return flags & button::back;
        case proto::button_role_e::start:
          return flags & button::start;
        case proto::button_role_e::left_stick:
          return flags & button::left_stick;
        case proto::button_role_e::right_stick:
          return flags & button::right_stick;
        case proto::button_role_e::guide:
          return flags & button::home;
        case proto::button_role_e::misc:
          return flags & button::misc;
        case proto::button_role_e::paddle1:
          return flags & button::paddle1;
        case proto::button_role_e::paddle2:
          return flags & button::paddle2;
        case proto::button_role_e::paddle3:
          return flags & button::paddle3;
        case proto::button_role_e::paddle4:
          return flags & button::paddle4;
      }
      return false;
    };

    for (std::size_t role = 0; role < proto::ROLE_COUNT; ++role) {
      const std::uint8_t index = plan.role_to_button[role];
      if (index == proto::ROLE_UNMAPPED || index >= plan.button_count || index >= proto::MAX_BUTTONS) {
        continue;
      }
      if (pressed(static_cast<proto::button_role_e>(role))) {
        put(plan.buttons[index], 1);
      }
    }

    return size;
  }

  void pack_gip(const state_t &state, bool y_axis_hid_down, std::span<std::uint8_t, GIP_SIZE> gip) {
    const auto axis_u16 = [](std::int16_t value) -> std::uint16_t {
      return static_cast<std::uint16_t>(static_cast<std::int32_t>(value) + 32768);
    };
    const auto axis_u16_y = [&](std::int16_t value) -> std::uint16_t {
      return y_axis_hid_down ? static_cast<std::uint16_t>(32767 - static_cast<std::int32_t>(value)) : axis_u16(value);
    };
    const auto trigger_u10 = [](std::uint8_t value) -> std::uint16_t {
      return static_cast<std::uint16_t>(static_cast<std::uint32_t>(value) * 1023U / 255U);
    };
    const auto put16 = [&](std::size_t offset, std::uint16_t value) {
      gip[offset] = static_cast<std::uint8_t>(value & 0xFF);
      gip[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    };

    put16(0, axis_u16(state.ls_x));
    put16(2, axis_u16_y(state.ls_y));
    put16(4, axis_u16(state.rs_x));
    put16(6, axis_u16_y(state.rs_y));
    put16(8, trigger_u10(state.lt));
    put16(10, trigger_u10(state.rt));

    const auto flags = state.button_flags;
    std::uint8_t low = 0;
    if (flags & button::a) {
      low |= 0x01;
    }
    if (flags & button::b) {
      low |= 0x02;
    }
    if (flags & button::x) {
      low |= 0x04;
    }
    if (flags & button::y) {
      low |= 0x08;
    }
    if (flags & button::left_button) {
      low |= 0x10;
    }
    if (flags & button::right_button) {
      low |= 0x20;
    }
    if (flags & button::left_stick) {
      low |= 0x40;
    }
    if (flags & button::right_stick) {
      low |= 0x80;
    }
    gip[12] = low;

    std::uint8_t high = 0;
    if (flags & button::back) {
      high |= 0x01;
    }
    if (flags & button::start) {
      high |= 0x02;
    }
    const int dir = hat_direction(flags);
    const std::uint8_t hat = dir < 0 ? 0 : static_cast<std::uint8_t>(dir + 1);
    high |= static_cast<std::uint8_t>((hat & 0x0F) << 2);
    if (flags & (button::home | button::misc)) {
      high |= 0x40;
    }
    gip[13] = high;
  }

  // ---------------------------------------------------------------------------
  // DualShock 4
  // ---------------------------------------------------------------------------

  namespace {
    constexpr std::uint16_t DS4_BUTTON_SQUARE = 1U << 4;
    constexpr std::uint16_t DS4_BUTTON_CROSS = 1U << 5;
    constexpr std::uint16_t DS4_BUTTON_CIRCLE = 1U << 6;
    constexpr std::uint16_t DS4_BUTTON_TRIANGLE = 1U << 7;
    constexpr std::uint16_t DS4_BUTTON_SHOULDER_LEFT = 1U << 8;
    constexpr std::uint16_t DS4_BUTTON_SHOULDER_RIGHT = 1U << 9;
    constexpr std::uint16_t DS4_BUTTON_TRIGGER_LEFT = 1U << 10;
    constexpr std::uint16_t DS4_BUTTON_TRIGGER_RIGHT = 1U << 11;
    constexpr std::uint16_t DS4_BUTTON_SHARE = 1U << 12;
    constexpr std::uint16_t DS4_BUTTON_OPTIONS = 1U << 13;
    constexpr std::uint16_t DS4_BUTTON_THUMB_LEFT = 1U << 14;
    constexpr std::uint16_t DS4_BUTTON_THUMB_RIGHT = 1U << 15;
    constexpr std::uint16_t DS4_DPAD_NONE = 0x8;
    constexpr std::uint8_t DS4_SPECIAL_PS = 0x01;
    constexpr std::uint8_t DS4_SPECIAL_TOUCHPAD = 0x02;
    constexpr std::uint16_t DS4_TOUCH_WIDTH = 1920;
    constexpr std::uint16_t DS4_TOUCH_HEIGHT = 943;

    /**
     * @brief Apply a Sony two-finger touch event to a DualShock 4 style touch packet.
     *
     * @param packet Touch packet to update.
     * @param touch_state Pointer bookkeeping.
     * @param touch Touch event.
     * @return `true` when the packet changed.
     */
    bool ds4_apply_touch(ds4_touch_t &packet, touch_state_t &touch_state, const touch_t &touch) {
      std::uint8_t finger = 0;
      bool down = false;
      bool cancel_all = false;
      if (!resolve_touch_slot(touch_state, touch, finger, down, cancel_all)) {
        return false;
      }

      packet.packet_counter++;
      if (cancel_all) {
        packet.is_up_tracking_num1 |= 0x80;
        packet.is_up_tracking_num2 |= 0x80;
        return true;
      }

      auto &tracking = finger == 0 ? packet.is_up_tracking_num1 : packet.is_up_tracking_num2;
      auto *data = finger == 0 ? packet.touch_data1 : packet.touch_data2;
      if (touch.event_type == touch_event::down) {
        tracking = static_cast<std::uint8_t>(touch_state.next_tracking_id++ & 0x7F);
      } else if (!down) {
        tracking |= 0x80;
      }

      if (down) {
        const auto x = static_cast<std::uint16_t>(std::clamp(touch.x, 0.0F, 1.0F) * DS4_TOUCH_WIDTH);
        const auto y = static_cast<std::uint16_t>(std::clamp(touch.y, 0.0F, 1.0F) * DS4_TOUCH_HEIGHT);
        pack_12bit_pair(data, x, y);
      }
      return true;
    }
  }  // namespace

  void ds4_init(ds4_report_t &report) {
    std::memset(&report, 0, sizeof(report));
    report.thumb_lx = 0x80;
    report.thumb_ly = 0x80;
    report.thumb_rx = 0x80;
    report.thumb_ry = 0x80;
    report.buttons = DS4_DPAD_NONE;
    report.battery_lvl = 0xFF;
    report.battery_lvl_special = 0x1A;  // Wired, full battery
    report.touch_packets_n = 1;
    report.current_touch.is_up_tracking_num1 = 0x80;
    report.current_touch.is_up_tracking_num2 = 0x80;
    for (auto &packet : report.previous_touch) {
      packet.is_up_tracking_num1 = 0x80;
      packet.is_up_tracking_num2 = 0x80;
    }
  }

  void ds4_update_state(ds4_report_t &report, const state_t &state, const options_t &options) {
    const auto flags = state.button_flags;
    std::uint16_t buttons = 0;
    if (flags & button::left_stick) {
      buttons |= DS4_BUTTON_THUMB_LEFT;
    }
    if (flags & button::right_stick) {
      buttons |= DS4_BUTTON_THUMB_RIGHT;
    }
    if (flags & button::left_button) {
      buttons |= DS4_BUTTON_SHOULDER_LEFT;
    }
    if (flags & button::right_button) {
      buttons |= DS4_BUTTON_SHOULDER_RIGHT;
    }
    if (flags & button::start) {
      buttons |= DS4_BUTTON_OPTIONS;
    }
    if (flags & button::back) {
      buttons |= DS4_BUTTON_SHARE;
    }
    if (flags & button::a) {
      buttons |= DS4_BUTTON_CROSS;
    }
    if (flags & button::b) {
      buttons |= DS4_BUTTON_CIRCLE;
    }
    if (flags & button::x) {
      buttons |= DS4_BUTTON_SQUARE;
    }
    if (flags & button::y) {
      buttons |= DS4_BUTTON_TRIANGLE;
    }
    if (state.lt > 0) {
      buttons |= DS4_BUTTON_TRIGGER_LEFT;
    }
    if (state.rt > 0) {
      buttons |= DS4_BUTTON_TRIGGER_RIGHT;
    }

    const int dir = hat_direction(flags);
    buttons |= static_cast<std::uint16_t>(dir < 0 ? DS4_DPAD_NONE : dir);
    report.buttons = buttons;

    std::uint8_t special = static_cast<std::uint8_t>(report.special & 0xFC);
    if (flags & button::home) {
      special |= DS4_SPECIAL_PS;
    }
    // Allow either the PS touchpad click or the Xbox Series share button to click the touchpad
    if (flags & (button::touchpad | button::misc)) {
      special |= DS4_SPECIAL_TOUCHPAD;
    }
    if (options.back_as_touchpad_click && (flags & button::back)) {
      special |= DS4_SPECIAL_TOUCHPAD;
    }
    report.special = special;

    report.trigger_l = state.lt;
    report.trigger_r = state.rt;
    report.thumb_lx = stick_to_u8(state.ls_x);
    report.thumb_ly = stick_to_u8_down(state.ls_y);
    report.thumb_rx = stick_to_u8(state.rs_x);
    report.thumb_ry = stick_to_u8_down(state.rs_y);
  }

  void ds4_update_motion(ds4_report_t &report, std::uint8_t motion_type, float x, float y, float z, const motion_scale_t &scale) {
    if (motion_type == motion::accel) {
      report.accel_x = motion_raw(motion_type, x, scale);
      report.accel_y = motion_raw(motion_type, y, scale);
      report.accel_z = motion_raw(motion_type, z, scale);
    } else if (motion_type == motion::gyro) {
      report.gyro_x = motion_raw(motion_type, x, scale);
      report.gyro_y = motion_raw(motion_type, y, scale);
      report.gyro_z = motion_raw(motion_type, z, scale);
    }
  }

  bool ds4_update_touch(ds4_report_t &report, touch_state_t &touch_state, const touch_t &touch) {
    return ds4_apply_touch(report.current_touch, touch_state, touch);
  }

  void ds4_update_battery(ds4_report_t &report, std::uint8_t state, std::uint8_t percentage) {
    // For details on the report format of these battery level fields, see:
    // https://github.com/torvalds/linux/blob/946c6b59c56dc6e7d8364a8959cb36bf6d10bc37/drivers/hid/hid-playstation.c#L2305-L2314
    switch (state) {
      case battery_state::charging:
      case battery_state::discharging:
        if (state == battery_state::charging) {
          report.battery_lvl_special |= 0x10;  // Connected via USB
        } else {
          report.battery_lvl_special &= static_cast<std::uint8_t>(~0x10);
        }
        if ((report.battery_lvl_special & 0xF) > 0xA) {
          report.battery_lvl_special = static_cast<std::uint8_t>((report.battery_lvl_special & ~0xF) | 0x5);
        }
        break;
      case battery_state::full:
        report.battery_lvl_special = 0x1B;  // USB + battery full
        report.battery_lvl = 0xFF;
        break;
      case battery_state::not_present:
      case battery_state::not_charging:
        report.battery_lvl_special = 0x1F;  // USB + charging error
        break;
      default:
        break;
    }

    if (percentage != battery_state::percentage_unknown) {
      const std::uint32_t pct = std::min<std::uint32_t>(percentage, 100U);
      report.battery_lvl = static_cast<std::uint8_t>(pct * 255U / 100U);
      if ((report.battery_lvl_special & 0x10) && (report.battery_lvl_special & 0xF) <= 0xA) {
        report.battery_lvl_special = static_cast<std::uint8_t>((report.battery_lvl_special & ~0xF) | ((pct + 5) / 10));
      }
    }
  }

  void ds4_advance(ds4_report_t &report, std::chrono::nanoseconds elapsed) {
    // Timestamp is reported in 5.333 µs units
    report.timestamp = static_cast<std::uint16_t>(report.timestamp + elapsed.count() / 5333);
    // Real hardware increments the 6-bit counter that shares a byte with the PS/touchpad bits
    const std::uint8_t counter = static_cast<std::uint8_t>(((report.special >> 2) + 1) & 0x3F);
    report.special = static_cast<std::uint8_t>((report.special & 0x03) | (counter << 2));
  }

  // ---------------------------------------------------------------------------
  // DualSense
  // ---------------------------------------------------------------------------

  namespace {
    constexpr std::uint8_t DS_BUTTONS0_SQUARE = 0x10;
    constexpr std::uint8_t DS_BUTTONS0_CROSS = 0x20;
    constexpr std::uint8_t DS_BUTTONS0_CIRCLE = 0x40;
    constexpr std::uint8_t DS_BUTTONS0_TRIANGLE = 0x80;
    constexpr std::uint8_t DS_BUTTONS1_L1 = 0x01;
    constexpr std::uint8_t DS_BUTTONS1_R1 = 0x02;
    constexpr std::uint8_t DS_BUTTONS1_L2 = 0x04;
    constexpr std::uint8_t DS_BUTTONS1_R2 = 0x08;
    constexpr std::uint8_t DS_BUTTONS1_CREATE = 0x10;
    constexpr std::uint8_t DS_BUTTONS1_OPTIONS = 0x20;
    constexpr std::uint8_t DS_BUTTONS1_L3 = 0x40;
    constexpr std::uint8_t DS_BUTTONS1_R3 = 0x80;
    constexpr std::uint8_t DS_BUTTONS2_PS = 0x01;
    constexpr std::uint8_t DS_BUTTONS2_TOUCHPAD = 0x02;
    constexpr std::uint8_t DS_BUTTONS2_MUTE = 0x04;
    constexpr std::uint8_t DS_HAT_NONE = 0x8;
    constexpr std::uint16_t DS_TOUCH_WIDTH = 1920;
    constexpr std::uint16_t DS_TOUCH_HEIGHT = 1080;
    constexpr std::uint8_t DS_STATUS_CHARGING = 0x1;
    constexpr std::uint8_t DS_STATUS_FULL = 0x2;
    constexpr std::uint8_t DS_STATUS_DISCHARGING = 0x0;
    constexpr std::uint8_t DS_STATUS_ERROR = 0xB;
  }  // namespace

  void dualsense_init(dualsense_report_t &report) {
    std::memset(&report, 0, sizeof(report));
    report.x = 0x80;
    report.y = 0x80;
    report.rx = 0x80;
    report.ry = 0x80;
    report.buttons[0] = DS_HAT_NONE;
    report.points[0].contact = 0x80;
    report.points[1].contact = 0x80;
    report.status = static_cast<std::uint8_t>((DS_STATUS_FULL << 4) | 0x0A);  // Full battery, wired
  }

  void dualsense_update_state(dualsense_report_t &report, const state_t &state, const options_t &options) {
    const auto flags = state.button_flags;

    const int dir = hat_direction(flags);
    std::uint8_t b0 = static_cast<std::uint8_t>(dir < 0 ? DS_HAT_NONE : dir);
    if (flags & button::x) {
      b0 |= DS_BUTTONS0_SQUARE;
    }
    if (flags & button::a) {
      b0 |= DS_BUTTONS0_CROSS;
    }
    if (flags & button::b) {
      b0 |= DS_BUTTONS0_CIRCLE;
    }
    if (flags & button::y) {
      b0 |= DS_BUTTONS0_TRIANGLE;
    }

    std::uint8_t b1 = 0;
    if (flags & button::left_button) {
      b1 |= DS_BUTTONS1_L1;
    }
    if (flags & button::right_button) {
      b1 |= DS_BUTTONS1_R1;
    }
    if (state.lt > 0) {
      b1 |= DS_BUTTONS1_L2;
    }
    if (state.rt > 0) {
      b1 |= DS_BUTTONS1_R2;
    }
    if (flags & button::back) {
      b1 |= DS_BUTTONS1_CREATE;
    }
    if (flags & button::start) {
      b1 |= DS_BUTTONS1_OPTIONS;
    }
    if (flags & button::left_stick) {
      b1 |= DS_BUTTONS1_L3;
    }
    if (flags & button::right_stick) {
      b1 |= DS_BUTTONS1_R3;
    }

    std::uint8_t b2 = 0;
    if (flags & button::home) {
      b2 |= DS_BUTTONS2_PS;
    }
    if (flags & button::touchpad) {
      b2 |= DS_BUTTONS2_TOUCHPAD;
    }
    if (options.back_as_touchpad_click && (flags & button::back)) {
      b2 |= DS_BUTTONS2_TOUCHPAD;
    }
    if (flags & button::misc) {
      b2 |= DS_BUTTONS2_MUTE;
    }

    report.buttons[0] = b0;
    report.buttons[1] = b1;
    report.buttons[2] = b2;
    report.buttons[3] = 0;

    report.z = state.lt;
    report.rz = state.rt;
    report.x = stick_to_u8(state.ls_x);
    report.y = stick_to_u8_down(state.ls_y);
    report.rx = stick_to_u8(state.rs_x);
    report.ry = stick_to_u8_down(state.rs_y);
  }

  void dualsense_update_motion(dualsense_report_t &report, std::uint8_t motion_type, float x, float y, float z, const motion_scale_t &scale) {
    if (motion_type == motion::accel) {
      report.accel[0] = motion_raw(motion_type, x, scale);
      report.accel[1] = motion_raw(motion_type, y, scale);
      report.accel[2] = motion_raw(motion_type, z, scale);
    } else if (motion_type == motion::gyro) {
      report.gyro[0] = motion_raw(motion_type, x, scale);
      report.gyro[1] = motion_raw(motion_type, y, scale);
      report.gyro[2] = motion_raw(motion_type, z, scale);
    }
  }

  bool dualsense_update_touch(dualsense_report_t &report, touch_state_t &touch_state, const touch_t &touch) {
    std::uint8_t finger = 0;
    bool down = false;
    bool cancel_all = false;
    if (!resolve_touch_slot(touch_state, touch, finger, down, cancel_all)) {
      return false;
    }

    if (cancel_all) {
      report.points[0].contact = 0x80;
      report.points[1].contact = 0x80;
      return true;
    }

    auto &point = report.points[finger];
    if (touch.event_type == touch_event::down) {
      point.contact = static_cast<std::uint8_t>(touch_state.next_tracking_id++ & 0x7F);
    } else if (!down) {
      point.contact |= 0x80;
    }

    if (down) {
      const auto x = static_cast<std::uint16_t>(std::clamp(touch.x, 0.0F, 1.0F) * DS_TOUCH_WIDTH);
      const auto y = static_cast<std::uint16_t>(std::clamp(touch.y, 0.0F, 1.0F) * DS_TOUCH_HEIGHT);
      pack_12bit_pair(&point.x_lo, x, y);
    }
    return true;
  }

  void dualsense_update_battery(dualsense_report_t &report, std::uint8_t state, std::uint8_t percentage) {
    std::uint8_t status = report.status;
    switch (state) {
      case battery_state::charging:
        status = static_cast<std::uint8_t>((DS_STATUS_CHARGING << 4) | (status & 0x0F));
        break;
      case battery_state::discharging:
        status = static_cast<std::uint8_t>((DS_STATUS_DISCHARGING << 4) | (status & 0x0F));
        break;
      case battery_state::full:
        status = static_cast<std::uint8_t>((DS_STATUS_FULL << 4) | 0x0A);
        break;
      case battery_state::not_present:
      case battery_state::not_charging:
        status = static_cast<std::uint8_t>((DS_STATUS_ERROR << 4) | (status & 0x0F));
        break;
      default:
        break;
    }

    if (percentage != battery_state::percentage_unknown) {
      const std::uint32_t pct = std::min<std::uint32_t>(percentage, 100U);
      status = static_cast<std::uint8_t>((status & 0xF0) | std::min<std::uint32_t>(pct / 10U, 0x0AU));
    }
    report.status = status;
  }

  void dualsense_advance(dualsense_report_t &report, std::chrono::nanoseconds elapsed) {
    report.seq_number++;
    // Sensor timestamp is reported in 0.33 µs units
    report.sensor_timestamp += static_cast<std::uint32_t>(elapsed.count() * 3 / 1000);
  }

  // ---------------------------------------------------------------------------
  // Switch Pro
  // ---------------------------------------------------------------------------

  void switch_pro_update_motion(switch_pro_state_t &imu, std::uint8_t motion_type, float x, float y, float z) {
    // SDL maps the Pro Controller wire frame to its sensor frame as
    //   sdl[0] = -wireY, sdl[1] = +wireZ, sdl[2] = -wireX
    // so the inverse packed here is wireX = -sdlZ, wireY = -sdlX, wireZ = +sdlY.
    if (motion_type == motion::accel) {
      constexpr float scale = 4096.0F;  // counts per g
      imu.accel[0] = to_int16((-z / EARTH_G) * scale);
      imu.accel[1] = to_int16((-x / EARTH_G) * scale);
      imu.accel[2] = to_int16((y / EARTH_G) * scale);
    } else if (motion_type == motion::gyro) {
      // Exact inverse of SDL's LoadIMUCalibration scale for the driver's fabricated SPI image
      constexpr float scale = 13371.0F / 936.0F;
      imu.gyro[0] = to_int16(-z * scale);
      imu.gyro[1] = to_int16(-x * scale);
      imu.gyro[2] = to_int16(y * scale);
    }
  }

  void pack_switch_pro_body(const state_t &state, const switch_pro_state_t &imu, std::span<std::uint8_t, SWITCH_PRO_BODY_SIZE> body) {
    std::fill(body.begin(), body.end(), static_cast<std::uint8_t>(0));
    const auto flags = state.button_flags;

    // byte 2 (right): bit0=Y bit1=X bit2=B bit3=A bit6=R bit7=ZR; face buttons by position
    std::uint8_t right = 0;
    if (flags & button::x) {
      right |= 0x01;  // west → Switch Y
    }
    if (flags & button::y) {
      right |= 0x02;  // north → Switch X
    }
    if (flags & button::a) {
      right |= 0x04;  // south → Switch B
    }
    if (flags & button::b) {
      right |= 0x08;  // east → Switch A
    }
    if (flags & button::right_button) {
      right |= 0x40;
    }
    if (state.rt > 0) {
      right |= 0x80;
    }

    // byte 3 (shared): bit0=Minus bit1=Plus bit2=RStick bit3=LStick bit4=Home bit5=Capture
    std::uint8_t shared = 0;
    if (flags & button::back) {
      shared |= 0x01;
    }
    if (flags & button::start) {
      shared |= 0x02;
    }
    if (flags & button::right_stick) {
      shared |= 0x04;
    }
    if (flags & button::left_stick) {
      shared |= 0x08;
    }
    if (flags & button::home) {
      shared |= 0x10;
    }
    if (flags & button::misc) {
      shared |= 0x20;
    }

    // byte 4 (left): bit0=Down bit1=Up bit2=Right bit3=Left bit6=L bit7=ZL
    std::uint8_t left = 0;
    if (flags & button::dpad_down) {
      left |= 0x01;
    }
    if (flags & button::dpad_up) {
      left |= 0x02;
    }
    if (flags & button::dpad_right) {
      left |= 0x04;
    }
    if (flags & button::dpad_left) {
      left |= 0x08;
    }
    if (flags & button::left_button) {
      left |= 0x40;
    }
    if (state.lt > 0) {
      left |= 0x80;
    }

    body[2] = right;
    body[3] = shared;
    body[4] = left;

    const auto stick_raw = [](std::int16_t value) -> std::uint16_t {
      const int raw = 0x800 + static_cast<int>(static_cast<std::int32_t>(value) * 0x600 / 32767);
      return static_cast<std::uint16_t>(std::clamp(raw, 0, 0xFFF));
    };
    pack_12bit_pair(&body[5], stick_raw(state.ls_x), stick_raw(state.ls_y));
    pack_12bit_pair(&body[8], stick_raw(state.rs_x), stick_raw(state.rs_y));

    // Three identical 5 ms IMU frames per report
    for (std::size_t frame = 0; frame < 3; ++frame) {
      const std::size_t o = 12 + frame * 12;
      const std::int16_t values[6] = {imu.accel[0], imu.accel[1], imu.accel[2], imu.gyro[0], imu.gyro[1], imu.gyro[2]};
      for (std::size_t i = 0; i < 6; ++i) {
        const auto v = static_cast<std::uint16_t>(values[i]);
        body[o + i * 2] = static_cast<std::uint8_t>(v & 0xFF);
        body[o + i * 2 + 1] = static_cast<std::uint8_t>(v >> 8);
      }
    }
  }

  // ---------------------------------------------------------------------------
  // Output decoding
  // ---------------------------------------------------------------------------

  std::vector<feedback_t> decode_output(output_kind_e kind, std::uint8_t source, std::uint8_t report_id, std::span<const std::uint8_t> data) {
    std::vector<feedback_t> result;

    switch (kind) {
      case output_kind_e::xbox_360:
        if (source == output_source::xinput) {
          feedback_t fb {.kind = feedback_t::kind_e::rumble};
          if (data.size() >= 5) {
            // xusb wire packet: cmd, size, left motor, right motor, reserved
            fb.lowfreq = expand8(data[2]);
            fb.highfreq = expand8(data[3]);
            result.push_back(fb);
          } else if (data.size() == 4) {
            // Raw XINPUT_VIBRATION: two little-endian 16-bit speeds
            fb.lowfreq = static_cast<std::uint16_t>(data[0] | (data[1] << 8));
            fb.highfreq = static_cast<std::uint16_t>(data[2] | (data[3] << 8));
            result.push_back(fb);
          }
        }
        break;

      case output_kind_e::xbox_one_hid:
        if (source == output_source::hid_output && report_id == 0x03 && data.size() >= 5) {
          // Xbox One HID rumble: enable mask, left trigger, right trigger, strong, weak (0..100)
          const std::uint8_t enable = data[0];
          if (enable & 0x0C) {
            feedback_t fb {.kind = feedback_t::kind_e::rumble};
            fb.lowfreq = (enable & 0x08) ? expand_percent(data[3]) : 0;
            fb.highfreq = (enable & 0x04) ? expand_percent(data[4]) : 0;
            result.push_back(fb);
          }
          if (enable & 0x03) {
            feedback_t fb {.kind = feedback_t::kind_e::rumble_triggers};
            fb.left_trigger = (enable & 0x02) ? expand_percent(data[1]) : 0;
            fb.right_trigger = (enable & 0x01) ? expand_percent(data[2]) : 0;
            result.push_back(fb);
          }
        }
        break;

      case output_kind_e::ds4:
        if (source == output_source::hid_output && report_id == 0x05 && data.size() >= 8) {
          const std::uint8_t flags = data[0];
          if (flags & 0x01) {
            feedback_t fb {.kind = feedback_t::kind_e::rumble};
            fb.lowfreq = expand8(data[4]);  // big motor
            fb.highfreq = expand8(data[3]);  // small motor
            result.push_back(fb);
          }
          if (flags & 0x02) {
            feedback_t fb {.kind = feedback_t::kind_e::rgb_led};
            fb.r = data[5];
            fb.g = data[6];
            fb.b = data[7];
            result.push_back(fb);
          }
        }
        break;

      case output_kind_e::dualsense:
        if (source == output_source::hid_output && report_id == 0x02 && data.size() >= 47) {
          const std::uint8_t flag0 = data[0];
          const std::uint8_t flag1 = data[1];
          if (flag0 & 0x03) {
            feedback_t fb {.kind = feedback_t::kind_e::rumble};
            fb.lowfreq = expand8(data[3]);  // left / strong motor
            fb.highfreq = expand8(data[2]);  // right / weak motor
            result.push_back(fb);
          }
          if (flag0 & 0x0C) {
            feedback_t fb {.kind = feedback_t::kind_e::adaptive_triggers};
            fb.event_flags = static_cast<std::uint8_t>(flag0 & 0x0C);
            fb.type_right = data[10];
            std::copy_n(data.begin() + 11, 10, fb.right.begin());
            fb.type_left = data[21];
            std::copy_n(data.begin() + 22, 10, fb.left.begin());
            result.push_back(fb);
          }
          if (flag1 & 0x04) {
            feedback_t fb {.kind = feedback_t::kind_e::rgb_led};
            fb.r = data[44];
            fb.g = data[45];
            fb.b = data[46];
            result.push_back(fb);
          }
          if (flag1 & 0x10) {
            feedback_t fb {.kind = feedback_t::kind_e::player_leds};
            fb.solid = static_cast<std::uint8_t>(data[43] & 0x1F);
            fb.flashing = 0;
            result.push_back(fb);
          }
        }
        break;

      case output_kind_e::switch_pro:
        if (source == output_source::hid_output && (report_id == 0x10 || report_id == 0x01) && data.size() >= 9) {
          // Byte 0 is the packet counter; two 4-byte HD rumble blocks follow (left, right)
          feedback_t fb {.kind = feedback_t::kind_e::rumble};
          fb.lowfreq = expand8(switch_rumble_amplitude(data.subspan(1, 4)));
          fb.highfreq = expand8(switch_rumble_amplitude(data.subspan(5, 4)));
          result.push_back(fb);
        }
        break;

      case output_kind_e::none:
        break;
    }

    return result;
  }

}  // namespace platf::hidmaestro::report
