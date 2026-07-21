/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

Unit tests for QueueAction() argument serialisation, compiled into the Core when UNIT_TESTS is enabled and invoked
via the public UnitTests() function.  The primary concern is the relocation of self-referential string pointers
when the argument buffer is duplicated into the message queue.

*********************************************************************************************************************/

#include <cstring>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "../defs.h"

static_assert(glSpanFieldOps<int8_t>.Size IS sizeof(std::span<int8_t>));
static_assert(glSpanFieldOps<const uint16_t>.Alignment IS alignof(std::span<const uint16_t>));
static_assert(glSpanFieldOps<const RGB8>.ElementSize IS sizeof(RGB8));

static SpanFieldValue read_oversized_span(CPTR Storage) noexcept
{
   return { Storage, std::numeric_limits<size_t>::max() };
}

static SpanFieldValue read_null_nonempty_span(CPTR) noexcept
{
   return { nullptr, 2 };
}

static void ignore_span_binding(APTR, CPTR, size_t) noexcept
{
}

static constexpr SpanFieldOps glOversizedSpanOps = {
   sizeof(std::span<const uint64_t>), alignof(std::span<const uint64_t>), sizeof(uint64_t),
   read_oversized_span, ignore_span_binding, ignore_span_binding
};

static constexpr SpanFieldOps glNullNonemptySpanOps = {
   sizeof(std::span<const int8_t>), alignof(std::span<const int8_t>), sizeof(int8_t),
   read_null_nonempty_span, ignore_span_binding, ignore_span_binding
};

//********************************************************************************************************************
// Validate typed access, construction, alignment, descriptor cross-checks and checked byte extents for embedded spans.

static bool span_field_metadata_check(kt::Log &Log)
{
   const FunctionField byte_field = {
      "Bytes", FDF_SPAN|FD_MUTABLE|FD_PTR, &glSpanFieldOps<int8_t>
   };
   const FunctionField word_field = {
      "Words", FDF_SPAN|FD_WORD, &glSpanFieldOps<const uint16_t>
   };
   const FunctionField struct_field = {
      "RGB8:Colours", FDF_SPAN|FD_PTR|FD_STRUCT, &glSpanFieldOps<const RGB8>
   };

   size_t aligned_offset, end_offset;
   if (span_field_layout(byte_field, 1, &aligned_offset, &end_offset) != ERR::Okay) {
      Log.warning("Failed to calculate byte-span layout.");
      return false;
   }
   if ((aligned_offset % alignof(std::span<int8_t>)) or
       (end_offset != aligned_offset + sizeof(std::span<int8_t>))) {
      Log.warning("Byte-span layout is incorrectly aligned or sized.");
      return false;
   }

   std::array<uint16_t, 3> words = { 10, 20, 30 };
   const std::span<const uint16_t> word_span(words);
   SpanFieldValue value;
   size_t byte_size;
   if (span_field_value(word_field, &word_span, &value, &byte_size) != ERR::Okay) {
      Log.warning("Failed to read a word span through its metadata.");
      return false;
   }
   if ((value.Data != words.data()) or (value.Extent != words.size()) or (byte_size != sizeof(words))) {
      Log.warning("Word-span metadata returned the wrong value or byte size.");
      return false;
   }

   std::array<RGB8, 2> colours = {};
   const std::span<const RGB8> colour_span(colours);
   if ((span_field_value(struct_field, &colour_span, &value, &byte_size) != ERR::Okay) or
       (byte_size != sizeof(colours))) {
      Log.warning("Named structure span metadata did not match the registered structure size.");
      return false;
   }

   std::array<int8_t, 4> first = { 1, 2, 3, 4 };
   std::array<int8_t, 2> second = { 5, 6 };
   alignas(std::span<int8_t>) std::array<std::byte, sizeof(std::span<int8_t>)> storage;
   byte_field.SpanOps->Construct(storage.data(), first.data(), first.size());
   auto constructed = byte_field.SpanOps->Read(storage.data());
   if ((constructed.Data != first.data()) or (constructed.Extent != first.size())) {
      Log.warning("Span metadata construction failed.");
      return false;
   }

   byte_field.SpanOps->Rebind(storage.data(), second.data(), second.size());
   const auto rebound = byte_field.SpanOps->Read(storage.data());
   if ((rebound.Data != second.data()) or (rebound.Extent != second.size())) {
      Log.warning("Span metadata rebinding failed.");
      return false;
   }

   const FunctionField missing_ops = { "Missing", FDF_SPAN|FD_PTR };
   const FunctionField wrong_element = { "Wrong", FDF_SPAN|FD_WORD, &glSpanFieldOps<const int8_t> };
   const FunctionField unknown_struct = {
      "UnknownSpanType:Values", FDF_SPAN|FD_PTR|FD_STRUCT, &glSpanFieldOps<const RGB8>
   };
   if ((span_field_layout(missing_ops, 0, nullptr, nullptr) != ERR::InvalidData) or
       (span_field_layout(wrong_element, 0, nullptr, nullptr) != ERR::InvalidData) or
       (span_field_layout(unknown_struct, 0, nullptr, nullptr) != ERR::InvalidData)) {
      Log.warning("Malformed span metadata was not rejected.");
      return false;
   }

   struct SpanArguments {
      std::span<int8_t> Bytes;
      int Tail;
   } span_args = { first, 42 };
   const FunctionField valid_fields[] = {
      byte_field,
      { "Tail", FD_INT },
      { nullptr, 0 }
   };
   const FunctionField invalid_fields[] = {
      missing_ops,
      { "Tail", FD_INT },
      { nullptr, 0 }
   };
   std::vector<int8_t> copied_args;
   if (copy_args(valid_fields, sizeof(span_args), (int8_t *)&span_args, copied_args) != ERR::Okay) {
      Log.warning("A valid span record could not be copied.");
      return false;
   }
   if (copy_args(invalid_fields, sizeof(span_args), (int8_t *)&span_args, copied_args) != ERR::InvalidData) {
      Log.warning("The argument walker did not reject missing span operations before pointer handling.");
      return false;
   }

   uint64_t dummy = 0;
   const FunctionField large_field = {
      "LargeValues", FDF_SPAN|FD_INT64, &glOversizedSpanOps
   };
   if (span_field_value(large_field, &dummy, &value, &byte_size) != ERR::InvalidData) {
      Log.warning("Overflowing span byte size was not rejected.");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Round-trip raw, structure and mutable spans through queue copying and relative message transport.

static void span_queue_callback()
{
}

static bool span_record_roundtrip_check(kt::Log &Log)
{
   struct SpanQueueArguments {
      std::span<const int8_t> Bytes;
      std::span<const RGB8> Colours;
      std::span<int16_t> Writable;
      std::span<const int8_t> Empty;
      FUNCTION Callback;
      int Tail;
   };

   const FunctionField fields[] = {
      { "Bytes", FDF_SPAN|FD_PTR, &glSpanFieldOps<const int8_t> },
      { "RGB8:Colours", FDF_SPAN|FD_PTR|FD_STRUCT, &glSpanFieldOps<const RGB8> },
      { "Writable", FDF_SPAN|FD_MUTABLE|FD_WORD, &glSpanFieldOps<int16_t> },
      { "Empty", FDF_SPAN|FD_PTR, &glSpanFieldOps<const int8_t> },
      { "Callback", FD_FUNCTION },
      { "Tail", FD_INT },
      { nullptr, 0 }
   };

   std::array<int8_t, 5> bytes = { 1, 2, 3, 4, 5 };
   std::array<RGB8, 2> colours = { RGB8{ 6, 7, 8, 9 }, RGB8{ 10, 11, 12, 13 } };
   std::array<int16_t, 3> writable = { 100, 200, 300 };
   FUNCTION callback(CALL::STD_C);
   callback.Context = CurrentContext();
   callback.Routine = (APTR)span_queue_callback;
   SpanQueueArguments source = { bytes, colours, writable, std::span<const int8_t>(bytes.data(), 0), callback, 77 };

   std::vector<int8_t> copied;
   if (copy_args(fields, sizeof(source), (int8_t *)&source, copied) != ERR::Okay) {
      Log.warning("Failed to copy a complete embedded-span record.");
      return false;
   }

   auto copied_args = (SpanQueueArguments *)copied.data();
   if ((copied_args->Bytes.size() != bytes.size()) or
       (memcmp(copied_args->Bytes.data(), bytes.data(), sizeof(bytes)) != 0) or
       (copied_args->Colours.size() != colours.size()) or
       (memcmp(copied_args->Colours.data(), colours.data(), sizeof(colours)) != 0)) {
      Log.warning("Read-only span payloads were not copied completely.");
      return false;
   }
   if ((copied_args->Writable.size() != writable.size()) or
       (not std::all_of(copied_args->Writable.begin(), copied_args->Writable.end(), [](int16_t Value) {
          return Value IS 0;
       }))) {
      Log.warning("Mutable span extension storage was not zero-initialised.");
      return false;
   }
   if ((copied_args->Empty.data() != nullptr) or (not copied_args->Empty.empty())) {
      Log.warning("An empty span was not copied in canonical form.");
      return false;
   }
   if ((uintptr_t(copied_args->Colours.data()) % alignof(RGB8)) or
       (uintptr_t(copied_args->Writable.data()) % alignof(int16_t))) {
      Log.warning("Typed span extension storage is incorrectly aligned.");
      return false;
   }
   if ((not copied_args->Callback.identical(callback)) or (copied_args->Tail != 77)) {
      Log.warning("Fields following embedded spans were copied at incorrect offsets.");
      return false;
   }

   bytes.fill(0x55);
   colours.fill(RGB8{});
   writable.fill(0x5555);
   if ((copied_args->Bytes[0] != 1) or (copied_args->Colours[0].Red != 6) or
       (copied_args->Writable[0] != 0)) {
      Log.warning("Copied span payloads retained caller-owned storage.");
      return false;
   }

   if (make_args_relative(fields, sizeof(source), copied.data(), copied.size()) != ERR::Okay) {
      Log.warning("Failed to convert embedded spans to relative offsets.");
      return false;
   }

   copied_args = (SpanQueueArguments *)copied.data();
   if ((uintptr_t(copied_args->Bytes.data()) < sizeof(source)) or
       (uintptr_t(copied_args->Bytes.data()) >= copied.size())) {
      Log.warning("Span data was not converted to a message-relative offset.");
      return false;
   }

   std::vector<int8_t> transported(copied);
   if (memcmp(transported.data(), copied.data(), copied.size()) != 0) {
      Log.warning("Byte-for-byte span message duplication failed.");
      return false;
   }
   if (make_args_absolute(fields, sizeof(source), transported.data(), transported.size()) != ERR::Okay) {
      Log.warning("Failed to restore embedded spans in a duplicated message.");
      return false;
   }

   auto transported_args = (SpanQueueArguments *)transported.data();
   if ((transported_args->Bytes.size() != 5) or (transported_args->Bytes[4] != 5) or
       (transported_args->Colours.size() != 2) or (transported_args->Colours[1].Alpha != 13) or
       (transported_args->Writable.size() != 3) or (transported_args->Empty.data() != nullptr) or
       (transported_args->Tail != 77) or
       (not transported_args->Callback.identical(callback))) {
      Log.warning("Restored span record contents or following fields are invalid.");
      return false;
   }

   std::vector<int8_t> invalid_offset(copied);
   auto invalid_args = (SpanQueueArguments *)invalid_offset.data();
   fields[0].SpanOps->Rebind(&invalid_args->Bytes, (CPTR)(invalid_offset.size() + 1), invalid_args->Bytes.size());
   if (make_args_absolute(fields, sizeof(source), invalid_offset.data(), invalid_offset.size()) != ERR::InvalidData) {
      Log.warning("An out-of-range relative span payload was not rejected.");
      return false;
   }

   release_copied_args(fields, sizeof(source), transported.data(), true);
   if (transported_args->Callback.defined()) {
      Log.warning("Callback cleanup after embedded spans used the wrong offset.");
      return false;
   }
   return true;
}

//********************************************************************************************************************
// Reject malformed source records before any payload is copied or dispatched.

static bool malformed_span_record_check(kt::Log &Log)
{
   alignas(std::span<const int8_t>) std::array<int8_t, sizeof(std::span<const int8_t>)> storage = {};
   const FunctionField null_fields[] = {
      { "Bytes", FDF_SPAN|FD_PTR, &glNullNonemptySpanOps },
      { nullptr, 0 }
   };
   std::vector<int8_t> copied;
   if (copy_args(null_fields, storage.size(), storage.data(), copied) != ERR::InvalidData) {
      Log.warning("A null span payload with non-zero extent was not rejected.");
      return false;
   }

   std::array<int8_t, 2> values = { 1, 2 };
   std::span<const int8_t> span(values);
   const FunctionField valid_fields[] = {
      { "Bytes", FDF_SPAN|FD_PTR, &glSpanFieldOps<const int8_t> },
      { nullptr, 0 }
   };
   if (copy_args(valid_fields, sizeof(span) - 1, (int8_t *)&span, copied) != ERR::InvalidData) {
      Log.warning("A truncated embedded-span record was not rejected.");
      return false;
   }

   const FunctionField oversized_fields[] = {
      { "Values", FDF_SPAN|FD_INT64, &glOversizedSpanOps },
      { nullptr, 0 }
   };
   uint64_t dummy = 0;
   if (copy_args(oversized_fields, sizeof(std::span<const uint64_t>), (int8_t *)&dummy, copied) != ERR::InvalidData) {
      Log.warning("A span payload beyond the message limit was not rejected.");
      return false;
   }
   return true;
}

//********************************************************************************************************************
// Queue several SetKey actions with string arguments held in temporary storage, then destroy that storage before
// the messages are processed.  Each QueueAction() call serialises into a temporary buffer that is released on
// return, so successive calls reuse the same heap blocks.  If the serialised messages retain pointers into those
// buffers (or into the caller's storage, which is scribbled after queueing), earlier messages are corrupted.

static bool queued_setkey_relocation_check(kt::Log &Log)
{
   auto task = (OBJECTPTR)CurrentTask();
   if (not task) {
      Log.warning("No current task available.");
      return false;
   }

   static const struct { CSTRING Key; CSTRING Value; } test_keys[] = {
      { "relocation_key_1", "relocation_value_1" },
      { "relocation_key_2", "relocation_value_2" },
      { "relocation_key_3", "relocation_value_3" }
   };

   for (auto &entry : test_keys) {
      auto key   = std::make_unique<char[]>(64);
      auto value = std::make_unique<char[]>(64);
      strcpy(key.get(), entry.Key);
      strcpy(value.get(), entry.Value);

      struct acSetKey args = { std::string_view(key.get()), std::string_view(value.get()) };
      if (QueueAction(AC::SetKey, task->UID, &args) != ERR::Okay) {
         Log.warning("QueueAction() rejected SetKey.");
         return false;
      }

      memset(key.get(), 0xee, 64);
      memset(value.get(), 0xee, 64);
   }

   // Additional heap churn so that the released serialisation buffers are reused before dispatch.

   std::vector<std::unique_ptr<char[]>> churn;
   for (int size=16; size <= 512; size += 8) {
      auto block = std::make_unique<char[]>(size);
      memset(block.get(), 0x55, size);
      churn.push_back(std::move(block));
   }

   ProcessMessages(PMF::NIL, 0);

   for (auto &entry : test_keys) {
      std::string result;
      struct acGetKey get = { entry.Key, &result };
      if (Action(AC::GetKey, task, &get) != ERR::Okay) {
         Log.warning("Queued SetKey for '%s' was not applied to the task.", entry.Key);
         return false;
      }

      if (result != entry.Value) {
         Log.warning("Queued SetKey for '%s' delivered a corrupted value '%s'.", entry.Key, result.c_str());
         return false;
      }
   }

   return true;
}

//********************************************************************************************************************

void queue_action_unit_tests(int &Passed, int &Total)
{
   kt::Log log("QueueAction");

   Total++;
   if (span_field_metadata_check(log)) Passed++;

   Total++;
   if (span_record_roundtrip_check(log)) Passed++;

   Total++;
   if (malformed_span_record_check(log)) Passed++;

   Total++;
   if (queued_setkey_relocation_check(log)) Passed++;
}
