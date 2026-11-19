// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/render_workers.hpp"
#include "adi/audio/io_audit.hpp"
#include <array>
#include <atomic>
#include <cfenv>
#include <stdexcept>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <dispatch/dispatch.h>
#else
#include <cerrno>
#include <semaphore.h>
#endif
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#include <immintrin.h>
#endif
namespace adi::engine {
namespace {
struct FloatingEnvironment {
    std::fenv_t environment{};
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    unsigned mxcsr=0;
#endif
    bool capture() noexcept {
        if(std::fegetenv(&environment)!=0)return false;
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
        mxcsr=_mm_getcsr();
#endif
        return true;
    }
    bool apply() const noexcept {
        if(std::fesetenv(&environment)!=0)return false;
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
        _mm_setcsr(mxcsr);
#endif
        return true;
    }
};
class Wakeup {
public:
    Wakeup() {
#if defined(_WIN32)
        semaphore_=CreateSemaphoreW(nullptr,0,1,nullptr);if(!semaphore_)throw std::runtime_error("render semaphore creation failed");
#elif defined(__APPLE__)
        semaphore_=dispatch_semaphore_create(0);if(!semaphore_)throw std::runtime_error("render semaphore creation failed");
#else
        if(sem_init(&semaphore_,0,0)!=0)throw std::runtime_error("render semaphore creation failed");
#endif
    }
    ~Wakeup() {
#if defined(_WIN32)
        CloseHandle(semaphore_);
#elif defined(__APPLE__)
        dispatch_release(semaphore_);
#else
        sem_destroy(&semaphore_);
#endif
    }
    bool post() noexcept {
#if defined(_WIN32)
        return ReleaseSemaphore(semaphore_,1,nullptr)!=0;
#elif defined(__APPLE__)
        dispatch_semaphore_signal(semaphore_);return true;
#else
        return sem_post(&semaphore_)==0;
#endif
    }
    bool wait() noexcept {
#if defined(_WIN32)
        return WaitForSingleObject(semaphore_,INFINITE)==WAIT_OBJECT_0;
#elif defined(__APPLE__)
        return dispatch_semaphore_wait(semaphore_,DISPATCH_TIME_FOREVER)==0;
#else
        int result=0;do{result=sem_wait(&semaphore_);}while(result!=0 && errno==EINTR);return result==0;
#endif
    }
private:
#if defined(_WIN32)
    HANDLE semaphore_=nullptr;
#elif defined(__APPLE__)
    dispatch_semaphore_t semaphore_=nullptr;
#else
    sem_t semaphore_{};
#endif
};
void pause() noexcept {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    _mm_pause();
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}
} // namespace
struct RenderWorkers::Impl {
    enum class State : unsigned {Idle,Preparing,Ready,Running,Done,Failed};
    struct Worker {
        Worker(std::size_t position,Initializer setup,void* setupContext):index(position),initialize(setup),initializationContext(setupContext),thread([this]{run();}){}
        ~Worker(){stop.store(true,std::memory_order_release);(void)wakeup.post();if(thread.joinable())thread.join();}
        void run() noexcept {
            if(initialize && !initialize(initializationContext,index)) {
                state.store(State::Failed,std::memory_order_release);started.store(true,std::memory_order_release);return;
            }
            started.store(true,std::memory_order_release);
            while(wakeup.wait()) {
                if(stop.load(std::memory_order_acquire))return;
                auto expected=State::Ready;
                if(state.compare_exchange_strong(expected,State::Running,std::memory_order_acq_rel)) {
                    {
                        audio::CallbackScope callback;
                        FloatingEnvironment previous;
                        if(!previous.capture() || !environment.apply()){state.store(State::Failed,std::memory_order_release);return;}
                        function(context,index);
                        // Keep this bit even if restoring FP state or a later
                        // idle wait fails: the DSP action already happened.
                        completed.store(true,std::memory_order_relaxed);
                        if(!previous.apply()){state.store(State::Failed,std::memory_order_release);return;}
                    }
                    state.store(State::Done,std::memory_order_release);
                }
            }
            // Failed OS waits must not strand the caller at its completion
            // barrier. A valid initialized semaphore normally cannot fail.
            state.store(State::Failed,std::memory_order_release);
        }
        const std::size_t index;
        Wakeup wakeup;
        Function function=nullptr;
        void* context=nullptr;
        FloatingEnvironment environment;
        std::atomic<State> state{State::Idle};
        std::atomic<bool> stop{false},completed{false},started{false};
        Initializer initialize=nullptr;
        void* initializationContext=nullptr;
        std::thread thread; // last: every field exists before this starts
    };
    Impl(std::size_t count,Initializer initialize,void* context):count_(count) {
        if(count==0 || count>maximum)throw std::invalid_argument("render worker count must be 1..16");
        // Resolve/warm the floating-environment helpers on the owner too.
        FloatingEnvironment environment;if(!environment.capture() || !environment.apply())throw std::runtime_error("render floating environment unavailable");
        for(std::size_t index=0;index<count;++index)workers[index]=std::make_unique<Worker>(index,initialize,context);
        for(std::size_t index=0;index<count;++index)while(!workers[index]->started.load(std::memory_order_acquire))std::this_thread::yield();
        for(std::size_t index=0;index<count;++index)if(workers[index]->state.load(std::memory_order_acquire)==State::Failed)throw std::runtime_error("render worker initialization failed");
    }
    std::size_t count_;
    std::array<std::unique_ptr<Worker>,maximum> workers;
};
RenderWorkers::RenderWorkers(std::size_t count,Initializer initialize,void* context):impl_(std::make_unique<Impl>(count,initialize,context)){}
RenderWorkers::~RenderWorkers()=default;
std::size_t RenderWorkers::size()const noexcept{return impl_->count_;}
std::uint32_t RenderWorkers::start(std::size_t count,Function function,void* context) noexcept {
    if(count==0 || !function || count>impl_->count_)return 0;
    FloatingEnvironment environment;if(!environment.capture())return 0;
    std::uint32_t mask=0;
    for(std::size_t index=0;index<count;++index) {
        auto& worker=*impl_->workers[index];
        auto expected=worker.state.load(std::memory_order_acquire);
        if(expected!=Impl::State::Idle && expected!=Impl::State::Done)continue;
        if(!worker.state.compare_exchange_strong(expected,Impl::State::Preparing,std::memory_order_acq_rel))continue;
        worker.completed.store(false,std::memory_order_relaxed);
        worker.function=function;worker.context=context;worker.environment=environment;expected=Impl::State::Preparing;
        if(!worker.state.compare_exchange_strong(expected,Impl::State::Ready,std::memory_order_release))continue;
        if(worker.wakeup.post())mask|=std::uint32_t{1}<<index;
        else worker.state.store(Impl::State::Failed,std::memory_order_release);
    }
    return mask;
}
std::uint32_t RenderWorkers::finish(std::uint32_t mask) noexcept {
    std::uint32_t completed=0;
    for(std::size_t index=0;index<impl_->count_;++index)if((mask&(std::uint32_t{1}<<index))!=0) {
        unsigned spins=0;
        auto state=impl_->workers[index]->state.load(std::memory_order_acquire);
        while(state==Impl::State::Ready || state==Impl::State::Running || state==Impl::State::Preparing) {
            pause();if(++spins==1024){spins=0;std::this_thread::yield();}
            state=impl_->workers[index]->state.load(std::memory_order_acquire);
        }
        if(impl_->workers[index]->completed.load(std::memory_order_relaxed))completed|=std::uint32_t{1}<<index;
    }
    return completed;
}
} // namespace adi::engine
