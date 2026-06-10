// This is a type-stable implementation of std::vector, for the purpose of ensuring that calls to size() and data()
// will always return the correct value regardless of the underlying type.

#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace kt {

template<typename T> class vector {
public:
   using value_type        = T;
   using reference         = T &;
   using const_reference   = T const &;
   using pointer           = T *;
   using const_pointer     = T const *;
   using iterator          = T *;
   using const_iterator    = T const *;
   using riterator         = std::reverse_iterator<iterator>;
   using const_riterator   = std::reverse_iterator<const_iterator>;
   using difference_type   = std::ptrdiff_t;
   using size_type         = std::size_t;

private:
   static const size_type MIN_CAPACITY = 8;
   size_type total_capacity;
   size_type length;
   T *elements;

   struct BufferGuard {
      T *data;
      size_type allocated;
      size_type constructed;

      BufferGuard(T *Data, size_type Allocated) noexcept :
         data(Data), allocated(Allocated), constructed(0)
      {
      }

      ~BufferGuard() {
         destroy_elements(data, constructed);
         deallocate_storage(data, allocated);
      }

      void release() noexcept {
         data        = nullptr;
         allocated   = 0;
         constructed = 0;
      }
   };

public:
   vector() noexcept : total_capacity(0), length(0), elements(nullptr) {
   }

   vector(size_type requested_capacity) : total_capacity(requested_capacity), length(0), elements(nullptr) {
      elements = allocate_storage(total_capacity);
   }

   template<std::input_iterator I> vector(I begin, I end) : vector() {
      assign_from_range(begin, end);
   }

   vector(std::initializer_list<T> const &list) : vector(std::begin(list), std::end(list)) {
   }

   ~vector() {
      destroy_elements(elements, length);
      deallocate_storage(elements, total_capacity);
   }

   vector(vector const &copy) : total_capacity(copy.length), length(0), elements(nullptr) {
      elements = allocate_storage(total_capacity);
      BufferGuard guard(elements, total_capacity);
      construct_copy_range(guard, copy.element_at(0), copy.element_at(copy.length));
      length = guard.constructed;
      guard.release();
   }

   vector & operator=(vector const &copy) {
      if (this != &copy) {
         vector<T> tmp(copy);
         tmp.swap(*this);
      }
      return *this;
   }

   vector(vector &&move) noexcept :
      total_capacity(move.total_capacity), length(move.length), elements(move.elements)
   {
      move.total_capacity = 0;
      move.length         = 0;
      move.elements       = nullptr;
   }

   vector & operator=(vector &&move) noexcept {
      if (this != &move) {
         destroy_elements(elements, length);
         deallocate_storage(elements, total_capacity);

         total_capacity = move.total_capacity;
         length         = move.length;
         elements       = move.elements;

         move.total_capacity = 0;
         move.length         = 0;
         move.elements       = nullptr;
      }
      return *this;
   }

   void swap(vector &other) noexcept {
      std::swap(total_capacity, other.total_capacity);
      std::swap(length, other.length);
      std::swap(elements, other.elements);
   }

   inline size_type size() const { return length; }
   inline size_type capacity() const { return total_capacity; }
   inline bool      empty() const { return not length; }
   inline T* data() { return elements; }
   inline const T* data() const { return elements; }

   inline reference       operator[](size_type index) { return elements[index];}
   inline const_reference operator[](size_type index) const { return elements[index];}
   inline reference       front() { return elements[0];}
   inline const_reference front() const { return elements[0];}
   inline reference       back() { return elements[length - 1];}
   inline const_reference back() const { return elements[length - 1];}

   inline iterator        begin() { return elements;}
   inline riterator       rbegin() { return riterator(end());}
   inline const_iterator  begin() const { return elements;}
   inline const_riterator rbegin() const { return const_riterator(end());}

   inline iterator        end() { return elements ? elements + length : nullptr;}
   inline riterator       rend() { return riterator(begin());}
   inline const_iterator  end() const { return elements ? elements + length : nullptr;}
   inline const_riterator rend() const { return const_riterator(begin());}

   inline const_iterator  cbegin() const { return begin();}
   inline const_riterator crbegin() const { return rbegin();}
   inline const_iterator  cend() const { return end();}
   inline const_riterator crend() const { return rend();}

   inline iterator from(size_t pIndex) { return iterator(&elements[pIndex]); }

   // Erasure

   inline iterator erase(const_iterator pos) {
      return erase(pos, pos ? pos + 1 : pos);
   }

   iterator erase(const_iterator first, const_iterator last) {
      if (not (first != last)) return const_cast<iterator>(first);
      if (not length) return begin();
      if (not iterator_inside_storage(first)) return const_cast<iterator>(first);
      if (not iterator_inside_or_end(last)) return const_cast<iterator>(first);
      if (last < first) return const_cast<iterator>(first);

      size_type index = size_type(first - begin());
      size_type count = size_type(last - first);
      if (not count) return begin() + index;

      if constexpr (std::is_nothrow_move_assignable_v<T>) {
         for (size_type i = index; i < length - count; ++i) {
            elements[i] = std::move(elements[i + count]);
         }
         destroy_elements(elements + length - count, count);
         length -= count;
      }
      else {
         vector<T> tmp(length - count);
         for (size_type i = 0; i < index; ++i) {
            tmp.push_back(elements[i]);
         }
         for (size_type i = index + count; i < length; ++i) {
            tmp.push_back(elements[i]);
         }
         tmp.swap(*this);
      }

      return index < length ? begin() + index : end();
   }

   iterator insert(const_iterator pTarget, const T &pValue) {
      T value(pValue);
      return insert_value(pTarget, std::move(value));
   }

   iterator insert(const_iterator pTarget, T &&pValue) {
      T value(std::move(pValue));
      return insert_value(pTarget, std::move(value));
   }

   void insert(const_iterator pTarget, const_iterator pStart, const_iterator pEnd) {
      if (not (pStart != pEnd)) return;
      if (pEnd < pStart) return;

      if (source_overlaps_storage(pStart, pEnd)) {
         vector<T> copy(pStart, pEnd);
         insert(pTarget, copy.begin(), copy.end());
         return;
      }

      size_type index = iterator_index(pTarget);
      size_type count = size_type(pEnd - pStart);
      size_type new_length = checked_add(length, count);
      size_type new_capacity = total_capacity;
      if (new_length > new_capacity) new_capacity = new_length;

      pointer new_elements = allocate_storage(new_capacity);
      BufferGuard guard(new_elements, new_capacity);

      construct_existing_range(guard, element_at(0), element_at(index));
      construct_copy_range(guard, pStart, pEnd);
      construct_existing_range(guard, element_at(index), element_at(length));

      destroy_elements(elements, length);
      deallocate_storage(elements, total_capacity);

      elements       = new_elements;
      length         = new_length;
      total_capacity = new_capacity;
      guard.release();
   }

   // Comparison

   inline bool operator!=(vector const &rhs) const {return !(*this == rhs);}

   inline bool operator==(vector const &rhs) const {
      return (size() == rhs.size()) and std::equal(begin(), end(), rhs.begin());
   }

   inline void push_back(value_type const &value) {
      T copy(value);
      ensure_capacity_for(checked_add(length, size_type(1)));
      moveBackInternal(std::move(copy));
   }

   inline void push_back(value_type &&value) {
      T moved(std::move(value));
      ensure_capacity_for(checked_add(length, size_type(1)));
      moveBackInternal(std::move(moved));
   }

   template<typename... Args> T & emplace_back(Args&&... args) {
      ensure_capacity_for(checked_add(length, size_type(1)));
      T *target = elements + length;
      new (target) T(std::forward<Args>(args)...);
      length += 1;
      return *target;
   }

   inline void pop_back() {
      length -= 1;
      elements[length].~T();
   }

   inline void reserve(size_type capacityUpperBound) {
      if (capacityUpperBound > total_capacity) {
         reserveCapacity(capacityUpperBound);
      }
   }

   void resize(size_type Count) {
      if (Count < length) {
         destroy_elements(elements + Count, length - Count);
         length = Count;
      }
      else if (Count > length) {
         reserve(Count);
         while (length < Count) {
            new (elements + length) T();
            length += 1;
         }
      }
   }

   void resize(size_type Count, const value_type &Value) {
      if (Count < length) {
         destroy_elements(elements + Count, length - Count);
         length = Count;
      }
      else if (Count > length) {
         reserve(Count);
         while (length < Count) {
            new (elements + length) T(Value);
            length += 1;
         }
      }
   }

   inline void clear() {
      destroy_elements(elements, length);
      length = 0;
   }

   // INTERNAL FUNCTIONALITY

private:
   static constexpr bool needs_aligned_storage() noexcept {
      return alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__;
   }

   static constexpr size_type max_size() noexcept {
      return std::numeric_limits<size_type>::max() / sizeof(T);
   }

   [[noreturn]] static void length_failure() noexcept {
      std::abort();
   }

   static size_type checked_add(size_type left, size_type right) noexcept {
      if (left > std::numeric_limits<size_type>::max() - right) length_failure();
      return left + right;
   }

   static void check_allocation_size(size_type count) noexcept {
      if (count > max_size()) length_failure();
   }

   static pointer allocate_storage(size_type count) {
      if (not count) return nullptr;
      check_allocation_size(count);
      size_type bytes = sizeof(T) * count;
      if constexpr (needs_aligned_storage()) {
         return (T *)(::operator new(bytes, std::align_val_t(alignof(T))));
      }
      else {
         return (T *)(::operator new(bytes));
      }
   }

   static void deallocate_storage(pointer data, size_type) noexcept {
      if (not data) return;
      if constexpr (needs_aligned_storage()) {
         ::operator delete(data, std::align_val_t(alignof(T)));
      }
      else {
         ::operator delete(data);
      }
   }

   static void destroy_elements(pointer data, size_type count) noexcept {
      if constexpr (not std::is_trivially_destructible_v<T>) {
         for (size_type i = count; i > 0; --i) {
            data[i - 1].~T();
         }
      }
   }

   static size_type recommended_capacity(size_type required, size_type current) noexcept {
      if (required > max_size()) length_failure();
      if (required <= current) return current;

      size_type next = current ? current : MIN_CAPACITY;
      while (next < required) {
         if (next > max_size() / 2) {
            next = required;
         }
         else {
            next *= 2;
         }
      }
      return next;
   }

   template<std::input_iterator I> void assign_from_range(I begin, I end) {
      if constexpr (std::forward_iterator<I>) {
         auto count = size_type(std::distance(begin, end));
         reserve(count);
      }

      for (auto i = begin; i != end; ++i) {
         push_back(*i);
      }
   }

   static void construct_copy_range(BufferGuard &guard, const_iterator first, const_iterator last) {
      for (auto source = first; source != last; ++source) {
         new (guard.data + guard.constructed) T(*source);
         guard.constructed += 1;
      }
   }

   static void construct_existing_range(BufferGuard &guard, iterator first, iterator last) {
      if (not (first != last)) return;

      if constexpr (std::is_trivially_copyable_v<T> and std::is_trivially_destructible_v<T>) {
         size_type count = size_type(last - first);
         if (count) {
            std::memcpy(guard.data + guard.constructed, first, sizeof(T) * count);
            guard.constructed += count;
         }
      }
      else {
         for (auto source = first; source != last; ++source) {
            new (guard.data + guard.constructed) T(std::move_if_noexcept(*source));
            guard.constructed += 1;
         }
      }
   }

   void ensure_capacity_for(size_type required) {
      if (required > total_capacity) {
         reserveCapacity(recommended_capacity(required, total_capacity));
      }
   }

   void reserveCapacity(size_type newCapacity) {
      if (newCapacity <= total_capacity) return;

      pointer new_elements = allocate_storage(newCapacity);
      BufferGuard guard(new_elements, newCapacity);
      construct_existing_range(guard, element_at(0), element_at(length));

      destroy_elements(elements, length);
      deallocate_storage(elements, total_capacity);

      elements       = new_elements;
      total_capacity = newCapacity;
      guard.release();
   }

   // Add new element to the end using placement new

   inline void pushBackInternal(T const &value) {
      new (elements + length) T(value);
      ++length;
   }

   inline void moveBackInternal(T &&value) {
      new (elements + length) T(std::move(value));
      ++length;
   }

   size_type iterator_index(const_iterator target) const noexcept {
      if (not target) return 0;
      return size_type(target - begin());
   }

   iterator element_at(size_type index) noexcept {
      return elements ? elements + index : nullptr;
   }

   const_iterator element_at(size_type index) const noexcept {
      return elements ? elements + index : nullptr;
   }

   bool iterator_inside_storage(const_iterator target) const noexcept {
      return elements and (target >= begin()) and (target < end());
   }

   bool iterator_inside_or_end(const_iterator target) const noexcept {
      return elements and (target >= begin()) and (target <= end());
   }

   bool source_overlaps_storage(const_iterator first, const_iterator last) const noexcept {
      if ((not elements) or (not (first != last))) return false;
      std::less<const_iterator> less;
      return less(first, end()) and less(begin(), last);
   }

   template<typename U> iterator insert_value(const_iterator pTarget, U &&pValue) {
      size_type index = iterator_index(pTarget);
      size_type new_length = checked_add(length, size_type(1));

      if ((new_length > total_capacity) or (not std::is_nothrow_move_constructible_v<T>) or
            (not std::is_nothrow_move_assignable_v<T>)) {
         size_type new_capacity = recommended_capacity(new_length, total_capacity);
         pointer new_elements = allocate_storage(new_capacity);
         BufferGuard guard(new_elements, new_capacity);

         construct_existing_range(guard, element_at(0), element_at(index));
         new (guard.data + guard.constructed) T(std::forward<U>(pValue));
         guard.constructed += 1;
         construct_existing_range(guard, element_at(index), element_at(length));

         destroy_elements(elements, length);
         deallocate_storage(elements, total_capacity);

         elements       = new_elements;
         length         = new_length;
         total_capacity = new_capacity;
         guard.release();
      }
      else if (index >= length) {
         new (elements + length) T(std::forward<U>(pValue));
         length = new_length;
      }
      else {
         new (elements + length) T(std::move_if_noexcept(elements[length - 1]));
         for (size_type i = length - 1; i > index; --i) {
            elements[i] = std::move_if_noexcept(elements[i - 1]);
         }
         elements[index] = std::forward<U>(pValue);
         length = new_length;
      }

      return begin() + index;
   }
};

} // namespace
