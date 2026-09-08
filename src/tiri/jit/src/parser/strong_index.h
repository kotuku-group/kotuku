// Strongly typed integer indices used by parser-only metadata.

#pragma once

#include <cstddef>
#include <functional>

template<typename Tag, typename T>
struct StrongIndex {
   T value{};

   constexpr StrongIndex() = default;
   constexpr explicit StrongIndex(T Value) : value(Value) {}
   [[nodiscard]] constexpr T raw() const { return value; }

   // Construction remains explicit, while existing arithmetic and container access can use the underlying value.
   constexpr operator T() const { return value; }

   auto operator<=>(const StrongIndex &) const = default;
   bool operator==(const StrongIndex &) const = default;
};

template<typename Tag, typename T>
constexpr StrongIndex<Tag, T> operator+(StrongIndex<Tag, T> Left, T Offset)
{
   return StrongIndex<Tag, T>(Left.value + Offset);
}

template<typename Tag, typename T>
constexpr StrongIndex<Tag, T> operator+(StrongIndex<Tag, T> Left, StrongIndex<Tag, T> Offset)
{
   return StrongIndex<Tag, T>(Left.value + Offset.value);
}

template<typename Tag, typename T>
constexpr StrongIndex<Tag, T> operator-(StrongIndex<Tag, T> Left, T Offset)
{
   return StrongIndex<Tag, T>(Left.value - Offset);
}

template<typename Tag, typename T>
constexpr T operator-(StrongIndex<Tag, T> Left, StrongIndex<Tag, T> Right)
{
   return Left.value - Right.value;
}

template<typename Tag, typename T>
constexpr StrongIndex<Tag, T> & operator++(StrongIndex<Tag, T> &Value)
{
   ++Value.value;
   return Value;
}

template<typename Tag, typename T>
constexpr StrongIndex<Tag, T> operator++(StrongIndex<Tag, T> &Value, int)
{
   auto old = Value;
   ++Value.value;
   return old;
}

template<typename Tag, typename T>
constexpr StrongIndex<Tag, T> & operator--(StrongIndex<Tag, T> &Value)
{
   --Value.value;
   return Value;
}

template<typename Tag, typename T>
constexpr StrongIndex<Tag, T> operator--(StrongIndex<Tag, T> &Value, int)
{
   auto old = Value;
   --Value.value;
   return old;
}

template<typename Tag, typename T>
constexpr StrongIndex<Tag, T> & operator+=(StrongIndex<Tag, T> &Value, T Offset)
{
   Value.value += Offset;
   return Value;
}

template<typename Tag, typename T>
constexpr StrongIndex<Tag, T> & operator-=(StrongIndex<Tag, T> &Value, T Offset)
{
   Value.value -= Offset;
   return Value;
}

namespace std {

template<typename Tag, typename T>
struct hash<StrongIndex<Tag, T>> {
   [[nodiscard]] size_t operator()(const StrongIndex<Tag, T> &Value) const noexcept {
      return std::hash<T>{}(Value.raw());
   }
};

} // namespace std
