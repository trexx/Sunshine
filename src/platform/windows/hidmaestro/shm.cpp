/**
 * @file src/platform/windows/hidmaestro/shm.cpp
 * @brief Direct access to the HIDMaestro driver's per-controller shared-memory sections.
 */
#ifndef DOXYGEN
  #define WINVER 0x0A00
  #define _WIN32_WINNT 0x0A00
#endif

// platform includes
#include <Windows.h>
#include <sddl.h>

// standard includes
#include <atomic>
#include <cstring>

// local includes
#include "shm.h"
#include "src/logging.h"
#include "src/utility.h"

namespace platf::hidmaestro::shm {
  using namespace std::literals;

  namespace {
    /**
     * @brief DACL the HIDMaestro SDK applies to its sections and events; used when we must create an event ourselves.
     */
    constexpr wchar_t SDDL[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GR;;;WD)";

    /**
     * @brief Open a named auto-reset event, creating it with the SDK's DACL when it does not exist yet.
     *
     * @param name Event name.
     * @return Event handle, or empty on failure.
     */
    unique_handle_t open_or_create_event(const std::wstring &name) {
      unique_handle_t event {OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name.c_str())};
      if (event) {
        return event;
      }

      PSECURITY_DESCRIPTOR descriptor = nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(SDDL, SDDL_REVISION_1, &descriptor, nullptr)) {
        return {};
      }
      auto free_descriptor = util::fail_guard([descriptor]() {
        LocalFree(descriptor);
      });
      SECURITY_ATTRIBUTES attributes = {};
      attributes.nLength = sizeof(attributes);
      attributes.lpSecurityDescriptor = descriptor;
      attributes.bInheritHandle = FALSE;
      event.reset(CreateEventW(&attributes, FALSE, FALSE, name.c_str()));
      return event;
    }

    /**
     * @brief Map a whole named section and verify it is at least `minimum_size` bytes.
     *
     * @param name Section name.
     * @param access `FILE_MAP_*` access mask.
     * @param minimum_size Smallest acceptable section size.
     * @param[out] mapping Receives the mapping handle.
     * @return Mapped view, or `nullptr` on failure.
     */
    void *map_section(const std::wstring &name, DWORD access, std::size_t minimum_size, unique_handle_t &mapping) {
      mapping.reset(OpenFileMappingW(access, FALSE, name.c_str()));
      if (!mapping) {
        return nullptr;
      }
      void *view = MapViewOfFile(mapping.get(), access, 0, 0, 0);
      if (!view) {
        mapping.reset();
        return nullptr;
      }
      MEMORY_BASIC_INFORMATION info = {};
      if (VirtualQuery(view, &info, sizeof(info)) == 0 || info.RegionSize < minimum_size) {
        UnmapViewOfFile(view);
        mapping.reset();
        return nullptr;
      }
      return view;
    }
  }  // namespace

  std::wstring object_name(std::wstring_view prefix, std::uint32_t index) {
    return std::wstring(prefix) + std::to_wstring(index);
  }

  input_channel_t::~input_channel_t() {
    close();
  }

  bool input_channel_t::open(std::uint32_t index, bool with_companion) {
    close();

    const auto section_name = object_name(L"Global\\HIDMaestroInput", index);
    view = static_cast<std::uint8_t *>(map_section(section_name, FILE_MAP_READ | FILE_MAP_WRITE, input_layout::SECTION_SIZE, mapping));
    if (!view) {
      BOOST_LOG(error) << "Couldn't open HIDMaestro input section for controller "sv << index << " ["sv << util::hex(GetLastError()).to_string_view() << ']';
      return false;
    }

    input_event = open_or_create_event(object_name(L"Global\\HIDMaestroInputEvent", index));
    if (!input_event) {
      BOOST_LOG(error) << "Couldn't open HIDMaestro input event for controller "sv << index << " ["sv << util::hex(GetLastError()).to_string_view() << ']';
      close();
      return false;
    }

    if (with_companion) {
      companion_event = open_or_create_event(object_name(L"Global\\HIDMaestroCompanionInputEvent", index));
      if (!companion_event) {
        BOOST_LOG(warning) << "Couldn't open HIDMaestro companion event for controller "sv << index << "; XInput state may lag"sv;
      }
    }

    seq_no = *reinterpret_cast<volatile std::uint32_t *>(view + input_layout::SEQ_NO);
    return true;
  }

  void input_channel_t::close() {
    if (view) {
      UnmapViewOfFile(view);
      view = nullptr;
    }
    mapping.reset();
    input_event.reset();
    companion_event.reset();
    seq_no = 0;
  }

  bool input_channel_t::write_frame(std::span<const std::uint8_t> body, const std::uint8_t *gip) {
    if (!view || body.size() > input_layout::DATA_CAPACITY) {
      return false;
    }

    auto *seq = reinterpret_cast<volatile LONG *>(view + input_layout::SEQ_NO);

    // Odd/even seqlock: mark the write in progress, publish the payload, then mark it complete
    std::uint32_t current = static_cast<std::uint32_t>(*seq);
    if (current & 1U) {
      current++;
    }
    const std::uint32_t pending = current + 1;
    InterlockedExchange(seq, static_cast<LONG>(pending));
    std::atomic_thread_fence(std::memory_order_seq_cst);

    *reinterpret_cast<std::uint32_t *>(view + input_layout::DATA_SIZE) = static_cast<std::uint32_t>(body.size());
    std::memcpy(view + input_layout::DATA, body.data(), body.size());
    if (gip) {
      std::memcpy(view + input_layout::GIP_DATA, gip, input_layout::GIP_SIZE);
    }
    // Always clear the vendor-blob path so a stale extended report from a previous writer is never emitted
    *reinterpret_cast<std::uint32_t *>(view + input_layout::EXTENDED_SIZE) = 0;

    std::atomic_thread_fence(std::memory_order_seq_cst);
    seq_no = pending + 1;
    InterlockedExchange(seq, static_cast<LONG>(seq_no));

    SetEvent(input_event.get());
    if (gip && companion_event) {
      SetEvent(companion_event.get());
    }
    return true;
  }

  output_channel_t::~output_channel_t() {
    close();
  }

  bool output_channel_t::open(std::uint32_t index) {
    close();

    const auto section_name = object_name(L"Global\\HIDMaestroOutput", index);
    view = static_cast<const std::uint8_t *>(map_section(section_name, FILE_MAP_READ, output_layout::SECTION_SIZE, mapping));
    if (!view) {
      BOOST_LOG(warning) << "Couldn't open HIDMaestro output section for controller "sv << index << " ["sv << util::hex(GetLastError()).to_string_view() << "]; rumble feedback unavailable"sv;
      return false;
    }

    output_event = open_or_create_event(object_name(L"Global\\HIDMaestroOutputEvent", index));
    if (!output_event) {
      BOOST_LOG(debug) << "HIDMaestro output event unavailable for controller "sv << index << "; polling only"sv;
    }

    // Skip anything published before we attached
    last_seen = *reinterpret_cast<const volatile std::uint32_t *>(view + output_layout::HEAD);
    return true;
  }

  void output_channel_t::close() {
    if (view) {
      UnmapViewOfFile(const_cast<std::uint8_t *>(view));
      view = nullptr;
    }
    mapping.reset();
    output_event.reset();
    last_seen = 0;
  }

  std::size_t output_channel_t::poll(const std::function<void(const output_packet_t &)> &callback) {
    if (!view) {
      return 0;
    }

    const std::uint32_t head = *reinterpret_cast<const volatile std::uint32_t *>(view + output_layout::HEAD);
    if (head == last_seen) {
      return 0;
    }
    if (head - last_seen > output_layout::SLOT_COUNT) {
      // We fell behind by more than the ring holds; the oldest packets are gone
      last_seen = head - output_layout::SLOT_COUNT;
    }

    std::size_t delivered = 0;
    std::uint8_t copy[output_layout::SLOT_DATA_CAPACITY];
    for (std::uint32_t seq = last_seen + 1; seq != head + 1; ++seq) {
      const std::uint8_t *slot = view + output_layout::SLOTS + ((seq - 1) % output_layout::SLOT_COUNT) * output_layout::SLOT_SIZE;
      const auto *slot_seq = reinterpret_cast<const volatile std::uint32_t *>(slot + output_layout::SLOT_SEQ_NO);

      const std::uint32_t observed = *slot_seq;
      if (static_cast<std::int32_t>(observed - seq) < 0) {
        // Head was published before this slot's SeqNo landed; retry on the next poll
        last_seen = seq - 1;
        return delivered;
      }
      if (observed != seq) {
        // Overwritten by a newer packet already
        continue;
      }

      const std::uint8_t source = slot[output_layout::SLOT_SOURCE];
      const std::uint8_t report_id = slot[output_layout::SLOT_REPORT_ID];
      std::uint16_t size = 0;
      std::memcpy(&size, slot + output_layout::SLOT_DATA_SIZE, sizeof(size));
      if (size > output_layout::SLOT_DATA_CAPACITY) {
        size = output_layout::SLOT_DATA_CAPACITY;
      }
      std::memcpy(copy, slot + output_layout::SLOT_DATA, size);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (*slot_seq != seq) {
        // Torn read; the slot was recycled while we copied it
        continue;
      }

      callback(output_packet_t {source, report_id, std::span<const std::uint8_t>(copy, size)});
      delivered++;
    }

    last_seen = head;
    return delivered;
  }

}  // namespace platf::hidmaestro::shm
