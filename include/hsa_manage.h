#include<type_traits>
#pragma once
#include <malloc.h>
#include <string.h>
namespace Concurrency {
namespace CLAMP {
namespace HSA {
extern void RegisterMemory(void *p, size_t sz);
}
}

template<class T>
class CLAllocator
{
 public:
  typedef T value_type;
  T* allocate(unsigned n) {
    T *p = (T*)memalign(0x1000, n*sizeof(T));
    CLAMP::HSA::RegisterMemory(p, sizeof(value_type) * n);
    return p;
  }
};

template<class T>
class CLDeleter {
 public:
  void operator()(T* ptr) {
    free(ptr);
  }
};

// Dummy interface that looks somewhat like std::shared_ptr<T>
template <typename T>
class _data {
  typedef typename std::remove_const<T>::type nc_T;
  friend _data<const T>;
  friend _data<nc_T>;
 public:
  _data() = delete;
  _data(const _data& d) restrict(cpu, amp):p_(d.p_) {}
  template <class = typename std::enable_if<std::is_const<T>::value>::type>
    _data(const _data<nc_T>& d) restrict(cpu, amp):p_(d.p_) {}
  template <class = typename std::enable_if<!std::is_const<T>::value>::type>
    _data(const _data<const T>& d) restrict(cpu, amp):p_(const_cast<T*>(d.p_)) {}
  template <typename T2>
    _data(const _data<T2>& d) restrict(cpu, amp):p_(reinterpret_cast<T *>(d.get())) {}
  __attribute__((annotate("user_deserialize")))
  explicit _data(__global T* t) restrict(cpu, amp) { p_ = t; }
  __global T* get(void) const restrict(cpu, amp) { return p_; }
  __global T* get_mutable(void) const restrict(cpu, amp) { return p_; }
  __global T* get_data() const { return get(); }
  void reset(__global T *t = NULL) restrict(cpu, amp) { p_ = t; }
 private:
  __global T* p_;
};


template <typename T>
class _data_host: public std::shared_ptr<T> {
 public:
  _data_host(const _data_host &other):std::shared_ptr<T>(other) {}
  template <class = typename std::enable_if<!std::is_const<T>::value>::type>
  _data_host(const _data_host<const T> &other):std::shared_ptr<T>(other) {}
  _data_host(std::nullptr_t x = nullptr):std::shared_ptr<T>(nullptr) {}

  __attribute__((annotate("serialize")))
  void __cxxamp_serialize(Serialize& s) const {
    s.AppendPtr((const void *)std::shared_ptr<T>::get());
  }
  __attribute__((annotate("user_deserialize")))
  explicit _data_host(__global T* t);
};
//enum used to track the cache state.
//HOST_OWNED: Most up to date version in the home location and will need to
//be copied back to the CL buffer before calling a kernel.
//CL_OWNED: data was used in a kernel invocation and is presumed to have
//been dirtied by it. Most up to date version will be in the CL buffer.
//SHARED: Data has been copied from the CL buffer back to the home location
//but has not been modified yet. Both buffers have the most up to date version.
namespace { typedef enum {
HOST_OWNED,
CL_OWNED,
SHARED
} cache_state; }

// Wrap a shared pointer to the CL buffer and the cache state. Act
// as if a shared pointer to the CL buffer itself.
template <typename T>
class _data_host_view {
 private:
  typedef typename std::remove_const<T>::type nc_T;
  friend _data_host_view<const T>;
  friend _data_host_view<nc_T>;
  template <typename T2> friend class _data_host_view;

  __attribute__((cpu)) std::shared_ptr<nc_T> cl_buffer;
  __attribute__((cpu)) std::shared_ptr<cache_state> state_ptr;
  __attribute__((cpu)) T* home_ptr;
  __attribute__((cpu)) size_t buffer_size;

 public:
  std::shared_ptr<nc_T> get_gmac_buffer() const { return gmac_buffer; }
  T *get_home_ptr() const {
    if (home_ptr) {
      synchronize();
      *state_ptr = HOST_OWNED;
      return home_ptr;
    }
    return nullptr;
  }
  std::shared_ptr<cache_state> get_state_ptr() const { return state_ptr; }
  size_t get_buffer_size() const { return buffer_size; }

  _data_host_view(nc_T* cache, T* home, size_t size) :
   cl_buffer(cache), home_ptr(home), state_ptr(new cache_state), buffer_size(size) {
    *state_ptr = HOST_OWNED;
  }

  template <class Deleter>
  _data_host_view(nc_T* cache, Deleter d, T* home, size_t size) :
   cl_buffer(cache, d), home_ptr(home), state_ptr(new cache_state), buffer_size(size) {
    *state_ptr = HOST_OWNED;
  }

  __attribute__((annotate("user_deserialize")))
  _data_host_view(T* cache) :
   gmac_buffer((nc_T*)(cache)), home_ptr(nullptr), state_ptr(new cache_state), buffer_size(0) {
    *state_ptr = GMAC_OWNED;
  };

  template <class Deleter>
  _data_host_view(nc_T* cache, Deleter d) :
   cl_buffer(cache, d), home_ptr(nullptr), state_ptr(new cache_state), buffer_size(0) {
    *state_ptr = CL_OWNED;
  }

  _data_host_view(const _data_host_view<T> &other) :
    cl_buffer(other.cl_buffer), state_ptr(other.state_ptr),
    home_ptr(other.home_ptr), buffer_size(other.buffer_size) {}

  template <class = typename std::enable_if<!std::is_const<T>::value>::type>
  _data_host_view(const _data_host_view<const T> &other) :
    gmac_buffer(other.gmac_buffer), state_ptr(other.state_ptr),
    home_ptr(const_cast<T*>(other.home_ptr)), buffer_size(other.buffer_size) {}

  template <typename ElementType>
  _data_host_view(const _data_host_view<ElementType> &other) :
    gmac_buffer(std::static_pointer_cast<nc_T>(std::static_pointer_cast<void>(other.get_gmac_buffer()))), state_ptr(other.get_state_ptr()),
    home_ptr(reinterpret_cast<T *>(other.get_home_ptr())), buffer_size(other.get_buffer_size()) {}

  template <typename ElementType>
  _data_host_view(const _data_host_view<const ElementType> &other) :
    gmac_buffer(std::static_pointer_cast<T>(std::static_pointer_cast<void>(other.get_gmac_buffer()))), state_ptr(other.get_state_ptr()),
    home_ptr(reinterpret_cast<T *>(const_cast<T*>(other.get_home_ptr()))), buffer_size(other.get_buffer_size()) {}

  _data_host_view(const _data_host<T> &other) :
    cl_buffer(other), home_ptr(nullptr), buffer_size(0) {}

  _data_host_view(std::nullptr_t x = nullptr):cl_buffer(nullptr), buffer_size(0) {}

  template <class = typename std::enable_if<std::is_const<T>::value>::type>
  _data_host_view(const _data_host<nc_T> &other) :
    cl_buffer(other), home_ptr(nullptr), buffer_size(0) {}

  template <class = typename std::enable_if<std::is_const<T>::value>::type>
  _data_host_view(const _data_host_view<nc_T> &other) :
    cl_buffer(other.cl_buffer), state_ptr(other.state_ptr),
    home_ptr(other.home_ptr), buffer_size(other.buffer_size) {}

  void reset() {
    cl_buffer.reset();
    state_ptr.reset();
  }

//The host buffer was modified without going through the array_view interface.
//Set it host owned so we know it is dirty and will copy it back to the OpenCL
//buffer when we serialize.
  void refresh() const { 
//    *state_ptr = HOST_OWNED; 
    if (home_ptr) {
      memcpy(reinterpret_cast<void*>(gmac_buffer.get()),
             reinterpret_cast<const void*>(home_ptr), buffer_size);
    }
  }

//If the cache is currently owned by the cl buffer, copy it back to the host
//buffer and change the state to shared.
  void synchronize() const {
    if (*state_ptr == CL_OWNED) {
      memcpy(const_cast<void*>(reinterpret_cast<const void*>(home_ptr)),
              reinterpret_cast<const void*>(cl_buffer.get()), buffer_size);
      *state_ptr = SHARED;
    }
  }

//Check the CL buffer use count to see if we are the last copy of it. This
//is used to check if we are the last copy of an array_view and need to
//implicitly synchronize.
  bool is_last() const { return cl_buffer.unique(); }

//Return the home location ptr, synchronizing first if necessary. The pointer
//returned is mutable, so we set it as host owned. If this is an array or array_view
//without a host buffer just return a pointer to the cl buffer.
  T* get_mutable() const {
    if (home_ptr) {
      synchronize();
      *state_ptr = HOST_OWNED;
      return home_ptr;
    } else {
      return cl_buffer.get();
    }
  }
  T* get() const {
    return get_mutable();
  }

//Return the home location ptr, synchronizing first if necessary. The pointer
//returned is const, so we can leave the cache in a shared state. If this is
//an array or array_view without a host buffer just return a pointer to the cl buffer.
  const T* get() {
    if (home_ptr) {
      synchronize();
      return home_ptr;
    } else {
      return cl_buffer.get();
    }
  }

//This is the serialization done by the runtime when an object is used in as a
//kernel argument. By overloading it we can do special work here instead of
//just using the compiler provided version. First, we copy the data back to
//the cl buffer if the cache is currently owned by the host. Instead of
//appending in the normal way we append the cl buffer pointer. This removes
//the cache and shared pointer cruft, and the cl runtime will translate the
//pointer to a ponter to global memory in the kernel.
  __attribute__((annotate("serialize")))
  void __cxxamp_serialize(Serialize& s) const {
    if (home_ptr && *state_ptr == HOST_OWNED) {
      memcpy(reinterpret_cast<void*>(cl_buffer.get()),
              reinterpret_cast<const void*>(home_ptr), buffer_size);
      *state_ptr = CL_OWNED;
    }
    s.AppendPtr((const void *)cl_buffer.get());
  }

};
} // namespace Concurrency
