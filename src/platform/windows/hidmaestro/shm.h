/**
 * @file src/platform/windows/hidmaestro/shm.h
 * @brief Direct access to the HIDMaestro driver's per-controller shared-memory sections.
 *
 * The HIDMaestro SDK (running in the broker) creates these sections when a controller is
 * created. Sunshine opens them by name and performs the input writes and output reads
 * itself, so no per-frame traffic crosses the broker pipe. Layouts mirror
 * `driver/driver.h` in the HIDMaestro repository (`HIDMAESTRO_SHARED_INPUT`,
 * `HIDMAESTRO_SHARED_OUTPUT`).
 */
#pragma once

// platform includes
#include <Windows.h>

// standard includes
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

// local includes
#include "src/platform/windows/hidmaestro/broker_client.h"

namespace platf::hidmaestro::shm {

  /**
   * @brief Offsets and sizes of the input section (`Global\HIDMaestroInput<N>`).
   */
  namespace input_layout {
    constexpr std::size_t SEQ_NO = 0;  ///< `volatile ULONG SeqNo`.
    constexpr std::size_t DATA_SIZE = 4;  ///< `ULONG DataSize`.
    constexpr std::size_t DATA = 8;  ///< `UCHAR Data[256]`.
    constexpr std::size_t DATA_CAPACITY = 256;  ///< Capacity of `Data`.
    constexpr std::size_t GIP_DATA = DATA + DATA_CAPACITY;  ///< `UCHAR GipData[14]` at 264.
    constexpr std::size_t GIP_SIZE = 14;  ///< Size of `GipData`.
    constexpr std::size_t EXTENDED_SIZE = GIP_DATA + GIP_SIZE;  ///< `ULONG ExtendedReportSize` at 278.
    constexpr std::size_t EXTENDED_DATA = EXTENDED_SIZE + 4;  ///< `UCHAR ExtendedReportData[80]` at 282.
    constexpr std::size_t EXTENDED_CAPACITY = 80;  ///< Capacity of `ExtendedReportData`.
    constexpr std::size_t SECTION_SIZE = EXTENDED_DATA + EXTENDED_CAPACITY;  ///< 362 bytes.
  }  // namespace input_layout

  /**
   * @brief Offsets and sizes of the output ring (`Global\HIDMaestroOutput<N>`).
   */
  namespace output_layout {
    constexpr std::size_t HEAD = 0;  ///< `volatile ULONG Head`.
    constexpr std::size_t SLOTS = 8;  ///< First slot offset (after `Head` and `_Reserved`).
    constexpr std::size_t SLOT_COUNT = 64;  ///< Ring capacity.
    constexpr std::size_t SLOT_SEQ_NO = 0;  ///< `volatile ULONG SeqNo` within a slot.
    constexpr std::size_t SLOT_SOURCE = 4;  ///< `UCHAR Source`.
    constexpr std::size_t SLOT_REPORT_ID = 5;  ///< `UCHAR ReportId`.
    constexpr std::size_t SLOT_DATA_SIZE = 6;  ///< `USHORT DataSize`.
    constexpr std::size_t SLOT_DATA = 8;  ///< `UCHAR Data[256]`.
    constexpr std::size_t SLOT_DATA_CAPACITY = 256;  ///< Capacity of a slot's `Data`.
    constexpr std::size_t SLOT_SIZE = SLOT_DATA + SLOT_DATA_CAPACITY;  ///< 264 bytes per slot.
    constexpr std::size_t SECTION_SIZE = SLOTS + SLOT_COUNT * SLOT_SIZE;  ///< Whole ring, 16904 bytes.
  }  // namespace output_layout

  /**
   * @brief Build the name of a per-controller kernel object.
   *
   * @param prefix Object prefix, e.g. `Global\HIDMaestroInput`.
   * @param index Controller index.
   * @return Object name with the index appended.
   */
  std::wstring object_name(std::wstring_view prefix, std::uint32_t index);

  /**
   * @brief Writer side of the input section for one controller.
   */
  class input_channel_t {
  public:
    input_channel_t() = default;
    ~input_channel_t();

    input_channel_t(const input_channel_t &) = delete;
    input_channel_t &operator=(const input_channel_t &) = delete;

    /**
     * @brief Open the input section and its doorbell events.
     *
     * @param index Controller index.
     * @param with_companion Also open the XUSB companion doorbell (Xbox 360 profiles).
     * @return `true` on success.
     */
    bool open(std::uint32_t index, bool with_companion);

    /**
     * @brief Unmap and close everything.
     */
    void close();

    /**
     * @brief Check whether the channel is open.
     *
     * @return `true` when the section is mapped.
     */
    bool is_open() const {
      return view != nullptr;
    }

    /**
     * @brief Publish one input frame using the driver's odd/even seqlock and ring the doorbells.
     *
     * @param body Report body (no Report ID byte), at most 256 bytes.
     * @param gip Optional 14-byte XUSB companion buffer.
     * @return `true` when the frame was written.
     */
    bool write_frame(std::span<const std::uint8_t> body, const std::uint8_t *gip);

  private:
    unique_handle_t mapping;  ///< File mapping handle.
    unique_handle_t input_event;  ///< `Global\HIDMaestroInputEvent<N>`.
    unique_handle_t companion_event;  ///< `Global\HIDMaestroCompanionInputEvent<N>`, optional.
    std::uint8_t *view = nullptr;  ///< Mapped view.
    std::uint32_t seq_no = 0;  ///< Last published (even) sequence number.
  };

  /**
   * @brief One packet read from the output ring.
   */
  struct output_packet_t {
    std::uint8_t source;  ///< `HIDMAESTRO_OUTPUT_SOURCE_*`.
    std::uint8_t report_id;  ///< HID Report ID (0 when none).
    std::span<const std::uint8_t> data;  ///< Payload bytes.
  };

  /**
   * @brief Reader side of the output ring for one controller.
   */
  class output_channel_t {
  public:
    output_channel_t() = default;
    ~output_channel_t();

    output_channel_t(const output_channel_t &) = delete;
    output_channel_t &operator=(const output_channel_t &) = delete;

    /**
     * @brief Open the output ring and its doorbell event.
     *
     * Packets already in the ring at open time are skipped.
     *
     * @param index Controller index.
     * @return `true` on success.
     */
    bool open(std::uint32_t index);

    /**
     * @brief Unmap and close everything.
     */
    void close();

    /**
     * @brief Check whether the channel is open.
     *
     * @return `true` when the section is mapped.
     */
    bool is_open() const {
      return view != nullptr;
    }

    /**
     * @brief Doorbell event signaled by the driver after each publish (auto-reset).
     *
     * @return Event handle, or `nullptr` when unavailable.
     */
    HANDLE event() const {
      return output_event.get();
    }

    /**
     * @brief Deliver every packet published since the previous call.
     *
     * @param callback Invoked once per packet, in publish order.
     * @return Number of packets delivered.
     */
    std::size_t poll(const std::function<void(const output_packet_t &)> &callback);

  private:
    unique_handle_t mapping;  ///< File mapping handle.
    unique_handle_t output_event;  ///< `Global\HIDMaestroOutputEvent<N>`.
    const std::uint8_t *view = nullptr;  ///< Mapped read-only view.
    std::uint32_t last_seen = 0;  ///< Last consumed `Head` value.
  };

}  // namespace platf::hidmaestro::shm
