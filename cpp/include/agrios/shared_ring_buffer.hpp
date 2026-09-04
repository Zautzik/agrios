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

#if defined(__linux__)
inline constexpr int kMapPopulateFlag = MAP_POPULATE;
#else
// MAP_POPULATE is Linux-specific; there is no portable equivalent (macOS
// has no analogous mmap flag). The explicit touch pass below is what
// actually guarantees pre-faulting on non-Linux targets -- MAP_POPULATE
// here is a kernel hint that reduces the *number* of faults during mmap()
// itself where it's available, not the sole mechanism this relies on.
inline constexpr int kMapPopulateFlag = 0;
#endif

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

        void* addr =
            ::mmap(nullptr, sizeof(Buffer), PROT_READ | PROT_WRITE, MAP_SHARED | kMapPopulateFlag, fd_, 0);
        if (addr == MAP_FAILED) {
            const std::string msg = "mmap failed: " + std::string(std::strerror(errno));
            ::close(fd_);
            throw std::runtime_error(msg);
        }

        // MAP_POPULATE (where available) is a kernel hint, not a guarantee --
        // it can still defer pages under memory pressure, and has no
        // portable equivalent on non-Linux targets. Belt and suspenders:
        // explicitly fault in every page of the mapping ourselves, so the
        // *first* push()/pop() an RT thread performs never pays a first-
        // touch minor-fault cost -- the same reasoning as prefault_stack()
        // in rt_thread.hpp, applied to the shared-memory side of the bridge.
        //
        // Owner: safe to write-touch (zero) the whole region -- there's no
        // live state yet, we're about to placement-construct into it anyway.
        // Non-owner: must NOT write -- doing so would corrupt whatever the
        // owner has already published (head_/tail_, or in-flight payload
        // data). A physical page already being resident (because the owner
        // faulted it in) does not exempt this process from faulting it into
        // *its own* page tables on first touch, so this still needs a
        // read-only pass, not a no-op.
        if (owner_) {
            std::memset(addr, 0, sizeof(Buffer));
            buffer_ = new (addr) Buffer();  // zero-initializes head_/tail_ (redundant with the memset
                                             // above for those two members, not for the payload array)
        } else {
            volatile const unsigned char* p = static_cast<const unsigned char*>(addr);
            unsigned char sink = 0;
            // Smaller-than-actual-page stride is always safe (just touches
            // some pages more than once); sysconf(_SC_PAGESIZE) would be
            // more precise but isn't worth the extra syscall for a one-time
            // startup cost.
            for (std::size_t i = 0; i < sizeof(Buffer); i += 4096) {
                sink ^= p[i];
            }
            (void)sink;
            buffer_ = reinterpret_cast<Buffer*>(addr);  // reuse the live object as-is
        }
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
