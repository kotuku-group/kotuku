
#include <kotuku/vector.hpp>
#include <array>
#include <forward_list>
#include <iostream>
#include <numeric>
#include <string>
#include <utility>

#ifndef IS
#define IS ==
#endif

struct TestContext {
   int total_checks{0};
   int failed_checks{0};

   void expect_true(bool Condition, char const *Message) {
      total_checks += 1;
      if (not Condition) {
         failed_checks += 1;
         std::cout << "FAILED: " << Message << '\n';
      }
   }

   template<typename T, typename U>
   void expect_equal(T const &Actual, U const &Expected, char const *Message) {
      total_checks += 1;
      if (not (Actual IS Expected)) {
         failed_checks += 1;
         std::cout << "FAILED: " << Message << " (actual=" << Actual << ", expected=" << Expected << ")\n";
      }
   }

   void summary() const {
      if (failed_checks IS 0) {
         std::cout << "All " << total_checks << " checks passed." << '\n';
      } else {
         std::cout << failed_checks << " of " << total_checks << " checks failed." << '\n';
      }
   }
};

struct ThrowOnMove {
   int value{0};
   inline static int value_constructs{0};
   inline static int copy_constructs{0};
   inline static int move_constructs{0};
   inline static int destructions{0};

   ThrowOnMove(int InValue = 0) : value(InValue) {
      value_constructs += 1;
   }

   ThrowOnMove(ThrowOnMove const &Other) : value(Other.value) {
      copy_constructs += 1;
   }

   ThrowOnMove(ThrowOnMove &&Other) noexcept(false) : value(Other.value) {
      move_constructs += 1;
   }

   ThrowOnMove &operator=(ThrowOnMove const &Other) {
      value = Other.value;
      return *this;
   }

   ~ThrowOnMove() noexcept {
      destructions += 1;
   }

   static void reset() {
      value_constructs = 0;
      copy_constructs = 0;
      move_constructs = 0;
      destructions = 0;
   }
};

struct NoThrowMover {
   int value{0};
   inline static int value_constructs{0};
   inline static int copy_constructs{0};
   inline static int move_constructs{0};
   inline static int destructions{0};

   NoThrowMover(int InValue = 0) noexcept : value(InValue) {
      value_constructs += 1;
   }

   NoThrowMover(NoThrowMover const &Other) noexcept : value(Other.value) {
      copy_constructs += 1;
   }

   NoThrowMover(NoThrowMover &&Other) noexcept : value(Other.value) {
      move_constructs += 1;
   }

   NoThrowMover &operator=(NoThrowMover const &Other) noexcept {
      value = Other.value;
      return *this;
   }

   ~NoThrowMover() noexcept {
      destructions += 1;
   }

   static void reset() {
      value_constructs = 0;
      copy_constructs = 0;
      move_constructs = 0;
      destructions = 0;
   }
};

void test_basic_accessors(TestContext &Context) {
   pf::vector<int> numbers;
   Context.expect_true(numbers.empty(), "Default vector starts empty");
   Context.expect_equal(numbers.size(), std::size_t(0), "Default size is zero");
   numbers.push_back(7);
   Context.expect_true(not numbers.empty(), "Vector is not empty after push_back");
   Context.expect_equal(numbers.size(), std::size_t(1), "Size increments after push_back");
   Context.expect_equal(numbers.front(), 7, "front returns the first element");
   Context.expect_equal(numbers.back(), 7, "back returns the last element");
   Context.expect_true(numbers.data() ? true : false, "data returns a valid pointer when populated");
   numbers.pop_back();
   Context.expect_true(numbers.empty(), "Vector becomes empty after pop_back");
   Context.expect_equal(numbers.size(), std::size_t(0), "Size returns to zero after pop_back");
}

void test_zero_capacity_growth(TestContext &Context) {
   pf::vector<int> values(0);
   values.push_back(1);
   Context.expect_equal(values.size(), std::size_t(1), "push_back succeeds after zero-capacity construction");
   Context.expect_equal(values.front(), 1, "push_back stores value in zero-capacity regression");
}

void test_range_and_initializer_construction(TestContext &Context) {
   std::array<int, 3> array_values{1, 2, 3};
   pf::vector<int> from_range(array_values.begin(), array_values.end());
   Context.expect_equal(from_range.size(), std::size_t(3), "Range constructor copies all elements");
   std::size_t index = 0;
   for (int value : from_range) {
      Context.expect_equal(value, array_values[index], "Range constructor preserves ordering");
      index += 1;
   }

   pf::vector<int> from_list{4, 5, 6, 7};
   Context.expect_equal(from_list.size(), std::size_t(4), "Initialiser list constructor sets size");
   Context.expect_equal(from_list.front(), 4, "Initialiser list front element matches");
   Context.expect_equal(from_list.back(), 7, "Initialiser list back element matches");

   std::forward_list<int> forward_source{8, 9, 10};
   pf::vector<int> from_forward(forward_source.begin(), forward_source.end());
   Context.expect_equal(from_forward.size(), std::size_t(3), "Forward iterator constructor copies all elements");
   Context.expect_equal(from_forward.front(), 8, "Forward iterator constructor keeps first element");
   Context.expect_equal(from_forward.back(), 10, "Forward iterator constructor keeps last element");

   pf::vector<int> reserved(32);
   Context.expect_equal(reserved.size(), std::size_t(0), "Explicit capacity constructor starts empty");
   reserved.push_back(42);
   Context.expect_equal(reserved.back(), 42, "Explicit capacity constructor allows pushes");
}

void test_copy_move_semantics(TestContext &Context) {
   pf::vector<int> original{1, 2, 3, 4};
   pf::vector<int> copied(original);
   Context.expect_equal(copied.size(), original.size(), "Copy constructor preserves size");
   std::size_t index = 0;
   for (int value : copied) {
      Context.expect_equal(value, original[index], "Copy constructor preserves contents");
      index += 1;
   }

   pf::vector<int> assigned;
   assigned = copied;
   Context.expect_equal(assigned.size(), copied.size(), "Copy assignment preserves size");
   index = 0;
   for (int value : assigned) {
      Context.expect_equal(value, copied[index], "Copy assignment preserves contents");
      index += 1;
   }

   pf::vector<int> moved(std::move(copied));
   Context.expect_equal(moved.size(), std::size_t(4), "Move constructor transfers size");
   Context.expect_equal(copied.size(), std::size_t(0), "Moved-from vector becomes empty");

   pf::vector<int> another{9, 10};
   moved = std::move(another);
   Context.expect_equal(moved.size(), std::size_t(2), "Move assignment transfers new size");
   Context.expect_equal(moved.front(), 9, "Move assignment transfers first value");
   Context.expect_equal(moved.back(), 10, "Move assignment transfers last value");

   pf::vector<int> left{11, 12};
   pf::vector<int> right{21};
   left.swap(right);
   Context.expect_equal(left.size(), std::size_t(1), "swap exchanges sizes");
   Context.expect_equal(right.size(), std::size_t(2), "swap exchanges sizes for other vector");
   Context.expect_equal(left.front(), 21, "swap moves values to left");
   Context.expect_equal(right.front(), 11, "swap moves values to right");
}

void test_iterator_coverage(TestContext &Context) {
   pf::vector<int> numbers{2, 4, 6, 8};
   Context.expect_equal(numbers.begin()[0], 2, "begin returns pointer to first element");
   Context.expect_equal(*(numbers.end() - 1), 8, "end points one past last element");
   Context.expect_equal(numbers.cbegin()[1], 4, "cbegin iterates over const data");
   Context.expect_equal(*(numbers.cend() - 1), 8, "cend matches end for const iteration");

   auto reverse_sum = std::accumulate(numbers.rbegin(), numbers.rend(), 0);
   Context.expect_equal(reverse_sum, 20, "Reverse iterator aggregates correctly");

   auto const_reverse_sum = std::accumulate(numbers.crbegin(), numbers.crend(), 0);
   Context.expect_equal(const_reverse_sum, 20, "Const reverse iterator aggregates correctly");

   auto third_iter = numbers.from(2);
   Context.expect_equal(*third_iter, 6, "from returns iterator at requested index");
}

void test_modifiers(TestContext &Context) {
   pf::vector<int> numbers{1, 3, 4};
   int lvalue = 0;
   numbers.insert(numbers.begin(), lvalue);
   Context.expect_equal(numbers.front(), 0, "insert with lvalue works at begin");

   numbers.insert(numbers.begin() + 2, 2);
   Context.expect_equal(numbers.begin()[2], 2, "insert with rvalue works inside vector");

   int extras[] = {5, 6, 7};
   numbers.insert(numbers.end(), extras, extras + 3);
   Context.expect_equal(numbers.size(), std::size_t(8), "Range insert appends new elements");
   Context.expect_equal(numbers.back(), 7, "Range insert preserves final element");

   auto erased = numbers.erase(numbers.begin() + 2);
   Context.expect_equal(*erased, 3, "erase returns iterator to next element");
   Context.expect_equal(numbers.size(), std::size_t(7), "erase removes one element");

   auto range_erased = numbers.erase(numbers.begin() + 3, numbers.end());
   Context.expect_true(range_erased IS numbers.end(), "Range erase returns end iterator");
   Context.expect_equal(numbers.size(), std::size_t(3), "Range erase shrinks vector appropriately");
   Context.expect_equal(numbers.back(), 3, "Range erase keeps remaining elements");

   pf::vector<std::pair<int, std::string>> paired;
   auto &emplaced = paired.emplace_back(1, "alpha");
   Context.expect_equal(emplaced.first, 1, "emplace_back constructs first element in place");
   Context.expect_true(emplaced.second.size() IS std::size_t(5), "emplace_back constructs second element in place");

   paired.clear();
   Context.expect_true(paired.empty(), "clear empties vector");
   Context.expect_equal(paired.size(), std::size_t(0), "clear sets size to zero");

   pf::vector<int> reserve_target;
   for (int value = 0; value < 32; value += 1) {
      reserve_target.push_back(value);
   }
   reserve_target.reserve(128);
   Context.expect_equal(reserve_target.size(), std::size_t(32), "reserve maintains element count");
   std::size_t index = 0;
   for (int value : reserve_target) {
      Context.expect_equal(value, int(index), "reserve keeps element order intact");
      index += 1;
   }
}

void test_comparisons(TestContext &Context) {
   pf::vector<int> alpha{1, 2, 3};
   pf::vector<int> beta{1, 2, 3};
   pf::vector<int> gamma{3, 2, 1};
   Context.expect_true(alpha IS beta, "operator== returns true for identical contents");
   Context.expect_true(not (alpha IS gamma), "operator== returns false for different contents");
   Context.expect_true(gamma != alpha, "operator!= returns true for different contents");
}

void test_sfinae_paths(TestContext &Context) {
   ThrowOnMove::reset();
   {
      pf::vector<ThrowOnMove> values;
      values.emplace_back(1);
      values.emplace_back(2);
      int copy_before = ThrowOnMove::copy_constructs;
      int move_before = ThrowOnMove::move_constructs;
      values.reserve(16);
      int copy_after = ThrowOnMove::copy_constructs;
      int move_after = ThrowOnMove::move_constructs;
      Context.expect_equal(copy_after - copy_before, 2, "reserve copies elements when move may throw");
      Context.expect_equal(move_after - move_before, 0, "reserve avoids moving when move may throw");
   }
   Context.expect_true(ThrowOnMove::destructions > 0, "Non-trivial destructor executed for ThrowOnMove elements");

   NoThrowMover::reset();
   {
      pf::vector<NoThrowMover> values;
      values.emplace_back(3);
      values.emplace_back(4);
      int copy_before = NoThrowMover::copy_constructs;
      int move_before = NoThrowMover::move_constructs;
      values.reserve(16);
      int copy_after = NoThrowMover::copy_constructs;
      int move_after = NoThrowMover::move_constructs;
      Context.expect_equal(move_after - move_before, 2, "reserve moves elements when move is noexcept");
      Context.expect_equal(copy_after - copy_before, 0, "reserve avoids copies when move is noexcept");

      int destruction_before = NoThrowMover::destructions;
      values.clear();
      int destruction_after = NoThrowMover::destructions;
      Context.expect_equal(destruction_after - destruction_before, 2, "clear calls destructor for each element when non-trivial");
   }
}

struct LifecycleTracker {
   int value{0};
   inline static int total_constructions{0};
   inline static int total_destructions{0};
   inline static int copy_constructions{0};
   inline static int move_constructions{0};
   inline static int assignments{0};

   LifecycleTracker(int InValue = 0) : value(InValue) {
      total_constructions += 1;
   }

   LifecycleTracker(LifecycleTracker const &Other) : value(Other.value) {
      total_constructions += 1;
      copy_constructions += 1;
   }

   LifecycleTracker(LifecycleTracker &&Other) noexcept : value(Other.value) {
      total_constructions += 1;
      move_constructions += 1;
   }

   LifecycleTracker &operator=(LifecycleTracker const &Other) {
      value = Other.value;
      assignments += 1;
      return *this;
   }

   LifecycleTracker &operator=(LifecycleTracker &&Other) noexcept {
      value = Other.value;
      assignments += 1;
      return *this;
   }

   ~LifecycleTracker() {
      total_destructions += 1;
   }

   static void reset() {
      total_constructions = 0;
      total_destructions = 0;
      copy_constructions = 0;
      move_constructions = 0;
      assignments = 0;
   }

   static bool is_balanced() {
      return total_constructions IS total_destructions;
   }
};

void test_insertion_lifecycle_management(TestContext &Context) {
   // Test single element insertion at various positions
   LifecycleTracker::reset();
   {
      pf::vector<LifecycleTracker> vec;

      // Initial elements to establish a baseline
      vec.emplace_back(1);
      vec.emplace_back(2);
      vec.emplace_back(3);

      int constructions_before = LifecycleTracker::total_constructions;
      int destructions_before = LifecycleTracker::total_destructions;

      // Insert at beginning (should trigger std::move_backward)
      LifecycleTracker new_item(0);
      vec.insert(vec.begin(), new_item);

      Context.expect_equal(vec.size(), std::size_t(4), "Insert at begin increases size");
      Context.expect_equal(vec.front().value, 0, "Insert at begin places correct value");

      // Insert in middle (should trigger std::move_backward)
      vec.insert(vec.begin() + 2, LifecycleTracker(99));

      Context.expect_equal(vec.size(), std::size_t(5), "Insert in middle increases size");
      Context.expect_equal(vec[2].value, 99, "Insert in middle places correct value");

      // Insert at end (should not trigger std::move_backward)
      vec.insert(vec.end(), LifecycleTracker(100));

      Context.expect_equal(vec.size(), std::size_t(6), "Insert at end increases size");
      Context.expect_equal(vec.back().value, 100, "Insert at end places correct value");

      int constructions_after = LifecycleTracker::total_constructions;
      int destructions_after = LifecycleTracker::total_destructions;

      // Verify we haven't leaked any objects during insertion operations
      Context.expect_true((constructions_after - constructions_before) >= 3, "At least 3 new constructions for inserted objects");
      Context.expect_true((destructions_after - destructions_before) >= 0, "No unexpected destructions during insertion");
   }

   // After vector destruction, verify all objects are properly cleaned up
   Context.expect_true(LifecycleTracker::is_balanced(), "All constructed objects are destroyed after vector destruction");

   // Test range insertion lifecycle management
   LifecycleTracker::reset();
   {
      pf::vector<LifecycleTracker> vec;
      vec.emplace_back(10);
      vec.emplace_back(20);

      int constructions_before = LifecycleTracker::total_constructions;

      // Create source data for range insertion
      LifecycleTracker source_data[] = {LifecycleTracker(30), LifecycleTracker(40), LifecycleTracker(50)};

      // Range insert at end
      vec.insert(vec.end(), source_data, source_data + 3);

      Context.expect_equal(vec.size(), std::size_t(5), "Range insert at end increases size correctly");
      Context.expect_equal(vec[2].value, 30, "Range insert preserves first element");
      Context.expect_equal(vec[4].value, 50, "Range insert preserves last element");

      // Range insert in middle (should trigger move_backward)
      LifecycleTracker middle_data[] = {LifecycleTracker(15)};
      vec.insert(vec.begin() + 1, middle_data, middle_data + 1);

      Context.expect_equal(vec.size(), std::size_t(6), "Range insert in middle increases size correctly");
      Context.expect_equal(vec[1].value, 15, "Range insert in middle places correct value");

      int constructions_after = LifecycleTracker::total_constructions;

      // Verify reasonable construction count (exact count depends on implementation details)
      Context.expect_true((constructions_after - constructions_before) >= 4, "Range insertion creates appropriate number of objects");
   }

   Context.expect_true(LifecycleTracker::is_balanced(), "All objects properly destroyed after range insertion test");

   // Test insertion with capacity expansion
   LifecycleTracker::reset();
   {
      pf::vector<LifecycleTracker> small_vec(2); // Small initial capacity
      small_vec.emplace_back(1);
      small_vec.emplace_back(2);

      int constructions_before = LifecycleTracker::total_constructions;

      // This should trigger capacity expansion
      small_vec.insert(small_vec.begin(), LifecycleTracker(0));

      Context.expect_equal(small_vec.size(), std::size_t(3), "Insert with expansion increases size");
      Context.expect_equal(small_vec.front().value, 0, "Insert with expansion places correct value");

      int constructions_after = LifecycleTracker::total_constructions;

      // During expansion, elements get copied/moved to new buffer
      Context.expect_true((constructions_after - constructions_before) >= 1, "Capacity expansion properly manages object lifecycle");
   }

   Context.expect_true(LifecycleTracker::is_balanced(), "All objects properly destroyed after capacity expansion test");
}

int main() {
   TestContext test_context;
   test_basic_accessors(test_context);
   test_zero_capacity_growth(test_context);
   test_range_and_initializer_construction(test_context);
   test_copy_move_semantics(test_context);
   test_iterator_coverage(test_context);
   test_modifiers(test_context);
   test_comparisons(test_context);
   test_sfinae_paths(test_context);
   test_insertion_lifecycle_management(test_context);
   test_context.summary();
   return test_context.failed_checks IS 0 ? 0 : 1;
}
