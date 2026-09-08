// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.
// ---
// Provides AGG's lightweight POD arrays, block allocators, and sorted vector containers. Hooks into path storage,
// rasteriser cells, scanline storage, gradient LUTs, and other allocation-heavy helpers. In the vector renderer it
// supplies compact geometry and span storage without depending on STL container behaviour.

#pragma once

#include <cstddef>
#include <cstring>
#include <algorithm>
#include <memory>
#include <type_traits>
#include "agg_basics.h"

namespace agg
{
    template<typename T>
    requires std::is_trivially_copyable_v<T>
    class pod_array_adaptor
    {
    public:
        using value_type = T;
        constexpr pod_array_adaptor(T* array, unsigned size) noexcept :
            m_array(array), m_size(size) {}

        constexpr unsigned size() const noexcept { return m_size; }
        constexpr const T& operator [] (unsigned i) const noexcept { return m_array[i]; }
        constexpr       T& operator [] (unsigned i)       noexcept { return m_array[i]; }
        constexpr const T& at(unsigned i) const           noexcept { return m_array[i]; }
        constexpr       T& at(unsigned i)                 noexcept { return m_array[i]; }
        constexpr T  value_at(unsigned i) const           noexcept { return m_array[i]; }

    private:
        T*       m_array;
        unsigned m_size;
    };


    //---------------------------------------------------------pod_auto_array
    template<typename T, unsigned Size>
    requires std::is_trivially_copyable_v<T> && (Size > 0)
    class pod_auto_array
    {
    public:
        using value_type = T;
        using self_type = pod_auto_array<T, Size>;

        pod_auto_array() {}
        explicit pod_auto_array(const T* c) noexcept
        {
            std::copy_n(c, Size, m_array);
        }

        constexpr const self_type& operator = (const T* c) noexcept
        {
            std::copy_n(c, Size, m_array);
            return *this;
        }

        static constexpr unsigned size() noexcept { return Size; }
        constexpr const T& operator [] (unsigned i) const noexcept { return m_array[i]; }
        constexpr       T& operator [] (unsigned i)       noexcept { return m_array[i]; }
        constexpr const T& at(unsigned i) const           noexcept { return m_array[i]; }
        constexpr       T& at(unsigned i)                 noexcept { return m_array[i]; }
        constexpr T  value_at(unsigned i) const           noexcept { return m_array[i]; }

    private:
        T m_array[Size];
    };


    //--------------------------------------------------------pod_auto_vector
    template<typename T, unsigned Size>
    requires std::is_trivially_copyable_v<T> && (Size > 0)
    class pod_auto_vector
    {
    public:
        using value_type = T;
        using self_type = pod_auto_vector<T, Size>;

        constexpr pod_auto_vector() noexcept : m_size(0) {}

        // Consolidated mutation methods
        constexpr void remove_all()            noexcept { m_size = 0; }
        constexpr void clear()                 noexcept { m_size = 0; }
        constexpr void add(const T& v)         noexcept { m_array[m_size++] = v; }
        constexpr void push_back(const T& v)   noexcept { add(v); }
        constexpr void inc_size(unsigned size) noexcept { m_size += size; }

        constexpr unsigned size() const noexcept { return m_size; }
        constexpr const T& operator [] (unsigned i) const noexcept { return m_array[i]; }
        constexpr       T& operator [] (unsigned i)       noexcept { return m_array[i]; }
        constexpr const T& at(unsigned i) const           noexcept { return (*this)[i]; }
        constexpr       T& at(unsigned i)                 noexcept { return (*this)[i]; }
        constexpr T  value_at(unsigned i) const           noexcept { return (*this)[i]; }

    private:
        T m_array[Size];
        unsigned m_size;
    };


    //---------------------------------------------------------------pod_array
    template<typename T>
    requires std::is_trivially_copyable_v<T>
    class pod_array
    {
    public:
        using value_type = T;
        using self_type = pod_array<T>;

        // Modern constructors with RAII
        pod_array() noexcept : m_array(nullptr), m_size(0) {}

        explicit pod_array(unsigned size) :
            m_array(std::make_unique<T[]>(size)), m_size(size) {}

        // Copy constructor with proper exception safety
        pod_array(const self_type& v) :
            m_array(v.m_size > 0 ? std::make_unique<T[]>(v.m_size) : nullptr),
            m_size(v.m_size)
        {
            if (m_size > 0) {
                std::copy_n(v.m_array.get(), m_size, m_array.get());
            }
        }

        // Move constructor
        pod_array(self_type&& v) noexcept :
            m_array(std::move(v.m_array)), m_size(v.m_size)
        {
            v.m_size = 0;
        }

        // Copy assignment with strong exception safety
        const self_type& operator = (const self_type& v) {
            if (this != &v) {
                self_type temp(v);  // Copy-and-swap idiom
                swap(temp);
            }
            return *this;
        }

        // Move assignment
        self_type& operator = (self_type&& v) noexcept {
            if (this != &v) {
                m_array = std::move(v.m_array);
                m_size = v.m_size;
                v.m_size = 0;
            }
            return *this;
        }

        void resize(unsigned size) {
            if (size != m_size) {
                m_array = size > 0 ? std::make_unique<T[]>(size) : nullptr;
                m_size = size;
            }
        }

        void swap(self_type& other) noexcept {
            std::swap(m_array, other.m_array);
            std::swap(m_size, other.m_size);
        }

        constexpr unsigned size() const noexcept { return m_size; }
        constexpr const T& operator [] (unsigned i) const noexcept { return m_array[i]; }
        constexpr       T& operator [] (unsigned i)       noexcept { return m_array[i]; }
        constexpr const T& at(unsigned i) const           noexcept { return m_array[i]; }
        constexpr       T& at(unsigned i)                 noexcept { return m_array[i]; }
        constexpr T  value_at(unsigned i) const           noexcept { return m_array[i]; }

        const T* data() const noexcept { return m_array.get(); }
              T* data()       noexcept { return m_array.get(); }

    private:
        std::unique_ptr<T[]> m_array;
        unsigned m_size;
    };

    //--------------------------------------------------------------pod_vector
    // A simple class template to store Plain Old Data, a vector
    // of a fixed size. The data is continous in memory
    //------------------------------------------------------------------------
    template<typename T>
    requires std::is_trivially_copyable_v<T>
    class pod_vector
    {
    public:
        using value_type = T;

        pod_vector() noexcept : m_size(0), m_capacity(0), m_array(nullptr) {}
        explicit pod_vector(unsigned cap, unsigned extra_tail=0);

        // Copy constructor with exception safety
        pod_vector(const pod_vector<T>& v);

        // Move constructor
        pod_vector(pod_vector<T>&& v) noexcept;

        // Copy assignment
        const pod_vector<T>& operator = (const pod_vector<T>& v);

        // Move assignment
        pod_vector<T>& operator = (pod_vector<T>&& v) noexcept;

        // Set new capacity. All data is lost, size is set to zero.
        void capacity(unsigned cap, unsigned extra_tail=0);
        constexpr unsigned capacity() const noexcept { return m_capacity; }

        // Allocate n elements. All data is lost,
        // but elements can be accessed in range 0...size-1.
        void allocate(unsigned size, unsigned extra_tail=0);

        void resize(unsigned new_size);

        void zero() noexcept {
            if (m_array and m_size > 0) {
                std::fill_n(m_array.get(), m_size, T{});
            }
        }

        void add(const T& v)         { m_array[m_size++] = v; }
        void push_back(const T& v)   { m_array[m_size++] = v; }
        void push_back(T&& v)        { m_array[m_size++] = std::move(v); }
        void insert_at(unsigned pos, const T& val);
        void inc_size(unsigned size) { m_size += size; }
        constexpr unsigned size()      const noexcept { return m_size; }
        constexpr unsigned byte_size() const noexcept { return m_size * sizeof(T); }
        void serialize(int8u* ptr) const;
        void deserialize(const int8u* data, unsigned byte_size);
        constexpr const T& operator [] (unsigned i) const noexcept { return m_array[i]; }
        constexpr       T& operator [] (unsigned i)       noexcept { return m_array[i]; }
        constexpr const T& at(unsigned i) const           noexcept { return m_array[i]; }
        constexpr       T& at(unsigned i)                 noexcept { return m_array[i]; }
        constexpr T  value_at(unsigned i) const           noexcept { return m_array[i]; }

        const T* data() const noexcept { return m_array.get(); }
              T* data()       noexcept { return m_array.get(); }

        void remove_all()         noexcept { m_size = 0; }
        void clear()              noexcept { m_size = 0; }
        void cut_at(unsigned num) noexcept { if(num < m_size) m_size = num; }

        void swap(pod_vector<T>& other) noexcept {
            std::swap(m_array, other.m_array);
            std::swap(m_size, other.m_size);
            std::swap(m_capacity, other.m_capacity);
        }

    private:
        unsigned m_size;
        unsigned m_capacity;
        std::unique_ptr<T[]> m_array;
    };

    template<typename T>
    requires std::is_trivially_copyable_v<T>
    void pod_vector<T>::capacity(unsigned cap, unsigned extra_tail) {
        m_size = 0;
        if (cap > m_capacity) {
            m_capacity = cap + extra_tail;
            m_array = m_capacity > 0 ? std::make_unique<T[]>(m_capacity) : nullptr;
        }
    }

    template<typename T>
    requires std::is_trivially_copyable_v<T>
    void pod_vector<T>::allocate(unsigned size, unsigned extra_tail) {
        capacity(size, extra_tail);
        m_size = size;
    }

    template<typename T>
    requires std::is_trivially_copyable_v<T>
    void pod_vector<T>::resize(unsigned new_size) {
        if (new_size > m_size) {
            if (new_size > m_capacity) {
                auto new_array = std::make_unique<T[]>(new_size);
                if (m_array and m_size > 0) {
                    std::copy_n(m_array.get(), m_size, new_array.get());
                }
                m_array = std::move(new_array);
                m_capacity = new_size;
            }
        }
        m_size = new_size;
    }

    template<typename T>
    requires std::is_trivially_copyable_v<T>
    pod_vector<T>::pod_vector(unsigned cap, unsigned extra_tail) :
        m_size(0), m_capacity(cap + extra_tail),
        m_array(m_capacity > 0 ? std::make_unique<T[]>(m_capacity) : nullptr) {}

    template<typename T>
    requires std::is_trivially_copyable_v<T>
    pod_vector<T>::pod_vector(const pod_vector<T>& v) :
        m_size(v.m_size), m_capacity(v.m_capacity),
        m_array(v.m_capacity > 0 ? std::make_unique<T[]>(v.m_capacity) : nullptr)
    {
        if (m_array and v.m_array and m_size > 0) {
            std::copy_n(v.m_array.get(), m_size, m_array.get());
        }
    }

    template<typename T>
    requires std::is_trivially_copyable_v<T>
    pod_vector<T>::pod_vector(pod_vector<T>&& v) noexcept :
        m_size(v.m_size), m_capacity(v.m_capacity), m_array(std::move(v.m_array))
    {
        v.m_size = 0;
        v.m_capacity = 0;
    }

    template<typename T>
    requires std::is_trivially_copyable_v<T>
    const pod_vector<T>& pod_vector<T>::operator = (const pod_vector<T>& v) {
        if (this != &v) {
            allocate(v.m_size);
            if (v.m_size > 0 and m_array and v.m_array) {
                std::copy_n(v.m_array.get(), v.m_size, m_array.get());
            }
        }
        return *this;
    }

    template<typename T>
    requires std::is_trivially_copyable_v<T>
    pod_vector<T>& pod_vector<T>::operator = (pod_vector<T>&& v) noexcept {
        if (this != &v) {
            m_array = std::move(v.m_array);
            m_size = v.m_size;
            m_capacity = v.m_capacity;
            v.m_size = 0;
            v.m_capacity = 0;
        }
        return *this;
    }

    template<typename T>
    requires std::is_trivially_copyable_v<T>
    void pod_vector<T>::serialize(int8u* ptr) const {
        if (m_size > 0 and m_array) {
            std::memcpy(ptr, m_array.get(), m_size * sizeof(T));
        }
    }

    template<typename T>
    requires std::is_trivially_copyable_v<T>
    void pod_vector<T>::deserialize(const int8u* data, unsigned byte_size) {
        byte_size /= sizeof(T);
        allocate(byte_size);
        if (byte_size > 0 and m_array and data) {
            std::memcpy(m_array.get(), data, byte_size * sizeof(T));
        }
    }

    template<typename T>
    requires std::is_trivially_copyable_v<T>
    void pod_vector<T>::insert_at(unsigned pos, const T& val) {
        if (pos >= m_size) {
            m_array[m_size] = val;
        } else {
            std::memmove(m_array.get() + pos + 1, m_array.get() + pos, (m_size - pos) * sizeof(T));
            m_array[pos] = val;
        }
        ++m_size;
    }

    //---------------------------------------------------------------pod_bvector
    // A simple class template to store Plain Old Data, similar to std::deque
    // It doesn't reallocate memory but instead, uses blocks of data of size
    // of (1 << S), that is, power of two. The data is NOT contiguous in memory,
    // so the only valid access method is operator [] or curr(), prev(), next()
    //
    // There reallocs occure only when the pool of pointers to blocks needs
    // to be extended (it happens very rarely). You can control the value
    // of increment to reallocate the pointer buffer. See the second constructor.
    // By default, the incremeent value equals (1 << S), i.e., the block size.
    //------------------------------------------------------------------------
    template<typename T, unsigned S=6>
    requires std::is_trivially_copyable_v<T> && (S > 0)
    class pod_bvector
    {
    public:
        enum block_scale_e
        {
            block_shift = S,
            block_size  = 1 << block_shift,
            block_mask  = block_size - 1
        };

        using value_type = T;

        ~pod_bvector();
        pod_bvector();
        pod_bvector(unsigned block_ptr_inc);

        // Copying
        pod_bvector(const pod_bvector<T, S>& v);
        const pod_bvector<T, S>& operator = (const pod_bvector<T, S>& v);

        void remove_all() { m_size = 0; }
        void clear()      { m_size = 0; }
        void free_all()   { free_tail(0); }
        void free_tail(unsigned size);
        void add(const T& val);
        void push_back(const T& val) { add(val); }
        void modify_last(const T& val);
        void remove_last();

        int allocate_continuous_block(unsigned num_elements);

        void add_array(const T* ptr, unsigned num_elem)
        {
            while(num_elem--)
            {
                add(*ptr++);
            }
        }

        template<class DataAccessor> void add_data(DataAccessor& data)
        {
            while(data.size())
            {
                add(*data);
                ++data;
            }
        }

        void cut_at(unsigned size)
        {
            if(size < m_size) m_size = size;
        }

        unsigned size() const { return m_size; }

        const T& operator [] (unsigned i) const
        {
            return m_blocks[i >> block_shift][i & block_mask];
        }

        T& operator [] (unsigned i)
        {
            return m_blocks[i >> block_shift][i & block_mask];
        }

        const T& at(unsigned i) const
        {
            return m_blocks[i >> block_shift][i & block_mask];
        }

        T& at(unsigned i)
        {
            return m_blocks[i >> block_shift][i & block_mask];
        }

        T value_at(unsigned i) const
        {
            return m_blocks[i >> block_shift][i & block_mask];
        }

        const T& curr(unsigned idx) const
        {
            return (*this)[idx];
        }

        T& curr(unsigned idx)
        {
            return (*this)[idx];
        }

        const T& prev(unsigned idx) const
        {
            return (*this)[(idx + m_size - 1) % m_size];
        }

        T& prev(unsigned idx)
        {
            return (*this)[(idx + m_size - 1) % m_size];
        }

        const T& next(unsigned idx) const
        {
            return (*this)[(idx + 1) % m_size];
        }

        T& next(unsigned idx)
        {
            return (*this)[(idx + 1) % m_size];
        }

        const T& last() const
        {
            return (*this)[m_size - 1];
        }

        T& last()
        {
            return (*this)[m_size - 1];
        }

        unsigned byte_size() const;
        void serialize(int8u* ptr) const;
        void deserialize(const int8u* data, unsigned byte_size);
        void deserialize(unsigned start, const T& empty_val,
                         const int8u* data, unsigned byte_size);

        template<class ByteAccessor>
        void deserialize(ByteAccessor data)
        {
            remove_all();
            unsigned elem_size = data.size() / sizeof(T);

            for(unsigned i = 0; i < elem_size; ++i)
            {
                int8u* ptr = (int8u*)data_ptr();
                for(unsigned j = 0; j < sizeof(T); ++j)
                {
                    *ptr++ = *data;
                    ++data;
                }
                ++m_size;
            }
        }

        template<class ByteAccessor>
        void deserialize(unsigned start, const T& empty_val, ByteAccessor data)
        {
            while(m_size < start)
            {
                add(empty_val);
            }

            unsigned elem_size = data.size() / sizeof(T);
            for(unsigned i = 0; i < elem_size; ++i)
            {
                int8u* ptr;
                if(start + i < m_size)
                {
                    ptr = (int8u*)(&((*this)[start + i]));
                }
                else
                {
                    ptr = (int8u*)data_ptr();
                    ++m_size;
                }
                for(unsigned j = 0; j < sizeof(T); ++j)
                {
                    *ptr++ = *data;
                    ++data;
                }
            }
        }

        const T* block(unsigned nb) const { return m_blocks[nb]; }

    private:
        void allocate_block(unsigned nb);
        T*   data_ptr();

        unsigned        m_size;
        unsigned        m_num_blocks;
        unsigned        m_max_blocks;
        T**             m_blocks;
        unsigned        m_block_ptr_inc;
    };

    template<typename T, unsigned S>
    requires std::is_trivially_copyable_v<T> && (S > 0)
    pod_bvector<T, S>::~pod_bvector()
    {
        if(m_num_blocks)
        {
            T** blk = m_blocks + m_num_blocks - 1;
            while(m_num_blocks--)
            {
                pod_allocator<T>::deallocate(*blk);
                --blk;
            }
        }
        pod_allocator<T*>::deallocate(m_blocks);
    }

    template<typename T, unsigned S>
    requires std::is_trivially_copyable_v<T> && (S > 0)
    void pod_bvector<T, S>::free_tail(unsigned size)
    {
        if(size < m_size)
        {
            unsigned nb = (size + block_mask) >> block_shift;
            while(m_num_blocks > nb)
            {
                pod_allocator<T>::deallocate(m_blocks[--m_num_blocks], block_size);
            }
            if(m_num_blocks == 0)
            {
                pod_allocator<T*>::deallocate(m_blocks);
                m_blocks = 0;
                m_max_blocks = 0;
            }
            m_size = size;
        }
    }

    template<typename T, unsigned S>
    requires std::is_trivially_copyable_v<T> && (S > 0)
    pod_bvector<T, S>::pod_bvector() :
        m_size(0),
        m_num_blocks(0),
        m_max_blocks(0),
        m_blocks(0),
        m_block_ptr_inc(block_size)
    {
    }

    template<typename T, unsigned S>
    requires std::is_trivially_copyable_v<T> && (S > 0)
    pod_bvector<T, S>::pod_bvector(unsigned block_ptr_inc) :
        m_size(0),
        m_num_blocks(0),
        m_max_blocks(0),
        m_blocks(0),
        m_block_ptr_inc(block_ptr_inc)
    {
    }

    template<typename T, unsigned S>
    requires std::is_trivially_copyable_v<T> && (S > 0)
    pod_bvector<T, S>::pod_bvector(const pod_bvector<T, S>& v) :
        m_size(v.m_size),
        m_num_blocks(v.m_num_blocks),
        m_max_blocks(v.m_max_blocks),
        m_blocks(v.m_max_blocks ? new T * [v.m_max_blocks] : 0),
        m_block_ptr_inc(v.m_block_ptr_inc)
    {
        unsigned i;
        for(i = 0; i < v.m_num_blocks; ++i) {
            m_blocks[i] = new T[block_size];
            memcpy(m_blocks[i], v.m_blocks[i], block_size * sizeof(T));
        }
    }


    //------------------------------------------------------------------------
    template<typename T, unsigned S>
    requires std::is_trivially_copyable_v<T> && (S > 0)
    const pod_bvector<T, S>& pod_bvector<T, S>::operator = (const pod_bvector<T, S>& v)
    {
        unsigned i;
        for(i = m_num_blocks; i < v.m_num_blocks; ++i)
        {
            allocate_block(i);
        }
        for(i = 0; i < v.m_num_blocks; ++i)
        {
            memcpy(m_blocks[i], v.m_blocks[i], block_size * sizeof(T));
        }
        m_size = v.m_size;
        return *this;
    }

    template<typename T, unsigned S>
    requires std::is_trivially_copyable_v<T> && (S > 0)
    void pod_bvector<T, S>::allocate_block(unsigned nb)
    {
        if(nb >= m_max_blocks)
        {
            T** new_blocks = new T * [m_max_blocks + m_block_ptr_inc];

            if(m_blocks)
            {
                memcpy(new_blocks,
                       m_blocks,
                       m_num_blocks * sizeof(T*));

                pod_allocator<T*>::deallocate(m_blocks);
            }
            m_blocks = new_blocks;
            m_max_blocks += m_block_ptr_inc;
        }
        m_blocks[nb] = new T [block_size];
        m_num_blocks++;
    }

    template<typename T, unsigned S>
    requires std::is_trivially_copyable_v<T> && (S > 0)
    inline T* pod_bvector<T, S>::data_ptr()
    {
        unsigned nb = m_size >> block_shift;
        if(nb >= m_num_blocks) allocate_block(nb);
        return m_blocks[nb] + (m_size & block_mask);
    }

    template<typename T, unsigned S>
    requires std::is_trivially_copyable_v<T> && (S > 0)
    inline void pod_bvector<T, S>::add(const T& val)
    {
        *data_ptr() = val;
        ++m_size;
    }

    template<typename T, unsigned S>
    requires std::is_trivially_copyable_v<T> && (S > 0)
    inline void pod_bvector<T, S>::remove_last()
    {
        if(m_size) --m_size;
    }

    template<typename T, unsigned S>
    requires std::is_trivially_copyable_v<T> && (S > 0)
    void pod_bvector<T, S>::modify_last(const T& val)
    {
        remove_last();
        add(val);
    }


    //------------------------------------------------------------------------
    template<typename T, unsigned S>
    requires std::is_trivially_copyable_v<T> && (S > 0)
    int pod_bvector<T, S>::allocate_continuous_block(unsigned num_elements)
    {
        if(num_elements < block_size)
        {
            data_ptr(); // Allocate initial block if necessary
            unsigned rest = block_size - (m_size & block_mask);
            unsigned index;
            if(num_elements <= rest)
            {
                // The rest of the block is good, we can use it
                //-----------------
                index = m_size;
                m_size += num_elements;
                return index;
            }

            // New block
            //---------------
            m_size += rest;
            data_ptr();
            index = m_size;
            m_size += num_elements;
            return index;
        }
        return -1; // Impossible to allocate
    }


    //------------------------------------------------------------------------
    template<typename T, unsigned S>
    requires std::is_trivially_copyable_v<T> && (S > 0)
    unsigned pod_bvector<T, S>::byte_size() const
    {
        return m_size * sizeof(T);
    }


    //------------------------------------------------------------------------
    template<typename T, unsigned S>
    requires std::is_trivially_copyable_v<T> && (S > 0)
    void pod_bvector<T, S>::serialize(int8u* ptr) const
    {
        unsigned i;
        for(i = 0; i < m_size; i++)
        {
            memcpy(ptr, &(*this)[i], sizeof(T));
            ptr += sizeof(T);
        }
    }

    //------------------------------------------------------------------------
    template<typename T, unsigned S>
    requires std::is_trivially_copyable_v<T> && (S > 0)
    void pod_bvector<T, S>::deserialize(const int8u* data, unsigned byte_size)
    {
        remove_all();
        byte_size /= sizeof(T);
        for(unsigned i = 0; i < byte_size; ++i)
        {
            T* ptr = data_ptr();
            memcpy(ptr, data, sizeof(T));
            ++m_size;
            data += sizeof(T);
        }
    }


    // Replace or add a number of elements starting from "start" position
    //------------------------------------------------------------------------
    template<typename T, unsigned S>
    requires std::is_trivially_copyable_v<T> && (S > 0)
    void pod_bvector<T, S>::deserialize(unsigned start, const T& empty_val,
                                        const int8u* data, unsigned byte_size)
    {
        while(m_size < start)
        {
            add(empty_val);
        }

        byte_size /= sizeof(T);
        for(unsigned i = 0; i < byte_size; ++i)
        {
            if(start + i < m_size)
            {
                memcpy(&((*this)[start + i]), data, sizeof(T));
            }
            else
            {
                T* ptr = data_ptr();
                memcpy(ptr, data, sizeof(T));
                ++m_size;
            }
            data += sizeof(T);
        }
    }






    //------------------------------------------------------------------------
    enum quick_sort_threshold_e
    {
        quick_sort_threshold = 9
    };


    //-----------------------------------------------------------swap_elements
    template<typename T>
    constexpr void swap_elements(T& a, T& b) noexcept {
        std::swap(a, b);
    }


    //--------------------------------------------------------------quick_sort
    template<typename Array, typename Less>
    requires requires(Array& arr, Less less, typename Array::value_type a, typename Array::value_type b) {
        arr.size();
        arr[0];
        less(a, b);
    }
    void quick_sort(Array& arr, Less less) {
        if (arr.size() < 2) [[unlikely]] return;

        typename Array::value_type* e1;
        typename Array::value_type* e2;

        int  stack[80];
        int* top = stack;
        int  limit = arr.size();
        int  base = 0;

        for(;;)
        {
            int len = limit - base;

            int i;
            int j;
            int pivot;

            if (len > quick_sort_threshold) [[likely]]
            {
                // we use base + len/2 as the pivot
                pivot = base + len / 2;
                swap_elements(arr[base], arr[pivot]);

                i = base + 1;
                j = limit - 1;

                // now ensure that *i <= *base <= *j with branch prediction hints
                e1 = &(arr[j]);
                e2 = &(arr[i]);
                if (less(*e1, *e2)) [[unlikely]] swap_elements(*e1, *e2);

                e1 = &(arr[base]);
                e2 = &(arr[i]);
                if (less(*e1, *e2)) [[unlikely]] swap_elements(*e1, *e2);

                e1 = &(arr[j]);
                e2 = &(arr[base]);
                if (less(*e1, *e2)) [[unlikely]] swap_elements(*e1, *e2);

                for(;;)
                {
                    do i++; while( less(arr[i], arr[base]) );
                    do j--; while( less(arr[base], arr[j]) );

                    if (i > j) [[unlikely]] {
                        break;
                    }

                    swap_elements(arr[i], arr[j]);
                }

                swap_elements(arr[base], arr[j]);

                // now, push the largest sub-array
                if (j - base > limit - i) [[likely]] {
                    top[0] = base;
                    top[1] = j;
                    base   = i;
                } else {
                    top[0] = i;
                    top[1] = limit;
                    limit  = j;
                }
                top += 2;
            }
            else
            {
                // the sub-array is small, perform insertion sort
                j = base;
                i = j + 1;

                for(; i < limit; j = i, i++)
                {
                    for(; less(*(e1 = &(arr[j + 1])), *(e2 = &(arr[j]))); j--)
                    {
                        swap_elements(*e1, *e2);
                        if(j == base)
                        {
                            break;
                        }
                    }
                }
                if(top > stack)
                {
                    top  -= 2;
                    base  = top[0];
                    limit = top[1];
                }
                else
                {
                    break;
                }
            }
        }
    }




    //------------------------------------------------------remove_duplicates
    // Remove duplicates from a sorted array. It doesn't cut the
    // tail of the array, it just returns the number of remaining elements.
    //-----------------------------------------------------------------------
    template<typename Array, typename Equal>
    requires requires(Array& arr, Equal equal, typename Array::value_type a, typename Array::value_type b) {
        arr.size();
        arr[0];
        equal(a, b);
    }
    constexpr unsigned remove_duplicates(Array& arr, Equal equal) {
        const auto size = arr.size();
        if (size < 2) [[unlikely]] return size;

        unsigned write_pos = 1;

        // Optimized loop with better cache access patterns and move semantics
        for (unsigned read_pos = 1; read_pos < size; ++read_pos) {
            if (!equal(arr[read_pos], arr[read_pos - 1])) [[likely]] {
                if (write_pos != read_pos) [[likely]] {
                    arr[write_pos] = std::move(arr[read_pos]);
                }
                ++write_pos;
            }
        }
        return write_pos;
    }

    //--------------------------------------------------------invert_container
    template<typename Array>
    constexpr void invert_container(Array& arr) noexcept {
        const auto size = arr.size();
        if (size < 2) [[unlikely]] return;

        // Optimized with pointer arithmetic for better performance
        auto* begin = &arr[0];
        auto* end = &arr[size - 1];

        while (begin < end) {
            std::swap(*begin++, *end--);
        }
    }

    //------------------------------------------------------binary_search_pos
    template<typename Array, typename Value, typename Less>
    requires requires(const Array& arr, const Value& val, Less less) {
        arr.size();
        arr[0];
        less(val, arr[0]);
    }
    constexpr unsigned binary_search_pos(const Array& arr, const Value& val, Less less) {
        if(arr.size() == 0) return 0;

        unsigned beg = 0;
        unsigned end = arr.size() - 1;

        if(less(val, arr[0])) return 0;
        if(less(arr[end], val)) return end + 1;

        while(end - beg > 1)
        {
            unsigned mid = (end + beg) >> 1;
            if(less(val, arr[mid])) end = mid;
            else                    beg = mid;
        }

        //if(beg <= 0 and less(val, arr[0])) return 0;
        //if(end >= arr.size() - 1 and less(arr[end], val)) ++end;

        return end;
    }

    //----------------------------------------------------------range_adaptor
    template<typename Array>
    requires requires(Array& arr) {
        arr.size();
        arr[0];
        typename Array::value_type;
    }
    class range_adaptor
    {
    public:
        using value_type = typename Array::value_type;

        constexpr range_adaptor(Array& array, unsigned start, unsigned size) noexcept :
            m_array(array), m_start(start), m_size(size)
        {}

        constexpr unsigned size() const noexcept { return m_size; }
        constexpr const value_type& operator [] (unsigned i) const noexcept { return m_array[m_start + i]; }
        constexpr       value_type& operator [] (unsigned i)       noexcept { return m_array[m_start + i]; }
        const value_type& at(unsigned i) const           { return m_array[m_start + i]; }
              value_type& at(unsigned i)                 { return m_array[m_start + i]; }
        value_type  value_at(unsigned i) const           { return m_array[m_start + i]; }

    private:
        Array& m_array;
        unsigned m_start;
        unsigned m_size;
    };

    //---------------------------------------------------------------int_less
    inline bool int_less(int a, int b) { return a < b; }

    //------------------------------------------------------------int_greater
    inline bool int_greater(int a, int b) { return a > b; }

    //----------------------------------------------------------unsigned_less
    inline bool unsigned_less(unsigned a, unsigned b) { return a < b; }

    //-------------------------------------------------------unsigned_greater
    inline bool unsigned_greater(unsigned a, unsigned b) { return a > b; }
}
