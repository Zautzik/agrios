#pragma once

#include <fcntl.h>     // O_CREAT, O_RDWR
#include <sys/mman.h>  // shm_open, mmap, munmap
#include <sys/stat.h>  // mode constants
#include <unistd.h>    // ftruncate, close

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>

#include "agrios/spsc_ring_buffer.hpp"

namespace agrios {

// Owns (or attaches to) a POSIX shared-memory segment holding exactly one
// SPSCRingBuffer<T, Capacity>, and placement-constructs it there.
//
// Exactly one process must construct with owner = true: this shm_unlinks any
// stale segment left behind by a crashed prior run (shm_open with O_CREAT
// does *not* zero an existing segment's contents, so a leftover segment
// would otherwise leave head_/tail_ at whatever garbage indices the last run
// left them at), creates a fresh segment, and placement-constructs the
// ring buffer into it. Every other process attaches with owner = false and
// reinterprets the already-initialized object -- it must not construct a new
// one on top of live producer/consumer state.
//
// All attaching processes must agree on T, Capacity, and name.
template <typename T, std::size_t Capacity>
class SharedRingBuffer {
   public:
    using Buffer = SPSCRingBuffer<T, Capacity>;

    SharedRingBuffer(std::string name, bool owner) : name_(std::move(name)), owner_(owner) {
        if (owner_) {
            ::shm_unlink(name_.c_str());  // best-effort; ENOENT is fine
        }

        const int flags = owner_ ? (O_CREAT | O_RDWR) : O_RDWR;
        fd_ = ::shm_open(name_.c_str(), flags, 0666);
        if (fd_ == -1) {
            throw std::runtime_error("shm_open(\"" + name_ + "\") failed: " + std::strerror(errno));
        }

        if (owner_ && ::ftruncate(fd_, static_cast<off_t>(sizeof(Buffer))) == -1) {
            const std::string msg = "ftruncate failed: " + std::string(std::strerror(errno));
            ::close(fd_);
            throw std::runtime_error(msg);
        }

        void* addr = ::mmap(nullptr, sizeof(Buffer), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (addr == MAP_FAILED) {
            const std::string msg = "mmap failed: " + std::string(std::strerror(errno));
            ::close(fd_);
            throw std::runtime_error(msg);
        }

        buffer_ = owner_ ? new (addr) Buffer()                    // zero-initializes head_/tail_
                          : reinterpret_cast<Buffer*>(addr);       // reuse the live object as-is
    }

    ~SharedRingBuffer() {
        if (buffer_ != nullptr) {
            ::munmap(buffer_, sizeof(Buffer));
        }
        if (fd_ != -1) {
            ::close(fd_);
        }
        if (owner_) {
            ::shm_unlink(name_.c_str());
        }
    }

    SharedRingBuffer(const SharedRingBuffer&) = delete;
    SharedRingBuffer& operator=(const SharedRingBuffer&) = delete;

    Buffer& get() noexcept { return *buffer_; }
    const Buffer& get() const noexcept { return *buffer_; }

   private:
    std::string name_;
    bool owner_;
    int fd_ = -1;
    Buffer* buffer_ = nullptr;
};

}  // namespace agrios
