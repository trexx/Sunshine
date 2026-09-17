/**
 * @file src/platform/hidmaestro/report.h
 * @brief Platform-neutral input report packing and output report decoding for HIDMaestro.
 *
 * Sunshine writes HIDMaestro input frames itself (the broker only manages device
 * lifecycle), so the exact byte layout of every supported profile lives here. The code
 * depends only on the standard library and `protocol.h` so it can be unit-tested on
 * every platform; the Windows backend adapts `platf::gamepad_state_t` and friends to
 * the small mirror types declared below.
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

// local includes
#include "src/platform/hidmaestro/protocol.h"

namespace platf::hidmaestro::report {

  /**
   * @brief Moonlight gamepad button flags (mirror of the constants in `platf`).
   */
  namespace button {
    constexpr std::uint32_t dpad_up = 0x0001;  ///< D-pad up.
    constexpr std::uint32_t dpad_down = 0x0002;  ///< D-pad down.
    constexpr std::uint32_t dpad_left = 0x0004;  ///< D-pad left.
    constexpr std::uint32_t dpad_right = 0x0008;  ///< D-pad right.
    constexpr std::uint32_t start = 0x0010;  ///< Start.
    constexpr std::uint32_t back = 0x0020;  ///< Back.
    constexpr std::uint32_t left_stick = 0x0040;  ///< Left stick click.
    constexpr std::uint32_t right_stick = 0x0080;  ///< Right stick click.
    constexpr std::uint32_t left_button = 0x0100;  ///< Left shoulder.
    constexpr std::uint32_t right_button = 0x0200;  ///< Right shoulder.
    constexpr std::uint32_t home = 0x0400;  ///< Guide / Home.
    constexpr std::uint32_t a = 0x1000;  ///< A.
    constexpr std::uint32_t b = 0x2000;  ///< B.
    constexpr std::uint32_t x = 0x4000;  ///< X.
    constexpr std::uint32_t y = 0x8000;  ///< Y.
    constexpr std::uint32_t paddle1 = 0x010000;  ///< Paddle 1.
    constexpr std::uint32_t paddle2 = 0x020000;  ///< Paddle 2.
    constexpr std::uint32_t paddle3 = 0x040000;  ///< Paddle 3.
    constexpr std::uint32_t paddle4 = 0x080000;  ///< Paddle 4.
    constexpr std::uint32_t touchpad = 0x100000;  ///< Touchpad click.
    constexpr std::uint32_t misc = 0x200000;  ///< Miscellaneous button.
  }  // namespace button

  /**
   * @brief Moonlight motion sensor types (mirror of `LI_MOTION_TYPE_*`).
   */
  namespace motion {
    constexpr std::uint8_t accel = 0x01;  ///< Accelerometer, m/s².
    constexpr std::uint8_t gyro = 0x02;  ///< Gyroscope, deg/s.
  }  // namespace motion

  /**
   * @brief Moonlight touch event types (mirror of `LI_TOUCH_EVENT_*`).
   */
  namespace touch_event {
    constexpr std::uint8_t hover = 0x00;  ///< Hover.
    constexpr std::uint8_t down = 0x01;  ///< Down.
    constexpr std::uint8_t up = 0x02;  ///< Up.
    constexpr std::uint8_t move = 0x03;  ///< Move.
    constexpr std::uint8_t cancel = 0x04;  ///< Cancel.
    constexpr std::uint8_t button_only = 0x05;  ///< Button only.
    constexpr std::uint8_t hover_leave = 0x06;  ///< Hover leave.
    constexpr std::uint8_t cancel_all = 0x07;  ///< Cancel all.
  }  // namespace touch_event

  /**
   * @brief Moonlight battery states (mirror of `LI_BATTERY_STATE_*`).
   */
  namespace battery_state {
    constexpr std::uint8_t unknown = 0x00;  ///< Unknown.
    constexpr std::uint8_t not_present = 0x01;  ///< Not present.
    constexpr std::uint8_t discharging = 0x02;  ///< Discharging.
    constexpr std::uint8_t charging = 0x03;  ///< Charging.
    constexpr std::uint8_t not_charging = 0x04;  ///< Not charging.
    constexpr std::uint8_t full = 0x05;  ///< Full.
    constexpr std::uint8_t percentage_unknown = 0xFF;  ///< Percentage unknown sentinel.
  }  // namespace battery_state

  /**
   * @brief Gamepad state as received from Moonlight (mirror of `platf::gamepad_state_t`).
   */
  struct state_t {
    std::uint32_t button_flags = 0;  ///< Moonlight button flags.
    std::uint8_t lt = 0;  ///< Left trigger, 0..255.
    std::uint8_t rt = 0;  ///< Right trigger, 0..255.
    std::int16_t ls_x = 0;  ///< Left stick X, right positive.
    std::int16_t ls_y = 0;  ///< Left stick Y, up positive.
    std::int16_t rs_x = 0;  ///< Right stick X, right positive.
    std::int16_t rs_y = 0;  ///< Right stick Y, up positive.
  };

  /**
   * @brief Options that influence how Sony profiles interpret Moonlight buttons.
   */
  struct options_t {
    bool back_as_touchpad_click = false;  ///< Also press the touchpad when Back is pressed.
  };

  /**
   * @brief Touch event as received from Moonlight (mirror of `platf::gamepad_touch_t`).
   */
  struct touch_t {
    std::uint8_t event_type = 0;  ///< `touch_event` value.
    std::uint32_t pointer_id = 0;  ///< Client pointer id.
    float x = 0;  ///< Normalized X, 0..1.
    float y = 0;  ///< Normalized Y, 0..1.
  };

  /**
   * @brief Capacity of the HIDMaestro shared-memory body.
   */
  constexpr std::size_t BODY_CAPACITY = 256;

  /**
   * @brief Size of the XUSB companion (GIP) buffer.
   */
  constexpr std::size_t GIP_SIZE = 14;

  /**
   * @brief Determine the 8-way hat direction from Moonlight d-pad flags.
   *
   * @param button_flags Moonlight button flags.
   * @return 0..7 for N, NE, E, SE, S, SW, W, NW; -1 when centered.
   */
  int hat_direction(std::uint32_t button_flags);

  /**
   * @brief Write a little-endian bitfield into a report body.
   *
   * @param body Report body.
   * @param bit_offset Offset of the least-significant bit.
   * @param bit_size Field width in bits (1..32).
   * @param value Value to store; only the low `bit_size` bits are used.
   */
  void write_bits(std::span<std::uint8_t> body, std::uint32_t bit_offset, std::uint32_t bit_size, std::uint32_t value);

  /**
   * @brief Read a little-endian bitfield from a report body.
   *
   * @param body Report body.
   * @param bit_offset Offset of the least-significant bit.
   * @param bit_size Field width in bits (1..32).
   * @return Extracted value.
   */
  std::uint32_t read_bits(std::span<const std::uint8_t> body, std::uint32_t bit_offset, std::uint32_t bit_size);

  /**
   * @brief Scale a signed 16-bit stick value into a HID logical range.
   *
   * @param value Stick value, -32768..32767.
   * @param field Target field (logical range).
   * @param invert Negate the value first (HID Y convention).
   * @return Logical value within `[logical_min, logical_max]`.
   */
  std::int32_t scale_axis(std::int16_t value, const proto::field_t &field, bool invert);

  /**
   * @brief Scale an 8-bit trigger into a HID logical range.
   *
   * @param value Trigger value, 0..255.
   * @param field Target field (logical range).
   * @return Logical value within `[logical_min, logical_max]`.
   */
  std::int32_t scale_trigger(std::uint8_t value, const proto::field_t &field);

  /**
   * @brief Pack a report body from a packing plan (`pack_mode_e::hid_generic`).
   *
   * @param plan Packing plan received from the broker.
   * @param state Gamepad state.
   * @param body Destination; at least `plan.data_size` bytes, zeroed by this call.
   * @return Number of body bytes written (`plan.data_size`).
   */
  std::size_t pack_generic(const proto::packing_plan_t &plan, const state_t &state, std::span<std::uint8_t> body);

  /**
   * @brief Pack the 14-byte XUSB companion buffer used by Xbox 360 profiles.
   *
   * @param state Gamepad state.
   * @param y_axis_hid_down Whether stick Y grows downward (matches the HID body convention).
   * @param gip Destination buffer.
   */
  void pack_gip(const state_t &state, bool y_axis_hid_down, std::span<std::uint8_t, GIP_SIZE> gip);

  // ---------------------------------------------------------------------------
  // DualShock 4 (USB report 0x01, 63-byte body)
  // ---------------------------------------------------------------------------

#pragma pack(push, 1)

  /**
   * @brief One DualShock 4 touch packet (two fingers).
   */
  struct ds4_touch_t {
    std::uint8_t packet_counter;  ///< Incremented per touch report.
    std::uint8_t is_up_tracking_num1;  ///< Bit 7 set when finger 1 is up; low 7 bits tracking id.
    std::uint8_t touch_data1[3];  ///< Finger 1 packed 12-bit X/Y.
    std::uint8_t is_up_tracking_num2;  ///< Bit 7 set when finger 2 is up; low 7 bits tracking id.
    std::uint8_t touch_data2[3];  ///< Finger 2 packed 12-bit X/Y.
  };

  /**
   * @brief DualShock 4 USB input report 0x01 body (Report ID excluded).
   */
  struct ds4_report_t {
    std::uint8_t thumb_lx;  ///< Left stick X, 0 left.
    std::uint8_t thumb_ly;  ///< Left stick Y, 0 up.
    std::uint8_t thumb_rx;  ///< Right stick X, 0 left.
    std::uint8_t thumb_ry;  ///< Right stick Y, 0 up.
    std::uint16_t buttons;  ///< Bits 0-3 hat, bits 4-15 buttons.
    std::uint8_t special;  ///< Bit 0 PS, bit 1 touchpad click, bits 2-7 counter.
    std::uint8_t trigger_l;  ///< L2, 0..255.
    std::uint8_t trigger_r;  ///< R2, 0..255.
    std::uint16_t timestamp;  ///< Sensor timestamp in 5.333 µs units.
    std::uint8_t battery_lvl;  ///< Battery level, 0..255.
    std::int16_t gyro_x;  ///< Gyro pitch.
    std::int16_t gyro_y;  ///< Gyro yaw.
    std::int16_t gyro_z;  ///< Gyro roll.
    std::int16_t accel_x;  ///< Accelerometer X.
    std::int16_t accel_y;  ///< Accelerometer Y.
    std::int16_t accel_z;  ///< Accelerometer Z.
    std::uint8_t unknown1[5];  ///< Reserved.
    std::uint8_t battery_lvl_special;  ///< Bit 4 USB connected, bits 0-3 level / status.
    std::uint8_t unknown2[2];  ///< Reserved.
    std::uint8_t touch_packets_n;  ///< Number of valid touch packets (1..3).
    ds4_touch_t current_touch;  ///< Most recent touch packet.
    ds4_touch_t previous_touch[2];  ///< Older touch packets.
    std::uint8_t padding[3];  ///< Padding to 63 bytes.
  };

  static_assert(sizeof(ds4_touch_t) == 9);
  static_assert(sizeof(ds4_report_t) == 63);

  /**
   * @brief DualSense touch point (4 bytes).
   */
  struct dualsense_touch_point_t {
    std::uint8_t contact;  ///< Bit 7 set when inactive; low 7 bits contact id.
    std::uint8_t x_lo;  ///< X bits 0-7.
    std::uint8_t x_hi_y_lo;  ///< X bits 8-11 in low nibble, Y bits 0-3 in high nibble.
    std::uint8_t y_hi;  ///< Y bits 4-11.
  };

  /**
   * @brief DualSense USB input report 0x01 body (Report ID excluded).
   */
  struct dualsense_report_t {
    std::uint8_t x;  ///< Left stick X, 0 left.
    std::uint8_t y;  ///< Left stick Y, 0 up.
    std::uint8_t rx;  ///< Right stick X, 0 left.
    std::uint8_t ry;  ///< Right stick Y, 0 up.
    std::uint8_t z;  ///< L2, 0..255.
    std::uint8_t rz;  ///< R2, 0..255.
    std::uint8_t seq_number;  ///< Rolling sequence number.
    std::uint8_t buttons[4];  ///< [0] hat + face, [1] shoulders/system/sticks, [2] PS/touchpad/mute, [3] reserved.
    std::uint8_t reserved[4];  ///< Reserved.
    std::int16_t gyro[3];  ///< Gyro pitch, yaw, roll.
    std::int16_t accel[3];  ///< Accelerometer X, Y, Z.
    std::uint32_t sensor_timestamp;  ///< Sensor timestamp in 0.33 µs units.
    std::uint8_t reserved2;  ///< Reserved.
    dualsense_touch_point_t points[2];  ///< Touch points.
    std::uint8_t reserved3[12];  ///< Reserved.
    std::uint8_t status;  ///< Bits 0-3 battery level, bits 4-7 charging status.
    std::uint8_t reserved4[10];  ///< Reserved.
  };

  static_assert(sizeof(dualsense_touch_point_t) == 4);
  static_assert(sizeof(dualsense_report_t) == 63);

#pragma pack(pop)

  /**
   * @brief Motion sensor scaling applied when packing Sony reports.
   */
  struct motion_scale_t {
    float accel_per_g = 8192.0F;  ///< Raw accelerometer counts per g.
    float gyro_per_dps = 16.0F;  ///< Raw gyroscope counts per deg/s.
  };

  /**
   * @brief Per-controller touch pointer bookkeeping for Sony touchpads.
   */
  struct touch_state_t {
    std::map<std::uint32_t, std::uint8_t> pointer_id_map;  ///< Client pointer id → finger slot.
    std::uint8_t available_pointers = 0x3;  ///< Bitmask of free finger slots.
    std::uint8_t next_tracking_id = 0;  ///< Next tracking id to assign.
  };

  /**
   * @brief Initialize a DualShock 4 report to the idle state.
   *
   * @param report Report to initialize.
   */
  void ds4_init(ds4_report_t &report);

  /**
   * @brief Apply Moonlight gamepad state to a DualShock 4 report.
   *
   * @param report Report to update.
   * @param state Gamepad state.
   * @param options Sony button options.
   */
  void ds4_update_state(ds4_report_t &report, const state_t &state, const options_t &options);

  /**
   * @brief Apply a motion sample to a DualShock 4 report.
   *
   * @param report Report to update.
   * @param motion_type `motion::accel` or `motion::gyro`.
   * @param x X component (m/s² or deg/s).
   * @param y Y component.
   * @param z Z component.
   * @param scale Sensor scaling.
   */
  void ds4_update_motion(ds4_report_t &report, std::uint8_t motion_type, float x, float y, float z, const motion_scale_t &scale);

  /**
   * @brief Apply a touch event to a DualShock 4 report.
   *
   * @param report Report to update.
   * @param touch_state Pointer bookkeeping for this controller.
   * @param touch Touch event.
   * @return `true` when the report changed.
   */
  bool ds4_update_touch(ds4_report_t &report, touch_state_t &touch_state, const touch_t &touch);

  /**
   * @brief Apply a battery update to a DualShock 4 report.
   *
   * @param report Report to update.
   * @param state `battery_state` value.
   * @param percentage 0..100 or `battery_state::percentage_unknown`.
   */
  void ds4_update_battery(ds4_report_t &report, std::uint8_t state, std::uint8_t percentage);

  /**
   * @brief Advance the DualShock 4 timestamp by an elapsed duration.
   *
   * @param report Report to update.
   * @param elapsed Time since the previous report.
   */
  void ds4_advance(ds4_report_t &report, std::chrono::nanoseconds elapsed);

  /**
   * @brief Initialize a DualSense report to the idle state.
   *
   * @param report Report to initialize.
   */
  void dualsense_init(dualsense_report_t &report);

  /**
   * @brief Apply Moonlight gamepad state to a DualSense report.
   *
   * @param report Report to update.
   * @param state Gamepad state.
   * @param options Sony button options.
   */
  void dualsense_update_state(dualsense_report_t &report, const state_t &state, const options_t &options);

  /**
   * @brief Apply a motion sample to a DualSense report.
   *
   * @param report Report to update.
   * @param motion_type `motion::accel` or `motion::gyro`.
   * @param x X component (m/s² or deg/s).
   * @param y Y component.
   * @param z Z component.
   * @param scale Sensor scaling.
   */
  void dualsense_update_motion(dualsense_report_t &report, std::uint8_t motion_type, float x, float y, float z, const motion_scale_t &scale);

  /**
   * @brief Apply a touch event to a DualSense report.
   *
   * @param report Report to update.
   * @param touch_state Pointer bookkeeping for this controller.
   * @param touch Touch event.
   * @return `true` when the report changed.
   */
  bool dualsense_update_touch(dualsense_report_t &report, touch_state_t &touch_state, const touch_t &touch);

  /**
   * @brief Apply a battery update to a DualSense report.
   *
   * @param report Report to update.
   * @param state `battery_state` value.
   * @param percentage 0..100 or `battery_state::percentage_unknown`.
   */
  void dualsense_update_battery(dualsense_report_t &report, std::uint8_t state, std::uint8_t percentage);

  /**
   * @brief Advance the DualSense sequence number and sensor timestamp.
   *
   * @param report Report to update.
   * @param elapsed Time since the previous report.
   */
  void dualsense_advance(dualsense_report_t &report, std::chrono::nanoseconds elapsed);

  // ---------------------------------------------------------------------------
  // Nintendo Switch Pro (intermediate body consumed by the driver's protocol responder)
  // ---------------------------------------------------------------------------

  /**
   * @brief Size of the Switch Pro intermediate body written to shared memory.
   *
   * Layout (mirrors `SwitchProPacker.BuildBody` in the HIDMaestro SDK): bytes 0-1 zero,
   * 2-4 button bytes, 5-7 left stick (12-bit packed), 8-10 right stick, 11 zero,
   * 12-47 three identical 12-byte IMU frames (accel X/Y/Z, gyro X/Y/Z as int16).
   */
  constexpr std::size_t SWITCH_PRO_BODY_SIZE = 48;

  /**
   * @brief Latest IMU sample for a Switch Pro controller, already in wire units.
   */
  struct switch_pro_state_t {
    std::int16_t accel[3] {};  ///< Wire accelerometer X, Y, Z.
    std::int16_t gyro[3] {};  ///< Wire gyroscope X, Y, Z.
  };

  /**
   * @brief Apply a motion sample to the Switch Pro IMU state.
   *
   * Moonlight delivers SDL-frame vectors; this converts to the Switch wire frame exactly
   * as the HIDMaestro SDK does so a round trip through SDL yields the original vector.
   *
   * @param imu IMU state to update.
   * @param motion_type `motion::accel` or `motion::gyro`.
   * @param x X component (m/s² or deg/s).
   * @param y Y component.
   * @param z Z component.
   */
  void switch_pro_update_motion(switch_pro_state_t &imu, std::uint8_t motion_type, float x, float y, float z);

  /**
   * @brief Pack the Switch Pro intermediate body.
   *
   * Moonlight reports face buttons by position (A south, B east, X west, Y north), so
   * they map onto the Switch's B, A, Y, X respectively.
   *
   * @param state Gamepad state.
   * @param imu Latest IMU sample.
   * @param body Destination of `SWITCH_PRO_BODY_SIZE` bytes.
   */
  void pack_switch_pro_body(const state_t &state, const switch_pro_state_t &imu, std::span<std::uint8_t, SWITCH_PRO_BODY_SIZE> body);

  // ---------------------------------------------------------------------------
  // Output (rumble / LED) decoding
  // ---------------------------------------------------------------------------

  /**
   * @brief Output source values published on the HIDMaestro output ring.
   */
  namespace output_source {
    constexpr std::uint8_t hid_output = 0;  ///< HID output report.
    constexpr std::uint8_t hid_feature = 1;  ///< HID feature report (set).
    constexpr std::uint8_t xinput = 2;  ///< XInput vibration via the XUSB companion.
    constexpr std::uint8_t hid_feature_read = 3;  ///< HID feature report (get) notification.
  }  // namespace output_source

  /**
   * @brief Which output protocol a controller speaks.
   */
  enum class output_kind_e : std::uint8_t {
    none,  ///< No output supported.
    xbox_360,  ///< XInput vibration packets via the XUSB companion.
    xbox_one_hid,  ///< Xbox One HID rumble report 0x03 via `xinputhid`.
    ds4,  ///< DualShock 4 USB output report 0x05.
    dualsense,  ///< DualSense USB output report 0x02.
    switch_pro,  ///< Switch Pro rumble subcommands.
  };

  /**
   * @brief Decoded feedback destined for the Moonlight client.
   */
  struct feedback_t {
    /**
     * @brief Feedback kinds.
     */
    enum class kind_e : std::uint8_t {
      rumble,  ///< Main motors.
      rumble_triggers,  ///< Trigger motors.
      rgb_led,  ///< Lightbar color.
      player_leds,  ///< Player indicator LEDs.
      adaptive_triggers,  ///< DualSense trigger effects.
    };

    kind_e kind;  ///< Feedback kind.
    std::uint16_t lowfreq = 0;  ///< Low-frequency (strong) motor, 0..65535.
    std::uint16_t highfreq = 0;  ///< High-frequency (weak) motor, 0..65535.
    std::uint16_t left_trigger = 0;  ///< Left trigger motor, 0..65535.
    std::uint16_t right_trigger = 0;  ///< Right trigger motor, 0..65535.
    std::uint8_t r = 0;  ///< Red.
    std::uint8_t g = 0;  ///< Green.
    std::uint8_t b = 0;  ///< Blue.
    std::uint8_t solid = 0;  ///< Solid player LED mask.
    std::uint8_t flashing = 0;  ///< Flashing player LED mask.
    std::uint8_t event_flags = 0;  ///< Adaptive trigger valid flags.
    std::uint8_t type_left = 0;  ///< Left trigger effect type.
    std::uint8_t type_right = 0;  ///< Right trigger effect type.
    std::array<std::uint8_t, 10> left {};  ///< Left trigger effect parameters.
    std::array<std::uint8_t, 10> right {};  ///< Right trigger effect parameters.
  };

  /**
   * @brief Decode one output packet from the HIDMaestro output ring.
   *
   * @param kind Output protocol of the controller.
   * @param source `output_source` value.
   * @param report_id HID Report ID (0 when none).
   * @param data Packet payload.
   * @return Zero or more feedback messages.
   */
  std::vector<feedback_t> decode_output(output_kind_e kind, std::uint8_t source, std::uint8_t report_id, std::span<const std::uint8_t> data);

}  // namespace platf::hidmaestro::report
