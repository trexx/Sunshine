/**
 * @file tests/unit/platform/test_hidmaestro_report.cpp
 * @brief Unit tests for HIDMaestro input report packing and output decoding.
 */
#include "../../tests_common.h"

#include <array>
#include <cstdint>
#include <span>

#include "src/platform/hidmaestro/protocol.h"
#include "src/platform/hidmaestro/report.h"

namespace {
  using namespace platf::hidmaestro;

  proto::field_t make_field(unsigned bit_offset, unsigned bit_size, int logical_min, int logical_max) {
    return proto::field_t {
      .bit_offset = static_cast<std::uint16_t>(bit_offset),
      .bit_size = static_cast<std::uint8_t>(bit_size),
      .present = 1,
      .logical_min = logical_min,
      .logical_max = logical_max,
    };
  }

  /**
   * @brief Packing plan matching HIDMaestro's `xbox-360-wired` descriptor.
   *
   * Body layout: LX/LY/RX/RY u16, combined Z u16, LT/RT (Vx/Vy) u16, 10 buttons, 4-bit hat, padding to 18 bytes.
   */
  proto::packing_plan_t xbox_360_plan() {
    proto::packing_plan_t plan {};
    plan.mode = static_cast<std::uint8_t>(proto::pack_mode_e::hid_generic);
    plan.data_size = 18;
    plan.packs_gip = 1;
    plan.hat_positions = 8;
    plan.hat_null = 0;
    plan.y_axis_hid_down = 1;
    plan.lx = make_field(0, 16, 0, 65535);
    plan.ly = make_field(16, 16, 0, 65535);
    plan.rx = make_field(32, 16, 0, 65535);
    plan.ry = make_field(48, 16, 0, 65535);
    plan.combined_z = make_field(64, 16, 0, 65535);
    plan.lt = make_field(80, 16, 0, 65535);
    plan.rt = make_field(96, 16, 0, 65535);
    plan.button_count = 10;
    for (unsigned i = 0; i < 10; ++i) {
      plan.buttons[i] = make_field(112 + i, 1, 0, 1);
    }
    plan.hat = make_field(122, 4, 1, 8);
    for (auto &role : plan.role_to_button) {
      role = proto::ROLE_UNMAPPED;
    }
    using proto::button_role_e;
    plan.role_to_button[static_cast<int>(button_role_e::a)] = 0;
    plan.role_to_button[static_cast<int>(button_role_e::b)] = 1;
    plan.role_to_button[static_cast<int>(button_role_e::x)] = 2;
    plan.role_to_button[static_cast<int>(button_role_e::y)] = 3;
    plan.role_to_button[static_cast<int>(button_role_e::left_bumper)] = 4;
    plan.role_to_button[static_cast<int>(button_role_e::right_bumper)] = 5;
    plan.role_to_button[static_cast<int>(button_role_e::back)] = 6;
    plan.role_to_button[static_cast<int>(button_role_e::start)] = 7;
    plan.role_to_button[static_cast<int>(button_role_e::left_stick)] = 8;
    plan.role_to_button[static_cast<int>(button_role_e::right_stick)] = 9;
    return plan;
  }

  std::uint16_t u16le(const std::uint8_t *p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
  }
}  // namespace

TEST(HidMaestroProtocol, MessageLayoutsMatchTheBrokerContract) {
  EXPECT_EQ(sizeof(proto::header_t), 16u);
  EXPECT_EQ(sizeof(proto::field_t), 12u);
  EXPECT_EQ(sizeof(proto::packing_plan_t), 508u);
  EXPECT_EQ(sizeof(proto::hello_response_t), 56u);
  EXPECT_EQ(sizeof(proto::ensure_driver_response_t), 272u);
  EXPECT_EQ(sizeof(proto::create_controller_request_t), 148u);
  EXPECT_EQ(sizeof(proto::create_controller_response_t), 780u);
  EXPECT_EQ(sizeof(proto::status_response_t), 56u);

  const auto header = proto::make_header<proto::create_controller_request_t>(proto::request_type_e::create_controller);
  EXPECT_EQ(header.version, proto::PROTOCOL_VERSION);
  EXPECT_EQ(header.size, sizeof(proto::create_controller_request_t));
  EXPECT_EQ(header.type, static_cast<std::uint32_t>(proto::request_type_e::create_controller));
}

TEST(HidMaestroReport, HatDirectionFollowsDpadFlags) {
  EXPECT_EQ(report::hat_direction(0), -1);
  EXPECT_EQ(report::hat_direction(report::button::dpad_up), 0);
  EXPECT_EQ(report::hat_direction(report::button::dpad_up | report::button::dpad_right), 1);
  EXPECT_EQ(report::hat_direction(report::button::dpad_right), 2);
  EXPECT_EQ(report::hat_direction(report::button::dpad_down | report::button::dpad_right), 3);
  EXPECT_EQ(report::hat_direction(report::button::dpad_down), 4);
  EXPECT_EQ(report::hat_direction(report::button::dpad_down | report::button::dpad_left), 5);
  EXPECT_EQ(report::hat_direction(report::button::dpad_left), 6);
  EXPECT_EQ(report::hat_direction(report::button::dpad_up | report::button::dpad_left), 7);
}

TEST(HidMaestroReport, BitfieldWriterRoundTrips) {
  std::array<std::uint8_t, 4> body {};
  report::write_bits(body, 3, 12, 0xABC);
  EXPECT_EQ(report::read_bits(body, 3, 12), 0xABCu);
  EXPECT_EQ(report::read_bits(body, 0, 3), 0u);
  report::write_bits(body, 3, 12, 0);
  EXPECT_EQ(report::read_bits(body, 0, 32), 0u);
}

TEST(HidMaestroReport, AxisScalingCoversTheLogicalRange) {
  const auto axis = make_field(0, 16, 0, 65535);
  EXPECT_EQ(report::scale_axis(-32768, axis, false), 0);
  EXPECT_EQ(report::scale_axis(32767, axis, false), 65535);
  EXPECT_EQ(report::scale_axis(0, axis, false), 32768);
  EXPECT_EQ(report::scale_axis(32767, axis, true), 0);
  EXPECT_EQ(report::scale_axis(-32768, axis, true), 65535);

  const auto byte_axis = make_field(0, 8, 0, 255);
  EXPECT_EQ(report::scale_axis(0, byte_axis, false), 128);
  EXPECT_EQ(report::scale_axis(32767, byte_axis, true), 0);

  const auto trigger = make_field(0, 10, 0, 1023);
  EXPECT_EQ(report::scale_trigger(0, trigger), 0);
  EXPECT_EQ(report::scale_trigger(255, trigger), 1023);
}

TEST(HidMaestroReport, PacksXbox360BodyAndGipBuffer) {
  const auto plan = xbox_360_plan();
  std::array<std::uint8_t, report::BODY_CAPACITY> body {};

  report::state_t idle {};
  ASSERT_EQ(report::pack_generic(plan, idle, body), 18u);
  EXPECT_EQ(u16le(&body[0]), 0x8000);
  EXPECT_EQ(u16le(&body[2]), 0x7FFF);  // mirrored HID Y
  EXPECT_EQ(u16le(&body[8]), 0x8000);  // combined Z centered
  EXPECT_EQ(u16le(&body[10]), 0);
  EXPECT_EQ(u16le(&body[12]), 0);
  EXPECT_EQ(body[14], 0);
  EXPECT_EQ(body[15], 0);

  report::state_t state {};
  state.button_flags = report::button::a | report::button::y | report::button::right_stick | report::button::dpad_up | report::button::dpad_right;
  state.ls_y = 32767;
  state.lt = 255;
  report::pack_generic(plan, state, body);
  EXPECT_EQ(u16le(&body[2]), 0);  // stick up is 0 in HID convention
  EXPECT_EQ(body[14], 0x09);  // A + Y
  EXPECT_EQ(body[15] & 0x03, 0x02);  // right stick is button 10
  EXPECT_EQ((body[15] >> 2) & 0x0F, 2);  // hat north-east
  EXPECT_EQ(u16le(&body[8]), 0);  // full LT pulls combined Z to the minimum
  EXPECT_EQ(u16le(&body[10]), 0xFFFF);

  std::array<std::uint8_t, report::GIP_SIZE> gip {};
  report::pack_gip(state, true, gip);
  EXPECT_EQ(u16le(&gip[2]), 0);
  EXPECT_EQ(u16le(&gip[8]), 1023);
  EXPECT_EQ(gip[12], 0x89);  // A | Y | RS
  EXPECT_EQ(gip[13], 2 << 2);  // hat NE

  state.button_flags = report::button::home | report::button::back | report::button::start;
  report::pack_gip(state, true, gip);
  EXPECT_EQ(gip[13], 0x40 | 0x01 | 0x02);
}

TEST(HidMaestroReport, GenericPlanIgnoresUnmappedRolesAndFourWayHats) {
  auto plan = xbox_360_plan();
  plan.hat_positions = 4;
  plan.hat = make_field(122, 4, 0, 3);
  plan.hat_null = 4;
  plan.role_to_button[static_cast<int>(proto::button_role_e::a)] = proto::ROLE_UNMAPPED;

  std::array<std::uint8_t, report::BODY_CAPACITY> body {};
  report::state_t state {};
  state.button_flags = report::button::a | report::button::dpad_down | report::button::dpad_left;
  report::pack_generic(plan, state, body);
  EXPECT_EQ(body[14] & 0x01, 0);  // A no longer mapped
  EXPECT_EQ((body[15] >> 2) & 0x0F, 2);  // SW rounds to S on a 4-way hat

  state.button_flags = 0;
  report::pack_generic(plan, state, body);
  EXPECT_EQ((body[15] >> 2) & 0x0F, 4);  // centered
}

TEST(HidMaestroReport, PacksDualShock4Report) {
  report::ds4_report_t ds4;
  report::ds4_init(ds4);
  EXPECT_EQ(sizeof(ds4), 63u);
  EXPECT_EQ(ds4.thumb_lx, 0x80);
  EXPECT_EQ(ds4.buttons & 0xF, 8);
  EXPECT_EQ(ds4.battery_lvl_special, 0x1A);

  report::state_t state {};
  state.button_flags = report::button::a | report::button::y | report::button::right_stick | report::button::dpad_up | report::button::dpad_right | report::button::home | report::button::misc;
  state.ls_y = 32767;
  state.lt = 255;
  report::ds4_update_state(ds4, state, {});
  EXPECT_EQ(ds4.buttons & 0xF, 1);  // NE
  EXPECT_TRUE(ds4.buttons & (1 << 5));  // cross
  EXPECT_TRUE(ds4.buttons & (1 << 7));  // triangle
  EXPECT_TRUE(ds4.buttons & (1 << 10));  // L2 digital
  EXPECT_TRUE(ds4.buttons & (1 << 15));  // R3
  EXPECT_EQ(ds4.special & 0x03, 0x03);  // PS + touchpad (misc)
  EXPECT_EQ(ds4.thumb_ly, 0);
  EXPECT_EQ(ds4.trigger_l, 255);

  report::ds4_update_state(ds4, report::state_t {.button_flags = report::button::back}, {.back_as_touchpad_click = true});
  EXPECT_EQ(ds4.special & 0x02, 0x02);
  EXPECT_TRUE(ds4.buttons & (1 << 12));  // share

  const report::motion_scale_t scale {.accel_per_g = 10000.0F, .gyro_per_dps = 20.0F};
  report::ds4_update_motion(ds4, report::motion::accel, 0.0F, 9.80665F, 0.0F, scale);
  EXPECT_EQ(ds4.accel_y, 10000);
  report::ds4_update_motion(ds4, report::motion::gyro, 90.0F, 0.0F, -45.0F, scale);
  EXPECT_EQ(ds4.gyro_x, 1800);
  EXPECT_EQ(ds4.gyro_z, -900);

  report::touch_state_t touches;
  EXPECT_TRUE(report::ds4_update_touch(ds4, touches, {.event_type = report::touch_event::down, .pointer_id = 7, .x = 0.5F, .y = 1.0F}));
  EXPECT_EQ(ds4.current_touch.is_up_tracking_num1 & 0x80, 0);
  const unsigned x = ds4.current_touch.touch_data1[0] | ((ds4.current_touch.touch_data1[1] & 0x0F) << 8);
  const unsigned y = (ds4.current_touch.touch_data1[1] >> 4) | (ds4.current_touch.touch_data1[2] << 4);
  EXPECT_EQ(x, 960u);
  EXPECT_EQ(y, 943u);
  EXPECT_TRUE(report::ds4_update_touch(ds4, touches, {.event_type = report::touch_event::up, .pointer_id = 7}));
  EXPECT_EQ(ds4.current_touch.is_up_tracking_num1 & 0x80, 0x80);
  EXPECT_FALSE(report::ds4_update_touch(ds4, touches, {.event_type = report::touch_event::move, .pointer_id = 99}));

  report::ds4_update_battery(ds4, report::battery_state::discharging, 50);
  EXPECT_EQ(ds4.battery_lvl_special & 0x10, 0);
  EXPECT_EQ(ds4.battery_lvl, 127);
  report::ds4_update_battery(ds4, report::battery_state::charging, 90);
  EXPECT_EQ(ds4.battery_lvl_special, 0x19);
  report::ds4_update_battery(ds4, report::battery_state::full, report::battery_state::percentage_unknown);
  EXPECT_EQ(ds4.battery_lvl_special, 0x1B);

  const auto before = ds4.timestamp;
  report::ds4_advance(ds4, std::chrono::nanoseconds(5333 * 10));  // ten 5.333 µs ticks
  EXPECT_EQ(static_cast<std::uint16_t>(ds4.timestamp - before), 10);
}

TEST(HidMaestroReport, PacksDualSenseReport) {
  report::dualsense_report_t ds5;
  report::dualsense_init(ds5);
  EXPECT_EQ(sizeof(ds5), 63u);
  EXPECT_EQ(ds5.buttons[0], 0x08);
  EXPECT_EQ(ds5.points[0].contact, 0x80);

  report::state_t state {};
  state.button_flags = report::button::a | report::button::y | report::button::right_stick | report::button::dpad_up | report::button::dpad_right | report::button::misc | report::button::touchpad;
  state.lt = 200;
  report::dualsense_update_state(ds5, state, {});
  EXPECT_EQ(ds5.buttons[0] & 0x0F, 1);
  EXPECT_TRUE(ds5.buttons[0] & 0x20);  // cross
  EXPECT_TRUE(ds5.buttons[0] & 0x80);  // triangle
  EXPECT_TRUE(ds5.buttons[1] & 0x04);  // L2 digital
  EXPECT_TRUE(ds5.buttons[1] & 0x80);  // R3
  EXPECT_EQ(ds5.buttons[2], 0x02 | 0x04);  // touchpad + mute
  EXPECT_EQ(ds5.z, 200);

  report::touch_state_t touches;
  ASSERT_TRUE(report::dualsense_update_touch(ds5, touches, {.event_type = report::touch_event::down, .pointer_id = 1, .x = 0.5F, .y = 0.5F}));
  ASSERT_TRUE(report::dualsense_update_touch(ds5, touches, {.event_type = report::touch_event::down, .pointer_id = 2, .x = 0.0F, .y = 0.0F}));
  EXPECT_FALSE(report::dualsense_update_touch(ds5, touches, {.event_type = report::touch_event::down, .pointer_id = 3}));  // only two fingers
  const unsigned x = ds5.points[0].x_lo | ((ds5.points[0].x_hi_y_lo & 0x0F) << 8);
  const unsigned y = (ds5.points[0].x_hi_y_lo >> 4) | (ds5.points[0].y_hi << 4);
  EXPECT_EQ(x, 960u);
  EXPECT_EQ(y, 540u);
  EXPECT_EQ(ds5.points[1].contact & 0x80, 0);
  ASSERT_TRUE(report::dualsense_update_touch(ds5, touches, {.event_type = report::touch_event::cancel_all}));
  EXPECT_EQ(ds5.points[0].contact & 0x80, 0x80);
  EXPECT_EQ(ds5.points[1].contact & 0x80, 0x80);

  report::dualsense_update_battery(ds5, report::battery_state::charging, 45);
  EXPECT_EQ(ds5.status, 0x14);
  report::dualsense_update_battery(ds5, report::battery_state::full, report::battery_state::percentage_unknown);
  EXPECT_EQ(ds5.status, 0x2A);

  const auto seq = ds5.seq_number;
  report::dualsense_advance(ds5, std::chrono::milliseconds(1));
  EXPECT_EQ(ds5.seq_number, static_cast<std::uint8_t>(seq + 1));
  EXPECT_EQ(ds5.sensor_timestamp, 3000u);
}

TEST(HidMaestroReport, PacksSwitchProBody) {
  report::switch_pro_state_t imu;
  std::array<std::uint8_t, report::SWITCH_PRO_BODY_SIZE> body {};

  report::state_t state {};
  state.button_flags = report::button::a | report::button::y | report::button::right_stick | report::button::dpad_up | report::button::dpad_right | report::button::back | report::button::misc;
  state.lt = 1;
  state.rs_x = -32768;
  report::pack_switch_pro_body(state, imu, body);
  EXPECT_EQ(body[0], 0);
  EXPECT_EQ(body[2], 0x04 | 0x02);  // south -> Switch B, north -> Switch X
  EXPECT_EQ(body[3], 0x01 | 0x04 | 0x20);  // minus, right stick, capture
  EXPECT_EQ(body[4], 0x02 | 0x04 | 0x80);  // up, right, ZL
  const unsigned lx = body[5] | ((body[6] & 0x0F) << 8);
  const unsigned ly = (body[6] >> 4) | (body[7] << 4);
  EXPECT_EQ(lx, 0x800u);
  EXPECT_EQ(ly, 0x800u);
  const unsigned rx = body[8] | ((body[9] & 0x0F) << 8);
  EXPECT_EQ(rx, 0x200u);  // fully left

  report::switch_pro_update_motion(imu, report::motion::accel, 0.0F, 9.80665F, 0.0F);
  EXPECT_EQ(imu.accel[2], 4096);  // SDL +Y maps onto wire +Z
  report::switch_pro_update_motion(imu, report::motion::gyro, 0.0F, 0.0F, 10.0F);
  EXPECT_EQ(imu.gyro[0], -143);  // -10 deg/s * 13371/936
  report::pack_switch_pro_body(state, imu, body);
  for (std::size_t frame = 0; frame < 3; ++frame) {
    const std::size_t o = 12 + frame * 12;
    EXPECT_EQ(static_cast<std::int16_t>(u16le(&body[o + 4])), 4096);
    EXPECT_EQ(static_cast<std::int16_t>(u16le(&body[o + 6])), -143);
  }
}

TEST(HidMaestroReport, DecodesXInputRumble) {
  const std::uint8_t wire[5] = {0x00, 0x08, 0x80, 0x40, 0x00};
  auto feedback = report::decode_output(report::output_kind_e::xbox_360, report::output_source::xinput, 0, wire);
  ASSERT_EQ(feedback.size(), 1u);
  EXPECT_EQ(feedback[0].kind, report::feedback_t::kind_e::rumble);
  EXPECT_EQ(feedback[0].lowfreq, 0x80 * 257);
  EXPECT_EQ(feedback[0].highfreq, 0x40 * 257);

  const std::uint8_t raw[4] = {0x34, 0x12, 0x78, 0x56};
  feedback = report::decode_output(report::output_kind_e::xbox_360, report::output_source::xinput, 0, raw);
  ASSERT_EQ(feedback.size(), 1u);
  EXPECT_EQ(feedback[0].lowfreq, 0x1234);
  EXPECT_EQ(feedback[0].highfreq, 0x5678);

  // HID output reports are not rumble for the Xbox 360 companion path
  feedback = report::decode_output(report::output_kind_e::xbox_360, report::output_source::hid_output, 0, wire);
  EXPECT_TRUE(feedback.empty());
}

TEST(HidMaestroReport, DecodesXboxOneHidRumble) {
  const std::uint8_t body[8] = {0x0F, 50, 100, 25, 0, 0xFF, 0, 0};
  auto feedback = report::decode_output(report::output_kind_e::xbox_one_hid, report::output_source::hid_output, 0x03, body);
  ASSERT_EQ(feedback.size(), 2u);
  EXPECT_EQ(feedback[0].kind, report::feedback_t::kind_e::rumble);
  EXPECT_EQ(feedback[0].lowfreq, 65535 * 25 / 100);
  EXPECT_EQ(feedback[0].highfreq, 0);
  EXPECT_EQ(feedback[1].kind, report::feedback_t::kind_e::rumble_triggers);
  EXPECT_EQ(feedback[1].left_trigger, 65535 * 50 / 100);
  EXPECT_EQ(feedback[1].right_trigger, 65535);

  const std::uint8_t motors_only[8] = {0x0C, 50, 100, 25, 10, 0xFF, 0, 0};
  feedback = report::decode_output(report::output_kind_e::xbox_one_hid, report::output_source::hid_output, 0x03, motors_only);
  ASSERT_EQ(feedback.size(), 1u);
  EXPECT_EQ(feedback[0].highfreq, 65535 * 10 / 100);

  feedback = report::decode_output(report::output_kind_e::xbox_one_hid, report::output_source::hid_output, 0x05, body);
  EXPECT_TRUE(feedback.empty());
}

TEST(HidMaestroReport, DecodesSonyOutputReports) {
  const std::uint8_t ds4[31] = {0x07, 0, 0, 0x10, 0x20, 1, 2, 3};
  auto feedback = report::decode_output(report::output_kind_e::ds4, report::output_source::hid_output, 0x05, ds4);
  ASSERT_EQ(feedback.size(), 2u);
  EXPECT_EQ(feedback[0].lowfreq, 0x20 * 257);
  EXPECT_EQ(feedback[0].highfreq, 0x10 * 257);
  EXPECT_EQ(feedback[1].kind, report::feedback_t::kind_e::rgb_led);
  EXPECT_EQ(feedback[1].r, 1);
  EXPECT_EQ(feedback[1].g, 2);
  EXPECT_EQ(feedback[1].b, 3);

  std::uint8_t ds5[47] = {};
  ds5[0] = 0x0F;
  ds5[1] = 0x14;
  ds5[2] = 0x11;
  ds5[3] = 0x22;
  ds5[10] = 0x21;
  ds5[11] = 0x01;
  ds5[21] = 0x26;
  ds5[22] = 0x02;
  ds5[43] = 0x04;
  ds5[44] = 9;
  ds5[45] = 8;
  ds5[46] = 7;
  feedback = report::decode_output(report::output_kind_e::dualsense, report::output_source::hid_output, 0x02, ds5);
  ASSERT_EQ(feedback.size(), 4u);
  EXPECT_EQ(feedback[0].kind, report::feedback_t::kind_e::rumble);
  EXPECT_EQ(feedback[0].lowfreq, 0x22 * 257);
  EXPECT_EQ(feedback[0].highfreq, 0x11 * 257);
  EXPECT_EQ(feedback[1].kind, report::feedback_t::kind_e::adaptive_triggers);
  EXPECT_EQ(feedback[1].event_flags, 0x0C);
  EXPECT_EQ(feedback[1].type_right, 0x21);
  EXPECT_EQ(feedback[1].right[0], 0x01);
  EXPECT_EQ(feedback[1].type_left, 0x26);
  EXPECT_EQ(feedback[1].left[0], 0x02);
  EXPECT_EQ(feedback[2].kind, report::feedback_t::kind_e::rgb_led);
  EXPECT_EQ(feedback[2].r, 9);
  EXPECT_EQ(feedback[3].kind, report::feedback_t::kind_e::player_leds);
  EXPECT_EQ(feedback[3].solid, 0x04);

  // Feature reports never carry rumble
  feedback = report::decode_output(report::output_kind_e::dualsense, report::output_source::hid_feature, 0x02, ds5);
  EXPECT_TRUE(feedback.empty());
}

TEST(HidMaestroReport, DecodesSwitchRumbleBlocks) {
  // Counter, then two HD rumble blocks; the SDK's neutral block is 00 01 40 40
  const std::uint8_t body[9] = {0x00, 0x00, 0xC8, 0x40, 0x72, 0x00, 0x01, 0x40, 0x40};
  auto feedback = report::decode_output(report::output_kind_e::switch_pro, report::output_source::hid_output, 0x10, body);
  ASSERT_EQ(feedback.size(), 1u);
  EXPECT_EQ(feedback[0].lowfreq, 255 * 257);
  EXPECT_EQ(feedback[0].highfreq, 0);

  feedback = report::decode_output(report::output_kind_e::none, report::output_source::hid_output, 0x10, body);
  EXPECT_TRUE(feedback.empty());
}
