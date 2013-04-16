// Copyright (C) by Ashton Mason. See LICENSE.txt for licensing information.
#ifndef THERON_DETAIL_SCHEDULER_NONBLOCKINGQUEUE_H
#define THERON_DETAIL_SCHEDULER_NONBLOCKINGQUEUE_H


#include <Theron/BasicTypes.h>
#include <Theron/Defines.h>
#include <Theron/YieldStrategy.h>

#include <Theron/Detail/Containers/Queue.h>
#include <Theron/Detail/Mailboxes/Mailbox.h>
#include <Theron/Detail/Scheduler/YieldImplementation.h>
#include <Theron/Detail/Scheduler/YieldPolicy.h>
#include <Theron/Detail/Threading/SpinLock.h>


#ifdef _MSC_VER
#pragma warning(push)
#pragma warning (disable:4324)  // structure was padded due to __declspec(align())
#endif //_MSC_VER


namespace Theron
{
namespace Detail
{


/**
\brief Blocking queue implementation based on spinlocks.
*/
class NonBlockingQueue
{
public:

    /**
    The item type which is queued by the queue.
    */
    typedef Mailbox ItemType;

    /**
    Context structure used to access the queue.
    */
    class ContextType
    {
    public:

        friend class NonBlockingQueue;

        inline ContextType() :
          mShared(false),
          mLocalWorkQueue()
        {
        }

    private:

        bool mShared;                       ///< Indicates whether this is the 'shared' context.
        Queue<Mailbox> mLocalWorkQueue;     ///< Local thread-specific work queue.
        YieldImplementation mYield;         ///< Thread yield strategy implementation.
    };

    /**
    Constructor.
    */
    inline explicit NonBlockingQueue(const YieldStrategy yieldStrategy);

    /**
    Initializes a user-allocated context as the 'shared' context common to all threads.
    */
    inline void InitializeSharedContext(ContextType *const context);

    /**
    Initializes a user-allocated context as the context associated with the calling thread.
    */
    inline void InitializeWorkerContext(ContextType *const context);

    /**
    Releases a previously initialized shared context.
    */
    inline void ReleaseSharedContext(ContextType *const context);

    /**
    Releases a previously initialized worker thread context.
    */
    inline void ReleaseWorkerContext(ContextType *const context);

    /**
    Returns true if a call to Pop would return no mailbox, for the given context.
    */
    inline bool Empty(const ContextType *const context) const;

    /**
    Wakes any worker threads which are blocked waiting for the queue to become non-empty.
    */
    inline void WakeAll();

    /**
    Pushes a mailbox into the queue, scheduling it for processing.
    \param localThread A hint indicating that the mailbox should be processed by the same thread.
    */
    inline void Push(ContextType *const context, Mailbox *const mailbox, const bool localThread);

    /**
    Pops a previously pushed mailbox from the queue for processing.
    */
    inline Mailbox *Pop(ContextType *const context);

    /**
    Processes a previously popped mailbox using the provided processor and user context.
    */
    template <class UserContextType, class ProcessorType>
    inline void Process(
        ContextType *const context,
        UserContextType *const userContext,
        Mailbox *const mailbox);

private:

    NonBlockingQueue(const NonBlockingQueue &other);
    NonBlockingQueue &operator=(const NonBlockingQueue &other);

    YieldStrategy mYieldStrategy;
    mutable SpinLock mSharedWorkQueueSpinLock;      ///< Spinlock protecting the shared work queue.
    Queue<Mailbox> mSharedWorkQueue;                ///< Work queue shared by all the threads in a scheduler.
};


inline NonBlockingQueue::NonBlockingQueue(const YieldStrategy yieldStrategy) : mYieldStrategy(yieldStrategy)
{
}


inline void NonBlockingQueue::InitializeSharedContext(ContextType *const context)
{
    context->mShared = true;
}


inline void NonBlockingQueue::InitializeWorkerContext(ContextType *const context)
{
    // Only worker threads should call this method.
    context->mShared = false;

    switch (mYieldStrategy)
    {
        default:                        context->mYield.SetYieldFunction(&Detail::YieldPolicy::YieldPolite);       break;
        case YIELD_STRATEGY_POLITE:     context->mYield.SetYieldFunction(&Detail::YieldPolicy::YieldPolite);       break;
        case YIELD_STRATEGY_STRONG:     context->mYield.SetYieldFunction(&Detail::YieldPolicy::YieldStrong);       break;
        case YIELD_STRATEGY_AGGRESSIVE: context->mYield.SetYieldFunction(&Detail::YieldPolicy::YieldAggressive);   break;
    }
}


inline void NonBlockingQueue::ReleaseSharedContext(ContextType *const /*context*/)
{
}


inline void NonBlockingQueue::ReleaseWorkerContext(ContextType *const /*context*/)
{
}


THERON_FORCEINLINE bool NonBlockingQueue::Empty(const ContextType *const context) const
{
    // Check the context's local queue.
    // If the provided context is the shared context then it doesn't have a local queue.
    if (!context->mShared)
    {
        if (!context->mLocalWorkQueue.Empty())
        {
            return false;
        }
    }

    // Check the shared work queue.
    bool empty(true);
    mSharedWorkQueueSpinLock.Lock();

    if (!mSharedWorkQueue.Empty())
    {
        empty = false;
    }

    mSharedWorkQueueSpinLock.Unlock();
    return empty;
}


THERON_FORCEINLINE void NonBlockingQueue::WakeAll()
{
    // Queue implementation is non-blocking, so threads don't block and don't need waking.
}


THERON_FORCEINLINE void NonBlockingQueue::Push(ContextType *const context, Mailbox *const mailbox, const bool localThread)
{
    // Try to push the mailbox onto the calling thread's local work queue.
    // If the provided context is the shared context then it doesn't have a local queue.
    if (localThread && !context->mShared)
    {
        // The local queue in a per-thread context is only accessed by that thread
        // so we don't need to protect access to it.
        context->mLocalWorkQueue.Push(mailbox);
        return;
    }

    // Push the mailbox onto the shared work queue.
    // Because the shared queue is accessed by multiple threads we have to protect it.
    mSharedWorkQueueSpinLock.Lock();
    mSharedWorkQueue.Push(mailbox);
    mSharedWorkQueueSpinLock.Unlock();
}


THERON_FORCEINLINE Mailbox *NonBlockingQueue::Pop(ContextType *const context)
{
    Mailbox *mailbox(0);

    // The shared context is never used to call Pop, only to Push
    // messages sent outside the context of a worker thread.
    THERON_ASSERT(context->mShared == false);

    // We only check the shared queue once the local queue is empty.
    if (context->mLocalWorkQueue.Empty())
    {
        // Pop a mailbox off the shared work queue.
        // Because the shared queue is accessed by multiple threads we have to protect it.
        // In this implementation the shared queue is protected by a spinlock.
        mSharedWorkQueueSpinLock.Lock();

        if (!mSharedWorkQueue.Empty())
        {
            mailbox = static_cast<Mailbox *>(mSharedWorkQueue.Pop());
        }

        mSharedWorkQueueSpinLock.Unlock();
    }
    else
    {
        // Try to pop a mailbox off the calling thread's local work queue.
        mailbox = static_cast<Mailbox *>(context->mLocalWorkQueue.Pop());
    }

    if (mailbox)
    {
        context->mYield.Reset();
        return mailbox;
    }

    // Progressive backoff.
    context->mYield.Execute();
    return 0;
}


template <class UserContextType, class ProcessorType>
THERON_FORCEINLINE void NonBlockingQueue::Process(
    ContextType *const /*context*/,
    UserContextType *const userContext,
    Mailbox *const mailbox)
{
    ProcessorType::Process(userContext, mailbox);
}


} // namespace Detail
} // namespace Theron


#ifdef _MSC_VER
#pragma warning(pop)
#endif //_MSC_VER


#endif // THERON_DETAIL_SCHEDULER_NONBLOCKINGQUEUE_H
