#include "../adapter_types.h"

#include <cstdint>
#include <iostream>

int main()
{
   int failures = 0;
   auto expect = [&failures](bool Condition, const char *Description) {
      if (not Condition) { std::cerr << Description << '\n'; failures++; }
   };
   int value = 123;
   expect(KotukuScintilla::PublicInteger(int64_t(-1), value) and std::cmp_equal(value, -1), "Preserve the -1 sentinel");
   expect(KotukuScintilla::PublicInteger(int64_t(INT32_MIN), value), "Accept INT_MIN");
   expect(KotukuScintilla::PublicInteger(int64_t(INT32_MAX), value), "Accept INT_MAX");
   expect(not KotukuScintilla::PublicInteger(int64_t(INT32_MIN) - 1, value), "Reject below INT_MIN");
   expect(not KotukuScintilla::PublicInteger(int64_t(INT32_MAX) + 1, value), "Reject above INT_MAX");
   expect(not KotukuScintilla::PublicInteger(UINT64_MAX, value), "Reject unsigned wraparound");
   expect(std::cmp_equal(value, INT32_MAX), "Rejected conversions preserve the destination");

   constexpr auto limit = KotukuScintilla::document_limit;
   expect(KotukuScintilla::DocumentGrowth(0, 0, size_t(limit)), "Accept the maximum public document");
   expect(not KotukuScintilla::DocumentGrowth(0, 0, size_t(limit) + 1), "Reserve the terminator and line count");
   expect(not KotukuScintilla::DocumentGrowth(limit, 0, 1), "Reject growth beyond the limit");
   expect(KotukuScintilla::DocumentGrowth(limit, 1, 1), "Allow equal-size replacement at the limit");
   expect(KotukuScintilla::DocumentGrowth(limit, limit, 0), "Allow deleting the entire document");
   expect(not KotukuScintilla::DocumentGrowth(0, 1, 0), "Reject deleting past the document");
   expect(not KotukuScintilla::DocumentGrowth(-1, 0, 0), "Reject a negative document size");
   expect(not KotukuScintilla::DocumentGrowth(1, -1, 0), "Reject a negative removal");
   expect(not KotukuScintilla::DocumentGrowth(1, 0, SIZE_MAX), "Reject overflowing additions");
   expect(not KotukuScintilla::DocumentGrowth(PTRDIFF_MAX, 0, 0), "Reject an oversized engine document");
   return failures ? 1 : 0;
}
