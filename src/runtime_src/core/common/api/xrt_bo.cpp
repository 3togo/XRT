/*
 * Copyright (C) 2020-2022, Xilinx Inc - All rights reserved
 * Xilinx Runtime (XRT) Experimental APIs
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

// This file implements XRT xclbin APIs as declared in
// core/include/experimental/xrt_bo.h
#define XCL_DRIVER_DLL_EXPORT  // exporting xrt_bo.h
#define XRT_CORE_COMMON_SOURCE // in same dll as core_common
#include "core/include/experimental/xrt_bo.h"
#include "core/include/experimental/xrt_aie.h"
#include "native_profile.h"
#include "bo.h"

#include "device_int.h"
#include "handle.h"
#include "kernel_int.h"
#include "core/common/device.h"
#include "core/common/memalign.h"
#include "core/common/message.h"
#include "core/common/query_requests.h"
#include "core/common/system.h"
#include "core/common/unistd.h"
#include "core/common/xclbin_parser.h"

#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
# pragma warning( disable : 4244 4100 4996 4505 )
#endif

namespace {

XRT_CORE_UNUSED
static bool
is_noop_emulation()
{
  static auto xem = std::getenv("XCL_EMULATION_MODE");
  static bool noop = xem ? (std::strcmp(xem,"noop")==0) : false;
  return noop;
}

XRT_CORE_UNUSED
static bool
is_sw_emulation()
{
  static auto xem = std::getenv("XCL_EMULATION_MODE");
  static bool swemu = xem ? (std::strcmp(xem,"sw_emu")==0) : false;
  return swemu;
}

inline bool
is_nodma(const xrt_core::device* core_device)
{
  return core_device->is_nodma();
}

inline bool
is_nodma(const xrt::device& device)
{
  return is_nodma(device.get_handle().get());
}

inline bool
is_nodma(xclDeviceHandle xhdl)
{
  return is_nodma(xrt_core::get_userpf_device(xhdl).get());
}

}

////////////////////////////////////////////////////////////////
// Exposed for Vitis aietools as extensions to xrt_bo.h
// Revisit post 2020.1
////////////////////////////////////////////////////////////////
// Removed as address was exposed through public API per request
///////////////////////////////////////////////////////////////

namespace {

inline size_t
get_alignment()
{
  return xrt_core::getpagesize();
}

inline  bool
is_aligned_ptr(const void* p)
{
  return p && (reinterpret_cast<uintptr_t>(p) % get_alignment())==0;
}

inline void
send_exception_message(const char* msg)
{
  xrt_core::message::send(xrt_core::message::severity_level::error, "XRT", msg);
}

inline void
send_exception_message(const std::string& msg)
{
  send_exception_message(msg.c_str());
}

} // namespace

namespace xrt {

// class bo_impl - Base class for buffer objects
//
// [bo_impl]: base class
//
// Derived classes:
// [hbuf]: host side buffer allocated in virtual memory and managed by XRT
// [ubuf]: host side buffer allocated and managed by user
// [kbuf]: host side buffer allocated and managed by kernel driver
// [sub]:  sub buffer
//
// Add new sub classes as needed, for example p2p, cma, svm.
//
// Life time of buffers are managed through shared pointers.
// A buffer is freed when last references is released.
class bo_impl
{
public:
  static constexpr uint64_t no_addr = std::numeric_limits<uint64_t>::max();
  static constexpr int32_t no_group = std::numeric_limits<int32_t>::max();
  static constexpr bo::flags no_flags = static_cast<bo::flags>(std::numeric_limits<uint32_t>::max());
  static constexpr xclBufferHandle null_bo = XRT_NULL_BO;
  static constexpr xclBufferExportHandle null_export = XRT_NULL_BO_EXPORT;

private:
  void
  get_bo_properties() const
  {
    xclBOProperties prop{};
    device->get_bo_properties(handle, &prop);
    addr = prop.paddr;
    grpid = prop.flags & XRT_BO_FLAGS_MEMIDX_MASK;
    flags = static_cast<bo::flags>(prop.flags & ~XRT_BO_FLAGS_MEMIDX_MASK);

#ifdef _WIN32 // All shims minus windows return proper flags
    // Remove when driver returns the flags that were used to ctor the bo
    auto mem_topo = device->get_axlf_section<const ::mem_topology*>(ASK_GROUP_TOPOLOGY);
    grpid = xrt_core::xclbin::address_to_memidx(mem_topo, addr);
#endif
  }

protected:
  // deliberately made protected, this is a file-scoped controlled API
  std::shared_ptr<xrt_core::device> device;     // NOLINT device where bo is allocated
  std::vector<std::shared_ptr<bo_impl>> clones; // NOLINT local m2m clones if any
  xclBufferHandle handle = null_bo;             // NOLINT driver bo handle
  size_t size = 0;                              // NOLINT size of buffer
  mutable uint64_t addr = no_addr;              // NOLINT bo device address
  mutable int32_t grpid = no_group;             // NOLINT memory group index
  mutable bo::flags flags = no_flags;           // NOLINT flags per bo properties
  mutable xclBufferExportHandle export_handle = null_export; // NOLINT export handle if exported
  bool free_bo;                                 // NOLINT should dtor free bo

public:
  explicit bo_impl(size_t sz)
    : handle(XRT_NULL_BO), size(sz), free_bo(false)
  {}

  bo_impl(xclDeviceHandle dhdl, xclBufferHandle bhdl, size_t sz)
    : device(xrt_core::get_userpf_device(dhdl)), handle(bhdl), size(sz), free_bo(true)
  {}

  bo_impl(xclDeviceHandle dhdl, xclBufferExportHandle ehdl)
    : device(xrt_core::get_userpf_device(dhdl)), handle(device->import_bo(ehdl)), free_bo(true)
  {
    xclBOProperties prop{};
    device->get_bo_properties(handle, &prop);
    size = prop.size;
  }

  bo_impl(xclDeviceHandle dhdl, pid_type pid, xclBufferExportHandle ehdl)
    : device(xrt_core::get_userpf_device(dhdl)), handle(device->import_bo(pid.pid, ehdl)), free_bo(true)
  {
    xclBOProperties prop{};
    device->get_bo_properties(handle, &prop);
    size = prop.size;
  }

  bo_impl(xclDeviceHandle dhdl, xcl_buffer_handle xhdl)
    : device(xrt_core::get_userpf_device(dhdl)), handle(xhdl.bhdl), free_bo(false)
  {
    xclBOProperties prop{};
    device->get_bo_properties(handle, &prop);
    size = prop.size;
  }

  bo_impl(const bo_impl* parent, size_t sz)
    : device(parent->device), handle(parent->handle), size(sz), free_bo(false)
  {}

  virtual
  ~bo_impl()
  {
    if (export_handle != null_export)
      device->close_export_handle(export_handle);
    if (free_bo)
      device->free_bo(handle);
  }

  bo_impl(const bo_impl&) = delete;
  bo_impl(bo_impl&&) = delete;
  bo_impl& operator=(bo_impl&) = delete;
  bo_impl& operator=(bo_impl&&) = delete;

  // BOs can be cloned internally by XRT to statisfy kernel
  // connectivity, the lifetime of a cloned BO is tied to the
  // lifetime of the BO from which is was cloned.
  void
  add_clone(std::shared_ptr<bo_impl> clone)
  {
    clones.push_back(std::move(clone));
  }

  xclBufferHandle
  get_xcl_handle() const
  {
    return handle;
  }

  const xrt_core::device*
  get_device() const
  {
    return device.get();
  }

  xclBufferExportHandle
  export_buffer() const
  {
    if (export_handle == null_export)
      export_handle = device->export_bo(handle);

    return export_handle;
  }

  void
  write(const void* src, size_t sz, size_t seek)
  {
    if (sz + seek > size)
      throw xrt_core::error(-EINVAL,"attempting to write past buffer size");
    auto hbuf = static_cast<char*>(get_hbuf()) + seek;
    std::memcpy(hbuf, src, sz);
  }

  void
  read(void* dst, size_t sz, size_t skip)
  {
    if (sz + skip > size)
      throw xrt_core::error(-EINVAL,"attempting to read past buffer size");
    auto hbuf = static_cast<char*>(get_hbuf()) + skip;
    std::memcpy(dst, hbuf, sz);
  }

  virtual void
  copy(const bo_impl* src, size_t sz, size_t src_offset, size_t dst_offset)
  {
    // Check size and offset of dst and src
    if (!sz)
      throw xrt_core::system_error(EINVAL, "size must be a positive number");
    if (sz + dst_offset > size)
      throw xrt_core::system_error(EINVAL, "copying past destination buffer size");
    if (src->get_size() < sz + src_offset)
      throw xrt_core::system_error(EINVAL, "copying past source buffer size");

    if (get_device() != src->get_device()) {
      copy_with_export(src, sz, src_offset, dst_offset);
      return;
    }

    // try copying with m2m
    try {
      auto m2m = xrt_core::device_query<xrt_core::query::m2m>(get_device());
      if (xrt_core::query::m2m::to_bool(m2m)) {
        device->copy_bo(get_xcl_handle(), src->get_xcl_handle(), sz, dst_offset, src_offset);
        return;
      }
    }
    catch (const std::exception&) {
    }

    // try copying with kdma
    try {
      if (xrt_core::config::get_cdma()) {
        xrt_core::kernel_int::copy_bo_with_kdma
          (device, sz, get_xcl_handle(), dst_offset, src->get_xcl_handle(), src_offset);
        return;
      }
    }
    catch (const std::exception& ex) {
      auto fmt = boost::format("Reverting to host copy of buffers (%s)") % ex.what();
      xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",  fmt.str());
    }

    // special case sw emulation on imported buffers
    if (is_sw_emulation() && (is_imported() || src->is_imported())) {
      device->copy_bo(get_xcl_handle(), src->get_xcl_handle(), sz, dst_offset, src_offset);
      return;
    }

    // revert to copying through host
    copy_through_host(src, sz, src_offset, dst_offset);
  }

  void
  copy_with_export(const bo_impl* src, size_t sz, size_t src_offset, size_t dst_offset)
  {
    // export bo from other device and create an import bo to copy from
    auto src_export_handle = src->export_buffer();
    auto src_import_bo = xrt::bo(device->get_user_handle(), src_export_handle);
    copy(src_import_bo.get_handle().get(), sz, src_offset, dst_offset);
  }

  void
  copy_through_host(const bo_impl* src, size_t sz, size_t src_offset, size_t dst_offset)
  {
    auto src_hbuf = static_cast<const char*>(src->get_hbuf());
    if (!src_hbuf)
      throw xrt_core::system_error(EINVAL, "No host side buffer in source buffer");

    auto dst_hbuf = static_cast<char*>(get_hbuf());
    if (!dst_hbuf)
      throw xrt_core::system_error(EINVAL, "No host side buffer in destination buffer");

    // sync to src to ensure data integrity, logically const
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) // special case
    const_cast<bo_impl*>(src)->sync(XCL_BO_SYNC_BO_FROM_DEVICE, sz, src_offset);

    // copy host side buffer
    std::memcpy(dst_hbuf + dst_offset, src_hbuf + src_offset, sz);

    // sync modified host buffer to device
    sync(XCL_BO_SYNC_BO_TO_DEVICE, sz, dst_offset);
  }

#ifdef XRT_ENABLE_AIE
  void
  sync(xrt::bo& bo, const std::string& port, xclBOSyncDirection dir, size_t sz, size_t offset)
  {
    device->sync_aie_bo(bo, port.c_str(), dir, sz, offset);
  }
#endif

  virtual void
  sync(xclBOSyncDirection dir, size_t sz, size_t offset)
  {
    // One may think that host_only BOs should not be synced, but here
    // is the deal: The sync does not really do DMA, but just flush
    // the CPU cache (to_device) so that device will get the most
    // up-to-date data from physical memory or invalid CPU cache
    // (from_device) so that host CPU can read the most up-to-date
    // data device has put into the physical memory. As of today, all
    // Xilinx's Alveo devices will automatically trigger cache
    // coherence actions when it reads from or write to physical
    // memory, but we still recommend user to perform explicit BO sync
    // operation just in case the HW changes in the future.
    // if (get_flags() != bo::flags::host_only)
    device->sync_bo(handle, dir, sz, offset);
  }

  virtual uint64_t
  get_address() const
  {
    if (addr == no_addr)
      get_bo_properties();

    return addr;
  }

  virtual uint64_t
  get_group_id() const
  {
    if (grpid == no_group)
      get_bo_properties();

    return grpid;
  }

  virtual bo::flags
  get_flags() const
  {
    if (flags == no_flags)
      get_bo_properties();

    return flags;
  }

  virtual size_t get_size()      const { return size;    }
  virtual void*  get_hbuf()      const { return nullptr; }
  virtual bool   is_sub()        const { return false;   }
  virtual bool   is_imported()   const { return false;   }
};


// class buffer_ubuf - User provide host side buffer
//
// Provided buffer must be aligned or exception is thrown
class buffer_ubuf : public bo_impl
{
  void* ubuf;

public:
  buffer_ubuf(xclDeviceHandle dhdl, xclBufferHandle bhdl, size_t sz, void* buf)
    : bo_impl(dhdl, bhdl, sz)
    , ubuf(buf)
  {}

  void*
  get_hbuf() const override
  {
    return ubuf;
  }
};


// class buffer_hbuf - XRT allocated host side buffer
//
// XRT allocated host side buffer.  The host side buffer
// is allocated in virtual memory on user space side.
class buffer_hbuf : public bo_impl
{
  xrt_core::aligned_ptr_type hbuf;

public:
  buffer_hbuf(xclDeviceHandle dhdl, xclBufferHandle bhdl, size_t sz, xrt_core::aligned_ptr_type&& b)
    : bo_impl(dhdl, bhdl, sz), hbuf(std::move(b))
  {}

  void*
  get_hbuf() const override
  {
    return hbuf.get();
  }
};

// class buffer_kbuf - Kernel driver host side buffer
//
// Kernel driver allocated host side buffer.  The host side buffer
// is allocated in physical memory by kernel driver.
class buffer_kbuf : public bo_impl
{
  void* hbuf;

public:
  buffer_kbuf(xclDeviceHandle dhdl, xclBufferHandle bhdl, size_t sz)
    : bo_impl(dhdl, bhdl, sz), hbuf(device->map_bo(handle, true))
  {}

  ~buffer_kbuf() override
  {
    // Imported BO can fail in xclUnmapBO if the exported BO has
    // already been unmapped or vice versa.
    try {
      device->unmap_bo(handle, hbuf);
    }
    catch (...) {
    }
  }

  buffer_kbuf(const buffer_kbuf&) = delete;
  buffer_kbuf(buffer_kbuf&&) = delete;
  buffer_kbuf& operator=(buffer_kbuf&) = delete;
  buffer_kbuf& operator=(buffer_kbuf&&) = delete;

  void*
  get_hbuf() const override
  {
    return hbuf;
  }
};

// class buffer_imported - Buffer imported from another device
//
// The exported buffer handle is an opaque type from a call
// to export_buffer() on a buffer to be exported.  The exported
// buffer can be imported within same process or from another
// process (linux pidfd support required)
class buffer_import : public bo_impl
{
  void* hbuf;

public:
  // buffer_import() - Import the buffer
  //
  // @device:  device to import to
  // @ehdl:    export handle obtained by calling export_buffer
  buffer_import(xclDeviceHandle dhdl, xclBufferExportHandle ehdl)
    : bo_impl(dhdl, ehdl)
  {
    try {
      hbuf = device->map_bo(handle, true);
    }
    catch (const std::exception&) {
      hbuf = nullptr;
    }
  }

  // buffer_import() - Import the buffer from another process
  //
  // @device:  device to import to
  // @pid:     process id of exporting process
  // @ehdl:    export handle obtained from exporting process
  //
  // This consrructor works on linux only and require pidfd support in
  // linux kernel.
  buffer_import(xclDeviceHandle dhdl, pid_type pid, xclBufferExportHandle ehdl)
    : bo_impl(dhdl, pid, ehdl)
  {
    try {
      hbuf = device->map_bo(handle, true);
    }
    catch (const std::exception&) {
      hbuf = nullptr;
    }
  }

  ~buffer_import() override
  {
    // Imported BO can fail in xclUnmapBO if the exported BO has
    // already been unmapped or vice versa.
    try {
      device->unmap_bo(handle, hbuf);
    }
    catch (...) {
    }
  }

  buffer_import(const buffer_import&) = delete;
  buffer_import(buffer_import&&) = delete;
  buffer_import& operator=(buffer_import&) = delete;
  buffer_import& operator=(buffer_import&&) = delete;

  bool
  is_imported() const override
  {
    return true;
  }

  void*
  get_hbuf() const override
  {
    if (!hbuf)
      throw xrt_core::system_error(std::errc::bad_address, "No host memory for imported buffer");
    return hbuf;
  }
};

// class buffer_dbuf - device only buffer
//
class buffer_dbuf : public bo_impl
{
public:
  buffer_dbuf(xclDeviceHandle dhdl, xclBufferHandle bhdl, size_t sz)
    : bo_impl(dhdl, bhdl, sz)
  {}

  void*
  get_hbuf() const override
  {
    throw xrt_core::error(-EINVAL, "device only buffer has no host buffer");
  }
};

class buffer_nodma : public bo_impl
{
  buffer_kbuf m_host_only;
  buffer_dbuf m_device_only;

  void
  valid_or_error(size_t sz, size_t offset)
  {
    if (!sz)
      throw xrt_core::system_error(EINVAL, "size must be a positive number");
    if (sz + offset > size)
      throw xrt_core::system_error(EINVAL, "offset exceeds buffer size");
  }

public:
  buffer_nodma(xclDeviceHandle dhdl, xclBufferHandle hbuf, xclBufferHandle dbuf, size_t sz)
    : bo_impl(sz), m_host_only(dhdl, hbuf, sz), m_device_only(dhdl, dbuf, sz)
  {
    device = xrt_core::get_userpf_device(dhdl);
    handle = dbuf;
  }

  void*
  get_hbuf() const override
  {
    return m_host_only.get_hbuf();
  }

  // sync is M2M copy between host and device bo
  // nodma is guaranteed to have M2M
  void
  sync(xclBOSyncDirection dir, size_t sz, size_t offset) override
  {
    if (dir == XCL_BO_SYNC_BO_TO_DEVICE) {
      // dst, src, size, dst_offset, src_offset
      device->copy_bo(m_device_only.get_xcl_handle(), m_host_only.get_xcl_handle(), sz, offset, offset);
    }
    else {
      // dst, src, size, dst_offset, src_offset
      device->copy_bo(m_host_only.get_xcl_handle(), m_device_only.get_xcl_handle(), sz, offset, offset);
    }
  }

  void
  copy(const bo_impl* src, size_t sz, size_t src_offset, size_t dst_offset) override
  {
    // Copy src device bo to dst (this) device bo
    bo_impl::copy(src, sz, src_offset, dst_offset);

    // Copy dst (this) dbuf to dst (this) hbuf
    device->copy_bo(m_host_only.get_xcl_handle(), m_device_only.get_xcl_handle(), sz, dst_offset, dst_offset);
  }
};

// class buffer_sub - Sub buffer
//
// Sub-buffer created from parent buffer
class buffer_sub : public bo_impl
{
  std::shared_ptr<bo_impl> m_parent;  // participate in ownership of parent
  size_t m_offset;
  void* m_hbuf;

public:
  buffer_sub(std::shared_ptr<bo_impl> par, size_t size, size_t off)
    : bo_impl(par.get(), size)
    , m_parent(std::move(par))
    , m_offset(off)
    , m_hbuf(static_cast<char*>(m_parent->get_hbuf()) + m_offset)
  {
    if (size + m_offset > m_parent->get_size())
      throw xrt_core::error(-EINVAL, "sub buffer size and offset");
  }

  void*
  get_hbuf() const override
  {
    return m_hbuf;
  }

  bool
  is_sub() const override
  {
    return true;
  }

  uint64_t
  get_address() const override
  {
    return bo_impl::get_address() + m_offset;
  }

  void
  sync(xclBOSyncDirection dir, size_t sz, size_t offset) override
  {
    size_t off = offset + m_offset;
    if (off + sz > m_parent->get_size())
      throw xrt_core::error(-EINVAL, "Invalid offset and size when syncing sub buffer");

    // sync through parent buffer, which handles nodma case also
    m_parent->sync(dir, sz, off);
  }
};

// class buffer_xbuf - Wrapper for extern managed xclBufferHandle
//
// This class is added to support xrt::bo object for host
// managed xclBufferHandles.  This allows the xclBufferHandle
// to be used as argument for kernel execution.  All other
// operations must be managed explicity by host via xcl APIs.
class buffer_xbuf : public bo_impl
{
public:
  buffer_xbuf(xclDeviceHandle dhdl, xclBufferHandle bhdl)
    : bo_impl(dhdl, xcl_buffer_handle{bhdl})
  {}

  void*
  get_hbuf() const override
  {
    throw xrt_core::error(std::errc::not_supported, "no host buffer access for xcl managed BOs");
  }

  void
  copy(const bo_impl*, size_t, size_t, size_t) override
  {
    throw xrt_core::error(std::errc::not_supported, "no copy of xcl managed BOs");
  }

  void
  sync(xclBOSyncDirection, size_t, size_t) override
  {
    throw xrt_core::error(std::errc::not_supported, "no sync of xcl managed BOs");
  }

  bool
  is_sub() const override
  {
    throw xrt_core::error(std::errc::not_supported, "no sub buffer property for xcl managed BOs");
  }

  bool
  is_imported() const override
  {
    throw xrt_core::error(std::errc::not_supported, "no import property for xcl managed BOs");
  }
};

// class buffer_clone - cloned buffer in different memory bank
//
// A cloned buffer is identical to src buffer except for its physical
// device location (memory group). The clone is valid only as long as
// the src buffer is valid, lifetime of clone is tied to lifetime of
// src per alloc_clone() implementation.
class buffer_clone : public bo_impl
{
public:
  buffer_clone(xclDeviceHandle dhdl, const std::shared_ptr<bo_impl>& src, xclBufferHandle clone, size_t sz)
    : bo_impl(dhdl, clone, sz)
  {
    // copy src to clone
    copy(src.get(), src->get_size(), 0, 0);
  }
};

} // namespace xrt

// Implementation details
namespace {

// C-API handles that must be explicitly closed. Corresponding managed
// handles are inserted in this map.  When the unmanaged handle is
// closed, it is removed from this map and underlying buffer is
// deleted if no other shared ptrs exists for this buffer
static xrt_core::handle_map<xrtBufferHandle, std::shared_ptr<xrt::bo_impl>> bo_cache;

static const std::shared_ptr<xrt::bo_impl>&
get_boh(xrtBufferHandle bhdl)
{
  return bo_cache.get_or_error(bhdl);
}

static xclBufferHandle
alloc_bo(xclDeviceHandle dhdl, void* userptr, size_t sz, xrtBufferFlags flags, xrtMemoryGroup grp)
{
  auto device = xrt_core::get_userpf_device(dhdl);
  flags = (flags & ~XRT_BO_FLAGS_MEMIDX_MASK) | grp;
  return device->alloc_bo(userptr, sz, flags);
}

static xclBufferHandle
alloc_bo(xclDeviceHandle dhdl, size_t sz, xrtBufferFlags flags, xrtMemoryGroup grp)
{
  auto device = xrt_core::get_userpf_device(dhdl);
  auto xflags = (flags & ~XRT_BO_FLAGS_MEMIDX_MASK) | grp;
  try {
    return device->alloc_bo(sz, xflags);
  }
  catch (const std::exception& ex) {
    if (flags == XRT_BO_FLAGS_HOST_ONLY) {
      auto fmt = boost::format("Failed to allocate host memory buffer (%s), make sure host bank is enabled "
                               "(see xbutil configure --host-mem)") % ex.what();
      send_exception_message(fmt.str());
    }
    throw;
  }
}

// driver allocates host buffer
static std::shared_ptr<xrt::bo_impl>
alloc_kbuf(xclDeviceHandle dhdl, size_t sz, xrtBufferFlags flags, xrtMemoryGroup grp)
{
  auto handle = alloc_bo(dhdl, sz, flags, grp);
  auto boh = std::make_shared<xrt::buffer_kbuf>(dhdl, handle, sz);
  return boh;
}

static std::shared_ptr<xrt::bo_impl>
alloc_ubuf(xclDeviceHandle dhdl, void* userptr, size_t sz, xrtBufferFlags flags, xrtMemoryGroup grp)
{
  // On NoDMA platforms a userptr would require userspace management
  // of specified userptr with extra memcpy on sync and copy.  If
  // supported then it would hide inefficient application code, so
  // just say no.
  if (is_nodma(dhdl))
    throw xrt_core::error(EINVAL, "userptr is not supported for NoDMA platforms");

  if (flags & XRT_BO_FLAGS_HOST_ONLY)
    throw xrt_core::error(EINVAL, "userptr is not supported for host only buffers");

  // error if userptr is not aligned properly
  if (!is_aligned_ptr(userptr))
    throw xrt_core::error(EINVAL, "userptr is not aligned");

  // driver pins and manages userptr
  auto handle = alloc_bo(dhdl, userptr, sz, flags, grp);
  auto boh = std::make_shared<xrt::buffer_ubuf>(dhdl, handle, sz, userptr);
  return boh;
}

static std::shared_ptr<xrt::bo_impl>
alloc_hbuf(xclDeviceHandle dhdl, xrt_core::aligned_ptr_type&& hbuf, size_t sz, xrtBufferFlags flags, xrtMemoryGroup grp)
{
  auto handle =  alloc_bo(dhdl, hbuf.get(), sz, flags, grp);
  auto boh = std::make_shared<xrt::buffer_hbuf>(dhdl, handle, sz, std::move(hbuf));
  return boh;
}

static std::shared_ptr<xrt::bo_impl>
alloc_dbuf(xclDeviceHandle dhdl, size_t sz, xrtBufferFlags, xrtMemoryGroup grp)
{
  auto handle = alloc_bo(dhdl, sz, XCL_BO_FLAGS_DEV_ONLY, grp);
  auto boh = std::make_shared<xrt::buffer_dbuf>(dhdl, handle, sz);
  return boh;
}

static std::shared_ptr<xrt::bo_impl>
alloc_nodma(xclDeviceHandle dhdl, size_t sz, xrtBufferFlags, xrtMemoryGroup grp)
{
  constexpr size_t align = 64;
  if (sz % align)
    throw xrt_core::error(EINVAL, "Invalid buffer size '" + std::to_string(sz) +
                          "', must be multiple of 64 bytes for NoDMA platforms");

  auto hbuf_handle = alloc_bo(dhdl, sz, XCL_BO_FLAGS_HOST_ONLY, grp);
  auto dbuf_handle = alloc_bo(dhdl, sz, XCL_BO_FLAGS_DEV_ONLY, grp);
  auto boh = std::make_shared<xrt::buffer_nodma>(dhdl, hbuf_handle, dbuf_handle, sz);
  return boh;
}

static std::shared_ptr<xrt::bo_impl>
alloc(xclDeviceHandle dhdl, size_t sz, xrtBufferFlags flags, xrtMemoryGroup grp)
{
  auto type = flags & ~XRT_BO_FLAGS_MEMIDX_MASK;
  switch (type) {
  case 0:
#ifndef XRT_EDGE
    if (is_nodma(dhdl))
      return alloc_nodma(dhdl, sz, flags, grp);
    else
      return alloc_hbuf(dhdl, xrt_core::aligned_alloc(get_alignment(), sz), sz, flags, grp);
#endif
  case XCL_BO_FLAGS_CACHEABLE:
  case XCL_BO_FLAGS_SVM:
  case XCL_BO_FLAGS_HOST_ONLY:
  case XCL_BO_FLAGS_P2P:
  case XCL_BO_FLAGS_EXECBUF:
    return alloc_kbuf(dhdl, sz, flags, grp);
  case XCL_BO_FLAGS_DEV_ONLY:
    return alloc_dbuf(dhdl, sz, flags, grp);
  default:
    throw std::runtime_error("Unknown buffer type");
  }
}

static std::shared_ptr<xrt::bo_impl>
alloc_xbuf(xclDeviceHandle dhdl, xclBufferHandle xhdl)
{
  return std::make_shared<xrt::buffer_xbuf>(dhdl, xhdl);
}

static std::shared_ptr<xrt::bo_impl>
alloc_userptr(xclDeviceHandle dhdl, void* userptr, size_t sz, xrtBufferFlags flags, xrtMemoryGroup grp)
{
  return alloc_ubuf(dhdl, userptr, sz, flags, grp);
}

static std::shared_ptr<xrt::bo_impl>
alloc_import(xclDeviceHandle dhdl, xclBufferExportHandle ehdl)
{
  return std::make_shared<xrt::buffer_import>(dhdl, ehdl);
}

static std::shared_ptr<xrt::bo_impl>
alloc_import_from_pid(xclDeviceHandle dhdl, xrt::pid_type pid, xclBufferExportHandle ehdl)
{
  return std::make_shared<xrt::buffer_import>(dhdl, pid, ehdl);
}

static std::shared_ptr<xrt::bo_impl>
alloc_sub(const std::shared_ptr<xrt::bo_impl>& parent, size_t size, size_t offset)
{
  return std::make_shared<xrt::buffer_sub>(parent, size, offset);
}

// alloc_clone() - Create a clone of src BO in specified memory bank
static std::shared_ptr<xrt::bo_impl>
alloc_clone(const std::shared_ptr<xrt::bo_impl>& src, xrt::memory_group grp)
{
  // Same device and flags as src bo
  auto dhdl = src->get_device()->get_device_handle();
  auto xflags = static_cast<xrtBufferFlags>(src->get_flags());

  auto clone_handle = alloc_bo(dhdl, src->get_size(), xflags, grp);
  auto clone = std::make_shared<xrt::buffer_clone>(dhdl, src, clone_handle, src->get_size());

  // the clone implmentation lifetime is tied to src
  src->add_clone(clone);
  return clone;
}

static xclDeviceHandle
get_xcl_device_handle(xrtDeviceHandle dhdl)
{
  return xrt_core::device_int::get_xcl_device_handle(dhdl);
}

// When no flags are specified, automatically infer host_only for
// NoDMA platforms when memory bank is host memory only.
static xrtBufferFlags
adjust_buffer_flags(const xrt::device& device, xrt::bo::flags flags, xrt::memory_group grp)
{
  if (flags != xrt::bo::flags::normal)
    return static_cast<xrtBufferFlags>(flags);

  if (!is_nodma(device))
    return static_cast<xrtBufferFlags>(flags);

  if (device.get_handle()->get_memory_type(grp) == xrt_core::device::memory_type::host)
    return static_cast<xrtBufferFlags>(xrt::bo::flags::host_only);

  return static_cast<xrtBufferFlags>(flags);
}

// When no flags are specified, automatically infer host_only for
// NoDMA platforms when memory bank is host memory only.
// Optimized short cut to avoid converting xclDeviceHandle to core device
static xrtBufferFlags
adjust_buffer_flags(xclDeviceHandle dhdl, xrt::bo::flags flags, xrt::memory_group grp)
{
  if (flags == xrt::bo::flags::normal)
    return adjust_buffer_flags(xrt::device{dhdl}, flags, grp);
  return static_cast<xrtBufferFlags>(flags);
}


} // namespace

////////////////////////////////////////////////////////////////
// xrt_bo implementation of extension APIs not exposed to end-user
////////////////////////////////////////////////////////////////
namespace xrt_core { namespace bo {

uint64_t
address(const xrt::bo& bo)
{
  return bo.get_handle()->get_address();
}

uint64_t
address(xrtBufferHandle handle)
{
  return get_boh(handle)->get_address();
}

int32_t
group_id(const xrt::bo& bo)
{
  return bo.get_handle()->get_group_id();
}

xclDeviceHandle
device_handle(const xrt::bo& bo)
{
    return bo.get_handle()->get_device()->get_device_handle();
}

xrt::bo::flags
get_flags(const xrt::bo& bo)
{
    return bo.get_handle()->get_flags();
}

xrt::bo
clone(const xrt::bo& src, xrt::memory_group target_grp)
{
  return alloc_clone(src.get_handle(), target_grp);
}

void
fill_copy_pkt(const xrt::bo& dst, const xrt::bo& src, size_t sz,
              size_t dst_offset, size_t src_offset, ert_start_copybo_cmd* pkt)
{
#ifndef _WIN32
  const auto& dst_boh = dst.get_handle();
  const auto& src_boh = src.get_handle();
  ert_fill_copybo_cmd(pkt, src_boh->get_xcl_handle(), dst_boh->get_xcl_handle(), src_offset, dst_offset, sz);
#else
  throw std::runtime_error("ert_fill_copybo_cmd not implemented on windows");
#endif
}

bool
is_imported(const xrt::bo& bo)
{
  const auto& boh = bo.get_handle();
  return boh->is_imported();
}

bool
is_aligned_ptr(const void* ptr)
{
  return ::is_aligned_ptr(ptr);
}

size_t
alignment()
{
  return ::get_alignment();
}

}} // namespace bo, xrt_core


////////////////////////////////////////////////////////////////
// xrt_bo C++ API implmentations (xrt_bo.h)
////////////////////////////////////////////////////////////////
namespace xrt {

bo::
bo(xclDeviceHandle dhdl, void* userptr, size_t sz, bo::flags flags, memory_group grp)
  : handle(xdp::native::profiling_wrapper("xrt::bo::bo",
      alloc_userptr, dhdl, userptr, sz, adjust_buffer_flags(dhdl, flags, grp), grp))
{}

bo::
bo(xclDeviceHandle dhdl, size_t size, bo::flags flags, memory_group grp)
  : handle(xdp::native::profiling_wrapper("xrt::bo::bo",
      alloc, dhdl, size, adjust_buffer_flags(dhdl, flags, grp), grp))
{}

bo::
bo(xclDeviceHandle dhdl, xclBufferExportHandle ehdl)
  : handle(xdp::native::profiling_wrapper("xrt::bo::bo",
	   alloc_import, dhdl, ehdl))
{}

bo::
bo(xclDeviceHandle dhdl, pid_type pid, xclBufferExportHandle ehdl)
  : handle(xdp::native::profiling_wrapper("xrt::bo::bo",
            alloc_import_from_pid, dhdl, pid , ehdl))
{}

bo::
bo(const bo& parent, size_t size, size_t offset)
  : handle(xdp::native::profiling_wrapper("xrt::bo::bo",
	   alloc_sub, parent.handle, size, offset))
{}

bo::
bo(xclDeviceHandle dhdl, xcl_buffer_handle xhdl)
  : handle(alloc_xbuf(dhdl, xhdl.bhdl))
{}

bo::
bo(xrtBufferHandle xhdl)
  : handle(xdp::native::profiling_wrapper("xrt::bo::bo",
	   get_boh, xhdl))
{}

size_t
bo::
size() const
{
  return xdp::native::profiling_wrapper("xrt::bo::size", [this]{
    return handle->get_size();
  }) ;
}

uint64_t
bo::
address() const
{
  return xdp::native::profiling_wrapper("xrt::bo::address", [this]{
    return handle->get_address();
  });
}

xclBufferExportHandle
bo::
export_buffer()
{
  return xdp::native::profiling_wrapper("xrt::bo::export_buffer", [this]{
    return handle->export_buffer();
  });
}

void
bo::
sync(xclBOSyncDirection dir, size_t size, size_t offset)
{
  return xdp::native::profiling_wrapper_sync("xrt::bo::sync", dir, size,
    [this, dir, size, offset]{
      handle->sync(dir, size, offset);
    });
}

void*
bo::
map()
{
  return xdp::native::profiling_wrapper("xrt::bo::map", [this]{
    return handle->get_hbuf();
  });
}

void
bo::
write(const void* src, size_t size, size_t seek)
{
  xdp::native::profiling_wrapper("xrt::bo::write", [this, src, size, seek]{
    handle->write(src, size, seek);
  });
}

void
bo::
read(void* dst, size_t size, size_t skip)
{
  xdp::native::profiling_wrapper("xrt::bo::read", [this, dst, size, skip]{
    handle->read(dst, size, skip);
  });
}

void
bo::
copy(const bo& src, size_t sz, size_t src_offset, size_t dst_offset)
{
  xdp::native::profiling_wrapper("xrt::bo::copy",
    [this, &src, sz, src_offset, dst_offset]{
      handle->copy(src.handle.get(), sz, src_offset, dst_offset);
    });
}

} // xrt

#ifdef XRT_ENABLE_AIE
////////////////////////////////////////////////////////////////
// xrt_aie_bo C++ API implmentations (xrt_aie.h)
////////////////////////////////////////////////////////////////
namespace xrt { namespace aie {

void
bo::
sync(const std::string& port, xclBOSyncDirection dir, size_t sz, size_t offset)
{
  get_handle()->sync(*this, port, dir, sz, offset);
}

}} // namespace aie, xrt
#endif

////////////////////////////////////////////////////////////////
// xrt_bo API implmentations (xrt_bo.h)
////////////////////////////////////////////////////////////////
xrtBufferHandle
xrtBOAllocUserPtr(xrtDeviceHandle dhdl, void* userptr, size_t size, xrtBufferFlags flags, xrtMemoryGroup grp)
{
  try {
    return xdp::native::profiling_wrapper(__func__,
    [dhdl, userptr, size, flags, grp]{
      auto boh = alloc_userptr(get_xcl_device_handle(dhdl), userptr, size, flags, grp);
      auto hdl = boh.get();
      bo_cache.add(hdl, std::move(boh));
      return hdl;
    });
  }
  catch (const xrt_core::error& ex) {
    xrt_core::send_exception_message(ex.what());
    errno = ex.get_code();
  }
  catch (const std::exception& ex) {
    send_exception_message(ex.what());
  }
  return nullptr;

}

xrtBufferHandle
xrtBOAlloc(xrtDeviceHandle dhdl, size_t size, xrtBufferFlags flags, xrtMemoryGroup grp)
{
  try {
    return xdp::native::profiling_wrapper(__func__,
    [dhdl, size, flags, grp]{
      auto boh = alloc(get_xcl_device_handle(dhdl), size, flags, grp);
      auto hdl = boh.get();
      bo_cache.add(hdl, std::move(boh));
      return hdl;
    });
  }
  catch (const xrt_core::error& ex) {
    xrt_core::send_exception_message(ex.what());
    errno = ex.get_code();
  }
  catch (const std::exception& ex) {
    send_exception_message(ex.what());
  }
  return nullptr;
}

xrtBufferHandle
xrtBOSubAlloc(xrtBufferHandle phdl, size_t sz, size_t offset)
{
  try {
    return xdp::native::profiling_wrapper(__func__, [phdl, sz, offset]{
      const auto& parent = get_boh(phdl);
      auto boh = alloc_sub(parent, sz, offset);
      auto hdl = boh.get();
      bo_cache.add(hdl, std::move(boh));
      return hdl;

    });
  }
  catch (const xrt_core::error& ex) {
    xrt_core::send_exception_message(ex.what());
    errno = ex.get_code();
  }
  catch (const std::exception& ex) {
    send_exception_message(ex.what());
  }
  return nullptr;
}

xrtBufferHandle
xrtBOImport(xrtDeviceHandle dhdl, xclBufferExportHandle ehdl)
{
  try {
    return xdp::native::profiling_wrapper(__func__, [dhdl, ehdl]{
      auto boh = alloc_import(get_xcl_device_handle(dhdl), ehdl);
      auto hdl = boh.get();
      bo_cache.add(hdl, std::move(boh));
      return hdl;
    });
  }
  catch (const xrt_core::error& ex) {
    xrt_core::send_exception_message(ex.what());
    errno = ex.get_code();
  }
  catch (const std::exception& ex) {
    send_exception_message(ex.what());
  }
  return nullptr;
}

xclBufferExportHandle
xrtBOExport(xrtBufferHandle bhdl)
{
  try {
    return xdp::native::profiling_wrapper(__func__, [bhdl]{
      return get_boh(bhdl)->export_buffer();
    });
  }
  catch (const xrt_core::error& ex) {
    xrt_core::send_exception_message(ex.what());
    errno = ex.get_code();
  }
  catch (const std::exception& ex) {
    send_exception_message(ex.what());
  }
  return XRT_NULL_BO_EXPORT;
}

xrtBufferHandle
xrtBOAllocFromXcl(xrtDeviceHandle dhdl, xclBufferHandle xhdl)
{
  try {
    return xdp::native::profiling_wrapper(__func__, [dhdl, xhdl] {
      auto boh = alloc_xbuf(xrtDeviceToXclDevice(dhdl), xhdl);
      auto hdl = boh.get();
      bo_cache.add(hdl, std::move(boh));
      return hdl;
    });
  }
  catch (const xrt_core::error& ex) {
    xrt_core::send_exception_message(ex.what());
    errno = ex.get_code();
  }
  catch (const std::exception& ex) {
    send_exception_message(ex.what());
  }
  return nullptr;
}

int
xrtBOFree(xrtBufferHandle bhdl)
{
  try {
    return xdp::native::profiling_wrapper(__func__, [bhdl]{
      bo_cache.remove_or_error(bhdl);
      return 0;
    });
  }
  catch (const xrt_core::error& ex) {
    xrt_core::send_exception_message(ex.what());
    errno = ex.get_code();
  }
  catch (const std::exception& ex) {
    send_exception_message(ex.what());
  }
  return -1;
}

size_t
xrtBOSize(xrtBufferHandle bhdl)
{
  try {
    return xdp::native::profiling_wrapper(__func__, [bhdl]{
      return get_boh(bhdl)->get_size();
    });
  }
  catch (const xrt_core::error& ex) {
    xrt_core::send_exception_message(ex.what());
    errno = ex.get_code();
  }
  catch (const std::exception& ex) {
    send_exception_message(ex.what());
  }
  return std::numeric_limits<size_t>::max();
}


int
xrtBOSync(xrtBufferHandle bhdl, xclBOSyncDirection dir, size_t size, size_t offset)
{
  try {
    return xdp::native::profiling_wrapper_sync(__func__, dir, size,
    [bhdl, dir, size, offset]{
      get_boh(bhdl)->sync(dir, size, offset);
      return 0;
    });
  }
  catch (const xrt_core::error& ex) {
    xrt_core::send_exception_message(ex.what());
    errno = ex.get_code();
  }
  catch (const std::exception& ex) {
    send_exception_message(ex.what());
  }
  return -1;
}

void*
xrtBOMap(xrtBufferHandle bhdl)
{
  try {
    return xdp::native::profiling_wrapper(__func__, [bhdl]{
      return get_boh(bhdl)->get_hbuf();
    });
  }
  catch (const xrt_core::error& ex) {
    xrt_core::send_exception_message(ex.what());
    errno = ex.get_code();
  }
  catch (const std::exception& ex) {
    send_exception_message(ex.what());
  }
  return nullptr;
}

int
xrtBOWrite(xrtBufferHandle bhdl, const void* src, size_t size, size_t seek)
{
  try {
    return xdp::native::profiling_wrapper(__func__,
    [bhdl, src, size, seek]{
      get_boh(bhdl)->write(src, size, seek);
      return 0;
    });
  }
  catch (const xrt_core::error& ex) {
    xrt_core::send_exception_message(ex.what());
    errno = ex.get_code();
  }
  catch (const std::exception& ex) {
    send_exception_message(ex.what());
  }
  return -1;
}

int
xrtBORead(xrtBufferHandle bhdl, void* dst, size_t size, size_t skip)
{
  try {
    return xdp::native::profiling_wrapper(__func__,
    [bhdl, dst, size, skip]{
      get_boh(bhdl)->read(dst, size, skip);
      return 0;
    });
  }
  catch (const xrt_core::error& ex) {
    xrt_core::send_exception_message(ex.what());
    errno = ex.get_code();
  }
  catch (const std::exception& ex) {
    send_exception_message(ex.what());
  }
  return -1;
}

int
xrtBOCopy(xrtBufferHandle dhdl, xrtBufferHandle shdl, size_t sz, size_t dst_offset, size_t src_offset)
{
  try {
    return xdp::native::profiling_wrapper(__func__,
    [dhdl, shdl, sz, dst_offset, src_offset]{
      const auto& dst = get_boh(dhdl);
      const auto& src = get_boh(shdl);
      dst->copy(src.get(), sz, src_offset, dst_offset);
      return 0;
    });
  }
  catch (const xrt_core::error& ex) {
    xrt_core::send_exception_message(ex.what());
    errno = ex.get_code();
  }
  catch (const std::exception& ex) {
    send_exception_message(ex.what());
  }
  return -1;
}


uint64_t
xrtBOAddress(xrtBufferHandle bhdl)
{
  try {
    return xdp::native::profiling_wrapper(__func__, [bhdl]{
      return get_boh(bhdl)->get_address();
    });
  }
  catch (const xrt_core::error& ex) {
    xrt_core::send_exception_message(ex.what());
    errno = ex.get_code();
  }
  catch (const std::exception& ex) {
    send_exception_message(ex.what());
  }
  return std::numeric_limits<uint64_t>::max();
}
