#include <atomic>
#include "objects.h"
#include "window_trace.h"
#include "kernel.h"
#include "../guest.h"
#include "../log.h"
#include "../crash.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <map>
#include <thread>
#include <unordered_map>

namespace kernel
{
    // One per waiting thread, registered with every object it waits on.
    struct WaitBlock
    {
        std::condition_variable wake;
    };
}

using namespace kernel;

namespace
{
    void TraceEvent(const Object&, const char*) {}

    using Clock = std::chrono::steady_clock;

    std::mutex g_dispatcher;

    std::mutex g_handleLock;
    std::unordered_map<uint32_t, std::shared_ptr<Object>> g_handles;
    uint32_t g_nextHandle = 0x00010004;

    // X_DISPATCHER_HEADER: the type byte at +0, the signal state at +4 and the
    // wait list at +8 (flink) and +12 (blink); a semaphore's limit follows at
    // +16. A header naming one of the objects below holds its id in the flink
    // and this in the blink -- which the title's own initialisation, pointing
    // the list at itself, overwrites.
    constexpr uint32_t kSignature = 0x4D573200;   // 'MW2\0'
    enum : uint8_t
    {
        kNotificationEvent = 0, kSynchronizationEvent = 1, kMutantType = 2,
        kSemaphoreType = 5, kThreadType = 6, kNotificationTimer = 8, kSynchronizationTimer = 9,
    };

    std::mutex g_headerLock;
    std::unordered_map<uint32_t, std::shared_ptr<Object>> g_byHeader;   // by id
    // The same objects by the guest address of their header. The title may
    // initialise a KEVENT in place again (KeInitializeEvent is inline), which
    // wipes the id written above; the object -- and any thread waiting on it --
    // must survive that, or the next KeSetEvent wakes a stranger.
    std::unordered_map<uint32_t, std::weak_ptr<Object>> g_byAddress;
    uint32_t g_nextId = 1;

    // NT's timeout as a host deadline: negative is relative, positive an
    // absolute system time. False when it never falls due.
    bool DeadlineFor(const int64_t* timeout100ns, Clock::time_point& deadline)
    {
        if (!timeout100ns || *timeout100ns == INT64_MIN) return false;
        const int64_t value = *timeout100ns;
        int64_t relative = value < 0 ? -value
                                     : value - int64_t(SystemTime100ns());
        // A year is as good as for ever, and keeps the arithmetic in range.
        constexpr int64_t kYear = 365LL * 24 * 3600 * 10'000'000;
        relative = std::clamp<int64_t>(relative, 0, kYear);
        deadline = Clock::now() + std::chrono::nanoseconds(relative * 100);
        return true;
    }

    std::shared_ptr<Object> TakeUp(uint32_t address)
    {
        const uint8_t type = *GuestPtr<uint8_t>(address);
        const int32_t state = int32_t(uint32_t(*GuestPtr<be32>(address + 4)));
        switch (type)
        {
        case kNotificationEvent:
        case kSynchronizationEvent:
        {
            auto event = std::make_shared<Event>();
            event->manualReset = type == kNotificationEvent;
            event->signalled = state != 0;
            return event;
        }
        case kSemaphoreType:
        {
            auto semaphore = std::make_shared<Semaphore>();
            semaphore->count = state;
            semaphore->limit = int32_t(uint32_t(*GuestPtr<be32>(address + 16)));
            return semaphore;
        }
        // Signal state 1 is free; a mutant initialised owned is not taken up
        // here, since the header does not say by which of our threads.
        case kMutantType:
            return std::make_shared<Mutant>();
        case kNotificationTimer:
        case kSynchronizationTimer:
        {
            auto timer = std::make_shared<Timer>();
            timer->manualReset = type == kNotificationTimer;
            timer->signalled = state != 0;
            return timer;
        }
        default:
            return nullptr;
        }
    }

    void AttachLocked(const std::shared_ptr<Object>& object, uint32_t address)
    {
        const uint32_t id = g_nextId++;
        *GuestPtr<be32>(address + 8) = id;
        *GuestPtr<be32>(address + 12) = kSignature;
        object->guestHeader = address;
        g_byHeader[id] = object;
        g_byAddress[address] = object;
    }

    uint8_t TypeOf(const Object& object)
    {
        if (auto* timer = dynamic_cast<const Timer*>(&object))
            return timer->manualReset ? kNotificationTimer : kSynchronizationTimer;
        if (auto* event = dynamic_cast<const Event*>(&object))
            return event->manualReset ? kNotificationEvent : kSynchronizationEvent;
        if (dynamic_cast<const Semaphore*>(&object)) return kSemaphoreType;
        if (dynamic_cast<const Mutant*>(&object)) return kMutantType;
        return kThreadType;   // threads, and anything else a handle names
    }

    // Timers fall due on a thread of their own.
    struct TimerQueue
    {
        std::mutex lock;
        std::condition_variable wake;
        struct Entry { std::weak_ptr<Timer> timer; uint64_t generation; uint32_t periodMs; };
        std::multimap<Clock::time_point, Entry> due;
        bool started = false;

        void Run()
        {
            std::unique_lock held(lock);
            for (;;)
            {
                if (due.empty()) { wake.wait(held); continue; }
                const auto first = due.begin();
                if (Clock::now() < first->first) { wake.wait_until(held, first->first); continue; }
                const Clock::time_point when = first->first;
                const Entry entry = first->second;
                due.erase(first);
                held.unlock();
                bool again = false;
                if (auto timer = entry.timer.lock())
                {
                    std::lock_guard dispatch(g_dispatcher);
                    if (timer->generation == entry.generation)
                    {
                        timer->signalled = true;
                        timer->Wake();
                        again = entry.periodMs != 0;
                        timer->running = again;
                    }
                }
                held.lock();
                if (again)
                    due.emplace(when + std::chrono::milliseconds(entry.periodMs), entry);
            }
        }

        void Add(const std::shared_ptr<Timer>& timer, Clock::time_point when,
                 uint64_t generation, uint32_t periodMs)
        {
            std::lock_guard held(lock);
            if (!started)
            {
                started = true;
                std::thread([this] { Run(); }).detach();
            }
            due.emplace(when, Entry{ timer, generation, periodMs });
            wake.notify_one();
        }
    };
    TimerQueue g_timers;
}

uintptr_t kernel::CurrentThread()
{
    static thread_local char token;
    return uintptr_t(&token);
}

std::mutex& kernel::DispatcherLock() { return g_dispatcher; }

void kernel::Dispatcher::Wake()
{
    for (WaitBlock* waiter : waiters) waiter->wake.notify_one();
}

void kernel::Event::Satisfy(uintptr_t)
{
    if (!manualReset)
    {
        signalled = false;
        if (guestHeader) *GuestPtr<be32>(guestHeader + 4) = 0;
    }
}

bool kernel::Event::Set(const char* thread, uint32_t lr)
{
    std::lock_guard held(g_dispatcher);
    const bool was = signalled;
    TraceEvent(*this, "Set");
    if (WindowTraceOn()) WindowTrace("set", thread ? thread : crash::CurrentThreadName(), guestHeader, handle, "event");
    signalled = true;
    setCounter++;
    lastSignalerThread = thread ? thread : crash::CurrentThreadName();
    lastSignalerLr = lr;
    if (guestHeader) *GuestPtr<be32>(guestHeader + 4) = 1;
    Wake();
    return was;
}

bool kernel::Event::Reset(const char* thread, uint32_t lr)
{
    TraceEvent(*this, "Reset");
    std::lock_guard held(g_dispatcher);
    const bool was = signalled;
    signalled = false;
    resetCounter++;
    lastSignalerThread = thread ? thread : crash::CurrentThreadName();
    lastSignalerLr = lr;
    if (guestHeader) *GuestPtr<be32>(guestHeader + 4) = 0;
    return was;
}

int32_t kernel::Semaphore::Release(int32_t delta, uint32_t lr)
{
    std::lock_guard held(g_dispatcher);
    const int32_t previous = count;
    setCounter++;
    if (WindowTraceOn()) WindowTrace("release", crash::CurrentThreadName(), guestHeader, handle, "semaphore");
    lastSignalerThread = crash::CurrentThreadName();
    lastSignalerLr = lr;
    count = int32_t(std::min<int64_t>(int64_t(count) + delta, limit));
    Wake();
    return previous;
}

void kernel::Mutant::Satisfy(uintptr_t thread)
{
    if (owner != thread) ownerName = crash::CurrentThreadName();
    owner = thread;
    depth++;
}

int32_t kernel::Mutant::Release()
{
    std::lock_guard held(g_dispatcher);
    const int32_t previous = int32_t(depth);
    if (owner == CurrentThread() && depth && --depth == 0)
    {
        owner = 0;
        Wake();
    }
    return previous;
}

uint32_t kernel::Wait(Dispatcher* const* objects, uint32_t count, bool all,
                      const int64_t* timeout100ns)
{
    Clock::time_point deadline;
    const bool timed = DeadlineFor(timeout100ns, deadline);
    const uintptr_t me = CurrentThread();
    WaitBlock block;
    bool registered = false;
    uint32_t result = X_STATUS_TIMEOUT;
    const auto waitStarted = Clock::now();
    for (uint32_t i = 0; i < count; i++) TraceEvent(*objects[i], "wait begins");
    if (WindowTraceOn())
        for (uint32_t i = 0; i < count; i++) WindowTrace("wait", crash::CurrentThreadName(), objects[i]->guestHeader, objects[i]->handle, objects[i]->TypeName());

    std::unique_lock held(g_dispatcher);
    for (;;)
    {
        if (all)
        {
            bool ready = true;
            for (uint32_t i = 0; i < count && ready; i++) ready = objects[i]->Signalled(me);
            if (ready)
            {
                for (uint32_t i = 0; i < count; i++) objects[i]->Satisfy(me);
                result = X_STATUS_SUCCESS;
                break;
            }
        }
        else
        {
            uint32_t i = 0;
            while (i < count && !objects[i]->Signalled(me)) i++;
            if (i < count)
            {
                objects[i]->Satisfy(me);
                result = i;   // STATUS_WAIT_0 + i
                break;
            }
        }
        if (timed && Clock::now() >= deadline) break;
        if (!registered)
        {
            for (uint32_t i = 0; i < count; i++) objects[i]->waiters.push_back(&block);
            registered = true;
        }
        if (timed) block.wake.wait_until(held, deadline);
        else
        {
            if (block.wake.wait_for(held, std::chrono::seconds(2)) == std::cv_status::timeout)
            {
                const char* tName = crash::CurrentThreadName();
                if (objects[0]->handle)
                    LOGW("sync: thread '%s' waiting on %u object(s) (first: %s, handle %08X, created by '%s' @ %08X, sig=%d) for >2s",
                         tName ? tName : "unnamed", count, objects[0]->TypeName(), objects[0]->handle,
                         objects[0]->creatorThread ? objects[0]->creatorThread : "?", objects[0]->creatorLr,
                         objects[0]->Signalled(me));
                else
                    LOGW("sync: thread '%s' waiting on %u object(s) (first: %s at guest %08X, sig=%d) for >2s",
                         tName ? tName : "unnamed", count, objects[0]->TypeName(), objects[0]->guestHeader,
                         objects[0]->Signalled(me));
                // Every object of a multi-wait, not only the first: which one
                // nobody signals is the question.
                if (count > 1)
                    for (uint32_t i = 0; i < count && i < 8; i++)
                        LOGW("sync:   [%u] %s at guest %08X handle %08X, sig=%d, created by '%s' @ %08X",
                             i, objects[i]->TypeName(), objects[i]->guestHeader, objects[i]->handle,
                             objects[i]->Signalled(me),
                             objects[i]->creatorThread ? objects[i]->creatorThread : "?", objects[i]->creatorLr);
                static uint32_t s_waitCount = 0;
                if (++s_waitCount % 30 == 0) kernel::ReportWaits();   // the whole picture now and then; it is long
            }
        }
    }
    for (uint32_t i = 0; i < count; i++) TraceEvent(*objects[i], result == X_STATUS_TIMEOUT ? "wait timed out" : "wait satisfied");
    if (WindowTraceOn())
        WindowTrace("woke", crash::CurrentThreadName(), objects[0]->guestHeader, objects[0]->handle, result == X_STATUS_TIMEOUT ? "timeout" : "signalled");
    if (registered)
        for (uint32_t i = 0; i < count; i++)
        {
            auto& list = objects[i]->waiters;
            list.erase(std::find(list.begin(), list.end(), &block));
        }
    // A thread that slept a long time and was then woken: by whom? The waker's
    // thread and call site are the producer the sleeper was waiting for, which is
    // what to look at when a pipeline moves in slow bursts.
    if (!timed && result != X_STATUS_TIMEOUT)
    {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - waitStarted).count();
        static std::atomic<uint32_t> woken{ 0 };
        if (ms >= 1000 && woken++ < 60)
        {
            const uint32_t i = result < count ? result : 0;
            const char* by = objects[i]->lastSignalerThread ? objects[i]->lastSignalerThread : "?";
            const uint32_t at = objects[i]->lastSignalerLr;
            held.unlock();
            LOGW("sync: '%s' woke after %lld ms on %s guest %08X handle %08X, last signalled by '%s' @ %08X (signal #%u)",
                 crash::CurrentThreadName() ? crash::CurrentThreadName() : "?", (long long)ms,
                 objects[i]->TypeName(), objects[i]->guestHeader, objects[i]->handle, by, at,
                 objects[i]->setCounter);
        }
    }

    // A wait that ran its whole timeout is how a missed wakeup shows: the thread
    // carries on only because the clock ran out. Say which, now and then.
    if (timed && result == X_STATUS_TIMEOUT)
    {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - waitStarted).count();
        static std::atomic<uint32_t> told{ 0 };
        if (ms >= 300 && told++ < 40)
        {
            held.unlock();
            LOGW("sync: '%s' waited %lld ms and timed out on %u object(s); first: %s guest %08X handle %08X sig=%d created by '%s' @ %08X",
                 crash::CurrentThreadName() ? crash::CurrentThreadName() : "?", (long long)ms, count,
                 objects[0]->TypeName(), objects[0]->guestHeader, objects[0]->handle,
                 int(objects[0]->Signalled(me)),
                 objects[0]->creatorThread ? objects[0]->creatorThread : "?", objects[0]->creatorLr);
        }
    }
    return result;
}

uint32_t kernel::InsertHandle(std::shared_ptr<Object> object, const char* creatorThread, uint32_t creatorLr)
{
    std::lock_guard held(g_handleLock);
    const uint32_t handle = g_nextHandle;
    g_nextHandle += 4;
    if (!object->handle) object->handle = handle;
    if (!object->creatorThread)
        object->creatorThread = creatorThread ? creatorThread : crash::CurrentThreadName();
    if (!object->creatorLr)
        object->creatorLr = creatorLr;
    g_handles.emplace(handle, std::move(object));
    return handle;
}

std::shared_ptr<Object> kernel::LookupHandle(uint32_t handle)
{
    std::lock_guard held(g_handleLock);
    auto it = g_handles.find(handle);
    return it == g_handles.end() ? nullptr : it->second;
}

bool kernel::CloseHandle(uint32_t handle)
{
    std::lock_guard held(g_handleLock);
    return g_handles.erase(handle) != 0;
}

std::shared_ptr<Dispatcher> kernel::ObjectAt(uint32_t guestAddress)
{
    if (!guestAddress) return nullptr;
    std::lock_guard held(g_headerLock);
    if (uint32_t(*GuestPtr<be32>(guestAddress + 12)) == kSignature)
    {
        auto it = g_byHeader.find(*GuestPtr<be32>(guestAddress + 8));
        if (it != g_byHeader.end()) return std::dynamic_pointer_cast<Dispatcher>(it->second);
    }
    // A header we adopted before and the title has initialised again: the same
    // object, in the state the new header says, with its waiters kept.
    if (auto known = g_byAddress.find(guestAddress); known != g_byAddress.end())
        if (auto existing = known->second.lock())
            if (auto fresh = TakeUp(guestAddress); fresh && TypeOf(*fresh) == TypeOf(*existing))
            {
                std::lock_guard dispatcher(DispatcherLock());
                if (auto* e = dynamic_cast<Event*>(existing.get()))
                    e->signalled = static_cast<Event*>(fresh.get())->signalled;
                else if (auto* m = dynamic_cast<Semaphore*>(existing.get()))
                    m->count = static_cast<Semaphore*>(fresh.get())->count;
                AttachLocked(existing, guestAddress);
                static int told = 0;
                if (told++ < 4) LOGI("sync: header at %08X initialised again; the object and its waiters kept", guestAddress);
                return std::dynamic_pointer_cast<Dispatcher>(existing);
            }
    auto object = TakeUp(guestAddress);
    if (!object) return nullptr;
    AttachLocked(object, guestAddress);
    return std::dynamic_pointer_cast<Dispatcher>(object);
}

void kernel::AttachHeader(const std::shared_ptr<Object>& object, uint32_t guestAddress,
                          uint8_t type)
{
    std::lock_guard held(g_headerLock);
    *GuestPtr<uint8_t>(guestAddress) = type;
    AttachLocked(object, guestAddress);
}

uint32_t kernel::HeaderOf(const std::shared_ptr<Object>& object)
{
    if (!object) return 0;
    {
        std::lock_guard held(g_headerLock);
        if (object->guestHeader) return object->guestHeader;
    }
    constexpr uint32_t kHeaderBytes = 32;
    const uint32_t address = AllocateGuest(kHeaderBytes);
    if (!address) return 0;
    std::memset(guest::Base() + address, 0, kHeaderBytes);
    AttachHeader(object, address, TypeOf(*object));
    return address;
}

bool kernel::SetTimer(const std::shared_ptr<Timer>& timer, int64_t due100ns, uint32_t periodMs)
{
    Clock::time_point when;
    const int64_t due = due100ns;
    if (!DeadlineFor(&due, when)) when = Clock::now();
    uint64_t generation;
    bool was;
    {
        std::lock_guard held(g_dispatcher);
        was = timer->running;
        timer->running = true;
        timer->signalled = false;
        generation = ++timer->generation;
    }
    g_timers.Add(timer, when, generation, periodMs);
    return was;
}

bool kernel::CancelTimer(const std::shared_ptr<Timer>& timer)
{
    std::lock_guard held(g_dispatcher);
    const bool was = timer->running;
    timer->running = false;
    timer->generation++;
    return was;
}
