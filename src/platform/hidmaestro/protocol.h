/**
 * @file src/platform/hidmaestro/protocol.h
 * @brief Wire protocol shared between Sunshine and the HIDMaestro broker process.
 *
 * The broker (`tools/hidmaestro-broker`, C#) hosts the HIDMaestro SDK for controller
 * lifecycle only. Sunshine talks to it over a message-mode named pipe using the
 * fixed-size, little-endian, byte-packed structures declared here. The C# side mirrors
 * these layouts with `StructLayout(LayoutKind.Sequential, Pack = 1)`, so every change
 * here must be paired with a change in `tools/hidmaestro-broker/Protocol.cs` and a
 * bump of `PROTOCOL_VERSION`.
 *
 * This header is platform-neutral so the packing plan can be unit-tested on any OS.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>

namespace platf::hidmaestro::proto {

  /**
   * @brief Protocol version understood by this build. Both peers must agree.
   */
  constexpr std::uint32_t PROTOCOL_VERSION = 1;

  /**
   * @brief Upper bound on any single pipe message, in bytes.
   */
  constexpr std::size_t MAX_MESSAGE_SIZE = 8192;

  /**
   * @brief Maximum number of buttons a packing plan can describe.
   */
  constexpr std::size_t MAX_BUTTONS = 32;

  /**
   * @brief Number of Sunshine button roles mapped by a packing plan.
   */
  constexpr std::size_t ROLE_COUNT = 16;

  /**
   * @brief Sentinel meaning "this role is not present on the device".
   */
  constexpr std::uint8_t ROLE_UNMAPPED = 0xFF;

  /**
   * @brief Request kinds carried in `header_t::type`.
   */
  enum class request_type_e : std::uint32_t {
    hello = 1,  ///< Version handshake and driver status probe.
    ensure_driver = 2,  ///< Install the HIDMaestro driver if it is missing (idempotent).
    create_controller = 3,  ///< Create a virtual controller pinned to a slot index.
    destroy_controller = 4,  ///< Dispose the virtual controller at a slot index.
    status = 5,  ///< Report driver and controller status.
    shutdown = 6,  ///< Dispose everything and exit the broker.
  };

  /**
   * @brief Result codes carried in `response_header_t::status`.
   */
  enum class status_e : std::uint32_t {
    ok = 0,  ///< Success.
    invalid_request = 1,  ///< Malformed message or unknown type.
    unsupported_version = 2,  ///< Peer protocol version mismatch.
    unknown_profile = 3,  ///< Requested HIDMaestro profile id does not exist.
    driver_install_failed = 4,  ///< `InstallDriver()` threw; see `error`.
    controller_create_failed = 5,  ///< `CreateControllerAt()` threw; see `error`.
    index_in_use = 6,  ///< A live controller already occupies the slot.
    not_found = 7,  ///< No live controller at the slot.
    not_elevated = 8,  ///< Broker is not running with administrator rights.
    internal_error = 9,  ///< Unexpected failure; see `error`.
  };

  /**
   * @brief How Sunshine must build the input report body for a controller.
   */
  enum class pack_mode_e : std::uint8_t {
    hid_generic = 0,  ///< Bitfield writer driven entirely by `packing_plan_t` fields.
    sony_ds4 = 1,  ///< Fixed DualShock 4 USB report 0x01 body (63 bytes).
    sony_dualsense = 2,  ///< Fixed DualSense USB report 0x01 body (63 bytes).
    switch_pro = 3,  ///< Intermediate Switch Pro body consumed by the driver's protocol responder.
  };

  /**
   * @brief Sunshine button roles indexed into `packing_plan_t::role_to_button`.
   */
  enum class button_role_e : std::uint8_t {
    a = 0,  ///< South face button (A / Cross / B on Nintendo layouts as reported by Moonlight).
    b = 1,  ///< East face button.
    x = 2,  ///< West face button.
    y = 3,  ///< North face button.
    left_bumper = 4,  ///< Left shoulder button.
    right_bumper = 5,  ///< Right shoulder button.
    back = 6,  ///< Back / View / Share / Minus.
    start = 7,  ///< Start / Menu / Options / Plus.
    left_stick = 8,  ///< Left stick click.
    right_stick = 9,  ///< Right stick click.
    guide = 10,  ///< Guide / PS / Home.
    misc = 11,  ///< Share (Xbox Series) / Mute (DualSense) / Capture (Switch).
    paddle1 = 12,  ///< Rear paddle 1.
    paddle2 = 13,  ///< Rear paddle 2.
    paddle3 = 14,  ///< Rear paddle 3.
    paddle4 = 15,  ///< Rear paddle 4.
  };

#pragma pack(push, 1)

  /**
   * @brief Header that starts every request.
   */
  struct header_t {
    std::uint32_t version;  ///< Must equal `PROTOCOL_VERSION`.
    std::uint32_t size;  ///< Total message size in bytes, including this header.
    std::uint32_t type;  ///< `request_type_e` value.
    std::uint32_t reserved;  ///< Reserved; zero.
  };

  /**
   * @brief Header that starts every response.
   */
  struct response_header_t {
    std::uint32_t version;  ///< Equals `PROTOCOL_VERSION`.
    std::uint32_t size;  ///< Total message size in bytes, including this header.
    std::uint32_t status;  ///< `status_e` value.
    std::uint32_t reserved;  ///< Reserved; zero.
  };

  /**
   * @brief One HID input field (an axis, hat or button) inside the report body.
   *
   * Bit offsets are relative to the first byte of the report body, i.e. they exclude
   * the Report ID byte that the driver prepends itself.
   */
  struct field_t {
    std::uint16_t bit_offset;  ///< Offset of the least-significant bit within the body.
    std::uint8_t bit_size;  ///< Width in bits (1..32).
    std::uint8_t present;  ///< Non-zero when the field exists in this descriptor.
    std::int32_t logical_min;  ///< HID logical minimum.
    std::int32_t logical_max;  ///< HID logical maximum.
  };

  /**
   * @brief Everything Sunshine needs to pack input for one created controller.
   */
  struct packing_plan_t {
    std::uint8_t mode;  ///< `pack_mode_e` value.
    std::uint8_t report_id;  ///< Report ID the driver prepends (informational; 0 if none).
    std::uint16_t data_size;  ///< Body size in bytes to publish as `DataSize`.
    std::uint8_t packs_gip;  ///< Non-zero when the 14-byte XUSB companion buffer must be filled.
    std::uint8_t hat_positions;  ///< Number of hat positions (4 or 8); 0 when no hat.
    std::uint8_t hat_null;  ///< Hat value meaning "centered".
    std::uint8_t y_axis_hid_down;  ///< Non-zero when stick Y grows downward (HID convention).
    field_t lx;  ///< Left stick X.
    field_t ly;  ///< Left stick Y.
    field_t rx;  ///< Right stick X.
    field_t ry;  ///< Right stick Y.
    field_t lt;  ///< Left trigger.
    field_t rt;  ///< Right trigger.
    field_t combined_z;  ///< Combined trigger axis (Xbox 360 DirectInput Z), if declared.
    field_t hat;  ///< Hat switch.
    std::uint8_t button_count;  ///< Number of valid entries in `buttons`.
    std::uint8_t reserved[3];  ///< Reserved; zero.
    field_t buttons[MAX_BUTTONS];  ///< Button fields in descriptor order.
    std::uint8_t role_to_button[ROLE_COUNT];  ///< `button_role_e` → index into `buttons`, or `ROLE_UNMAPPED`.
  };

  /**
   * @brief `hello` request.
   */
  struct hello_request_t {
    header_t header;  ///< Message header.
    std::uint32_t protocol_version;  ///< Caller's `PROTOCOL_VERSION`.
  };

  /**
   * @brief `hello` response.
   */
  struct hello_response_t {
    response_header_t header;  ///< Message header.
    std::uint32_t broker_version;  ///< Broker build number.
    char sdk_version[32];  ///< HIDMaestro.Core assembly version, NUL-terminated.
    std::uint8_t driver_installed;  ///< Non-zero when the driver manifest marker is present.
    std::uint8_t elevated;  ///< Non-zero when the broker runs with administrator rights.
    std::uint8_t reserved[2];  ///< Reserved; zero.
  };

  /**
   * @brief `ensure_driver` request.
   */
  struct ensure_driver_request_t {
    header_t header;  ///< Message header.
  };

  /**
   * @brief `ensure_driver` response.
   */
  struct ensure_driver_response_t {
    response_header_t header;  ///< Message header.
    char error[256];  ///< Human-readable failure detail, NUL-terminated (empty on success).
  };

  /**
   * @brief `create_controller` request.
   */
  struct create_controller_request_t {
    header_t header;  ///< Message header.
    std::uint32_t index;  ///< HIDMaestro controller index (== Sunshine gamepad slot).
    char profile_id[64];  ///< HIDMaestro profile id, e.g. `xbox-360-wired`, NUL-terminated.
    char identity_key[64];  ///< Stable identity key for device paths, NUL-terminated.
  };

  /**
   * @brief `create_controller` response.
   */
  struct create_controller_response_t {
    response_header_t header;  ///< Message header.
    packing_plan_t plan;  ///< Packing plan for the created controller.
    char error[256];  ///< Human-readable failure detail, NUL-terminated (empty on success).
  };

  /**
   * @brief `destroy_controller` request.
   */
  struct destroy_controller_request_t {
    header_t header;  ///< Message header.
    std::uint32_t index;  ///< HIDMaestro controller index.
  };

  /**
   * @brief Response with no payload beyond the status.
   */
  struct simple_response_t {
    response_header_t header;  ///< Message header.
  };

  /**
   * @brief `status` request.
   */
  struct status_request_t {
    header_t header;  ///< Message header.
  };

  /**
   * @brief `status` response.
   */
  struct status_response_t {
    response_header_t header;  ///< Message header.
    std::uint8_t driver_installed;  ///< Non-zero when the driver manifest marker is present.
    std::uint8_t reserved[3];  ///< Reserved; zero.
    std::uint32_t live_controllers;  ///< Number of controllers currently hosted.
    char sdk_version[32];  ///< HIDMaestro.Core assembly version, NUL-terminated.
  };

  /**
   * @brief `shutdown` request.
   */
  struct shutdown_request_t {
    header_t header;  ///< Message header.
  };

#pragma pack(pop)

  static_assert(sizeof(header_t) == 16);
  static_assert(sizeof(response_header_t) == 16);
  static_assert(sizeof(field_t) == 12);
  static_assert(sizeof(packing_plan_t) == 8 + 8 * sizeof(field_t) + 4 + MAX_BUTTONS * sizeof(field_t) + ROLE_COUNT);
  static_assert(sizeof(packing_plan_t) == 508);
  static_assert(sizeof(hello_request_t) == 20);
  static_assert(sizeof(hello_response_t) == 56);
  static_assert(sizeof(ensure_driver_request_t) == 16);
  static_assert(sizeof(ensure_driver_response_t) == 272);
  static_assert(sizeof(create_controller_request_t) == 148);
  static_assert(sizeof(create_controller_response_t) == 16 + 508 + 256);
  static_assert(sizeof(destroy_controller_request_t) == 20);
  static_assert(sizeof(simple_response_t) == 16);
  static_assert(sizeof(status_request_t) == 16);
  static_assert(sizeof(status_response_t) == 56);
  static_assert(sizeof(shutdown_request_t) == 16);
  static_assert(sizeof(create_controller_response_t) <= MAX_MESSAGE_SIZE);

  /**
   * @brief Build a request header for a fixed-size request type.
   *
   * @tparam T Request structure type.
   * @param type Request kind.
   * @return Populated header.
   */
  template<typename T>
  constexpr header_t make_header(request_type_e type) {
    return header_t {
      .version = PROTOCOL_VERSION,
      .size = static_cast<std::uint32_t>(sizeof(T)),
      .type = static_cast<std::uint32_t>(type),
      .reserved = 0,
    };
  }

}  // namespace platf::hidmaestro::proto
