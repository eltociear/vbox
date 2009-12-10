/** @file
 *
 * AutoWriteLock/AutoReadLock: smart R/W semaphore wrappers
 */

/*
 * Copyright (C) 2006-2000 Sun Microsystems, Inc.
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 USA or visit http://www.sun.com if you need
 * additional information or have any questions.
 */

#include "AutoLock.h"

#include "Logging.h"

#include <iprt/cdefs.h>
#include <iprt/critsect.h>
#include <iprt/thread.h>
#include <iprt/semaphore.h>

#include <iprt/err.h>
#include <iprt/assert.h>

#if defined(DEBUG)
# include <iprt/asm.h> // for ASMReturnAddress
#endif

#include <iprt/string.h>
#include <iprt/path.h>

#include <vector>
#include <list>

namespace util
{

////////////////////////////////////////////////////////////////////////////////
//
// Global variables
//
////////////////////////////////////////////////////////////////////////////////

#ifdef VBOX_WITH_DEBUG_LOCK_VALIDATOR
// index used for allocating thread-local storage for locking stack
RTTLS LockHandle::s_lockingStackTlsIndex = NIL_RTTLS;

/**
 * One item on the LockingStack. One of these gets pushed on the
 * stack for each lock operation and popped for each unlock.
 */
struct LockStackItem
{
    LockStackItem(LockHandle *pLock_,
                  const char *pcszFile_,
                  unsigned uLine_,
                  const char *pcszFunction_)
        : pLock(pLock_),
          pcszFile(pcszFile_),
          uLine(uLine_),
          pcszFunction(pcszFunction_)
    {
        pcszFile = RTPathFilename(pcszFile_);
    }

    LockHandle      *pLock;

    // information about where the lock occured (passed down from the AutoLock classes)
    const char      *pcszFile;
    unsigned        uLine;
    const char      *pcszFunction;
};

typedef std::list<LockStackItem> LockHandlesList;
struct LockingStack
{
    LockingStack()
        : threadSelf(RTThreadSelf()),
          pcszThreadName(NULL),
          c(0)
    {
        threadSelf = RTThreadSelf();
        pcszThreadName = RTThreadGetName(threadSelf);
    }

    RTTHREAD            threadSelf;
    const char          *pcszThreadName;

    LockHandlesList     ll;
    size_t              c;
};

LockingStack* getThreadLocalLockingStack()
{
    // very first call in this process: allocate the TLS variable
    if (LockHandle::s_lockingStackTlsIndex == NIL_RTTLS)
    {
        LockHandle::s_lockingStackTlsIndex = RTTlsAlloc();
        Assert(LockHandle::s_lockingStackTlsIndex != NIL_RTTLS);
    }

    // get pointer to thread-local locking stack
    LockingStack *pStack = (LockingStack*)RTTlsGet(LockHandle::s_lockingStackTlsIndex);
    if (!pStack)
    {
        // first call on this thread:
        pStack = new LockingStack;
        RTTlsSet(LockHandle::s_lockingStackTlsIndex, pStack);
    }

    return pStack;
}
#endif

////////////////////////////////////////////////////////////////////////////////
//
// LockHandle
//
////////////////////////////////////////////////////////////////////////////////

#ifdef VBOX_WITH_DEBUG_LOCK_VALIDATOR
void LockHandle::validateLock(LOCKVAL_SRC_POS_DECL)
{
    // put "this" on locking stack
    LockingStack *pStack = getThreadLocalLockingStack();
    LockStackItem lsi(this, RT_SRC_POS_ARGS);
    pStack->ll.push_back(lsi);
    ++pStack->c;

    LogFlow(("LOCKVAL: lock from %s (%s:%u), new count: %RI32\n", lsi.pcszFunction, lsi.pcszFile, lsi.uLine, (uint32_t)pStack->c));
}

void LockHandle::validateUnlock()
{
    // pop "this" from locking stack
    LockingStack *pStack = getThreadLocalLockingStack();

    LogFlow(("LOCKVAL: unlock, old count: %RI32\n", (uint32_t)pStack->c));

    AssertMsg(pStack->c == pStack->ll.size(), ("Locking size mismatch"));
    AssertMsg(pStack->c > 0, ("Locking stack is empty when it should have current LockHandle on top"));

    // validate that "this" is the top item on the stack
    LockStackItem &lsiTop = pStack->ll.back();
    if (lsiTop.pLock != this)
    {
        // violation of unlocking order: "this" was not the last to be locked on this thread,
        // see if it's somewhere deep under the locks
        bool fFound;
        uint32_t c = 0;
        for (LockHandlesList::iterator it = pStack->ll.begin();
             it != pStack->ll.end();
             ++it, ++c)
        {
            LockStackItem &lsiThis = *it;
            if (lsiThis.pLock == this)
            {
                AssertMsgFailed(("Unlocking order violation: unlock attempted for LockHandle which is %d items under the top item\n", c));
                pStack->ll.erase(it);
                fFound = true;
                break;
            }
        }

        if (!fFound)
            AssertMsgFailed(("Locking stack does not contain current LockHandle at all\n"));
    }
    else
        pStack->ll.pop_back();
    --pStack->c;
}
#endif // VBOX_WITH_DEBUG_LOCK_VALIDATOR

////////////////////////////////////////////////////////////////////////////////
//
// RWLockHandle
//
////////////////////////////////////////////////////////////////////////////////

struct RWLockHandle::Data
{
    Data()
    { }

    RTSEMRW sem;
};

RWLockHandle::RWLockHandle()
{
    m = new Data();
    int vrc = RTSemRWCreate(&m->sem);
    AssertRC(vrc);
}

/*virtual*/ RWLockHandle::~RWLockHandle()
{
    RTSemRWDestroy(m->sem);
    delete m;
}

/*virtual*/ bool RWLockHandle::isWriteLockOnCurrentThread() const
{
    return RTSemRWIsWriteOwner(m->sem);
}

/*virtual*/ void RWLockHandle::lockWrite(LOCKVAL_SRC_POS_DECL)
{
#ifdef VBOX_WITH_DEBUG_LOCK_VALIDATOR
    validateLock(LOCKVAL_SRC_POS_ARGS);
#endif
    int vrc = RTSemRWRequestWrite(m->sem, RT_INDEFINITE_WAIT);
    AssertRC(vrc);
}

/*virtual*/ void RWLockHandle::unlockWrite()
{
#ifdef VBOX_WITH_DEBUG_LOCK_VALIDATOR
    validateUnlock();
#endif
    int vrc = RTSemRWReleaseWrite(m->sem);
    AssertRC(vrc);

}

/*virtual*/ void RWLockHandle::lockRead(LOCKVAL_SRC_POS_DECL)
{
#ifdef VBOX_WITH_DEBUG_LOCK_VALIDATOR
    validateLock(LOCKVAL_SRC_POS_ARGS);
#endif
    int vrc = RTSemRWRequestRead(m->sem, RT_INDEFINITE_WAIT);
    AssertRC(vrc);
}

/*virtual*/ void RWLockHandle::unlockRead()
{
#ifdef VBOX_WITH_DEBUG_LOCK_VALIDATOR
    validateUnlock();
#endif
    int vrc = RTSemRWReleaseRead(m->sem);
    AssertRC(vrc);
}

/*virtual*/ uint32_t RWLockHandle::writeLockLevel() const
{
    return RTSemRWGetWriteRecursion(m->sem);
}

////////////////////////////////////////////////////////////////////////////////
//
// WriteLockHandle
//
////////////////////////////////////////////////////////////////////////////////

struct WriteLockHandle::Data
{
    Data()
    { }

    mutable RTCRITSECT sem;
};

WriteLockHandle::WriteLockHandle()
{
    m = new Data;
    RTCritSectInit(&m->sem);
}

WriteLockHandle::~WriteLockHandle()
{
    RTCritSectDelete(&m->sem);
    delete m;
}

/*virtual*/ bool WriteLockHandle::isWriteLockOnCurrentThread() const
{
    return RTCritSectIsOwner(&m->sem);
}

/*virtual*/ void WriteLockHandle::lockWrite(LOCKVAL_SRC_POS_DECL)
{
#ifdef VBOX_WITH_DEBUG_LOCK_VALIDATOR
    validateLock(LOCKVAL_SRC_POS_ARGS);
#endif

#if defined(DEBUG)
    RTCritSectEnterDebug(&m->sem,
                         "WriteLockHandle::lockWrite() return address >>>",
                         0, (RTUINTPTR)ASMReturnAddress());
#else
    RTCritSectEnter(&m->sem);
#endif
}

/*virtual*/ void WriteLockHandle::unlockWrite()
{
#ifdef VBOX_WITH_DEBUG_LOCK_VALIDATOR
    validateUnlock();
#endif

    RTCritSectLeave(&m->sem);
}

/*virtual*/ void WriteLockHandle::lockRead(LOCKVAL_SRC_POS_DECL)
{
    lockWrite(LOCKVAL_SRC_POS_ARGS);
}

/*virtual*/ void WriteLockHandle::unlockRead()
{
    unlockWrite();
}

/*virtual*/ uint32_t WriteLockHandle::writeLockLevel() const
{
    return RTCritSectGetRecursion(&m->sem);
}

////////////////////////////////////////////////////////////////////////////////
//
// AutoLockBase
//
////////////////////////////////////////////////////////////////////////////////

typedef std::vector<LockHandle*> HandlesVector;
typedef std::vector<uint32_t> CountsVector;

struct AutoLockBase::Data
{
    Data(size_t cHandles
#ifdef VBOX_WITH_DEBUG_LOCK_VALIDATOR
         , const char *pcszFile_,
         unsigned uLine_,
         const char *pcszFunction_
#endif
        )
        : fIsLocked(false),
          aHandles(cHandles),       // size of array
          acUnlockedInLeave(cHandles)
#ifdef VBOX_WITH_DEBUG_LOCK_VALIDATOR
          , pcszFile(pcszFile_),
          uLine(uLine_),
          pcszFunction(pcszFunction_)
#endif
    {
        for (uint32_t i = 0; i < cHandles; ++i)
        {
            acUnlockedInLeave[i] = 0;
            aHandles[i] = NULL;
        }
    }

    bool            fIsLocked;          // if true, then all items in aHandles are locked by this AutoLock and
                                        // need to be unlocked in the destructor
    HandlesVector   aHandles;           // array (vector) of LockHandle instances; in the case of AutoWriteLock
                                        // and AutoReadLock, there will only be one item on the list; with the
                                        // AutoMulti* derivatives, there will be multiple
    CountsVector    acUnlockedInLeave;  // for each lock handle, how many times the handle was unlocked in leave(); otherwise 0

#ifdef VBOX_WITH_DEBUG_LOCK_VALIDATOR
    // information about where the lock occured (passed down from the AutoLock classes)
    const char      *pcszFile;
    unsigned        uLine;
    const char      *pcszFunction;
#endif
};

AutoLockBase::AutoLockBase(uint32_t cHandles
                           COMMA_LOCKVAL_SRC_POS_DECL)
{
    m = new Data(cHandles
                 COMMA_LOCKVAL_SRC_POS_ARGS);
}

AutoLockBase::AutoLockBase(uint32_t cHandles,
                           LockHandle *pHandle
                           COMMA_LOCKVAL_SRC_POS_DECL)
{
    Assert(cHandles == 1);
    m = new Data(1
                 COMMA_LOCKVAL_SRC_POS_ARGS);
    m->aHandles[0] = pHandle;
}

AutoLockBase::~AutoLockBase()
{
    delete m;
}

/**
 * Requests ownership of all contained lock handles by calling
 * the pure virtual callLockImpl() function on each of them,
 * which must be implemented by the descendant class; in the
 * implementation, AutoWriteLock will request a write lock
 * whereas AutoReadLock will request a read lock.
 *
 * Does *not* modify the lock counts in the member variables.
 */
void AutoLockBase::callLockOnAllHandles()
{
    for (HandlesVector::iterator it = m->aHandles.begin();
         it != m->aHandles.end();
         ++it)
    {
        LockHandle *pHandle = *it;
        if (pHandle)
            // call virtual function implemented in AutoWriteLock or AutoReadLock
            this->callLockImpl(*pHandle);
    }
}

/**
 * Releases ownership of all contained lock handles by calling
 * the pure virtual callUnlockImpl() function on each of them,
 * which must be implemented by the descendant class; in the
 * implementation, AutoWriteLock will release a write lock
 * whereas AutoReadLock will release a read lock.
 *
 * Does *not* modify the lock counts in the member variables.
 */
void AutoLockBase::callUnlockOnAllHandles()
{
    // unlock in reverse order!
    for (HandlesVector::reverse_iterator it = m->aHandles.rbegin();
         it != m->aHandles.rend();
         ++it)
    {
        LockHandle *pHandle = *it;
        if (pHandle)
            // call virtual function implemented in AutoWriteLock or AutoReadLock
            this->callUnlockImpl(*pHandle);
    }
}

/**
 * Destructor implementation that can also be called explicitly, if required.
 * Restores the exact state before the AutoLock was created; that is, unlocks
 * all contained semaphores and might actually lock them again if leave()
 * was called during the AutoLock's lifetime.
 */
void AutoLockBase::cleanup()
{
    bool fAnyUnlockedInLeave = false;

    uint32_t i = 0;
    for (HandlesVector::iterator it = m->aHandles.begin();
         it != m->aHandles.end();
         ++it)
    {
        LockHandle *pHandle = *it;
        if (pHandle)
        {
            if (m->acUnlockedInLeave[i])
            {
                // there was a leave() before the destruction: then restore the
                // lock level that might have been set by locks other than our own
                if (m->fIsLocked)
                {
                    --m->acUnlockedInLeave[i];
                    fAnyUnlockedInLeave = true;
                }
                for (; m->acUnlockedInLeave[i]; --m->acUnlockedInLeave[i])
                    callLockImpl(*pHandle);
            }
        }
        ++i;
    }

    if (m->fIsLocked && !fAnyUnlockedInLeave)
        callUnlockOnAllHandles();
}

/**
 * Requests ownership of all contained semaphores. Public method that can
 * only be called once and that also gets called by the AutoLock constructors.
 */
void AutoLockBase::acquire()
{
    AssertMsg(!m->fIsLocked, ("m->fIsLocked is true, attempting to lock twice!"));
    callLockOnAllHandles();
    m->fIsLocked = true;
}

/**
 * Releases ownership of all contained semaphores. Public method.
 */
void AutoLockBase::release()
{
    AssertMsg(m->fIsLocked, ("m->fIsLocked is false, cannot release!"));
    callUnlockOnAllHandles();
    m->fIsLocked = false;
}

////////////////////////////////////////////////////////////////////////////////
//
// AutoReadLock
//
////////////////////////////////////////////////////////////////////////////////

/**
 * Release all read locks acquired by this instance through the #lock()
 * call and destroys the instance.
 *
 * Note that if there there are nested #lock() calls without the
 * corresponding number of #unlock() calls when the destructor is called, it
 * will assert. This is because having an unbalanced number of nested locks
 * is a program logic error which must be fixed.
 */
/*virtual*/ AutoReadLock::~AutoReadLock()
{
    LockHandle *pHandle = m->aHandles[0];

    if (pHandle)
    {
        if (m->fIsLocked)
            callUnlockImpl(*pHandle);
    }
}

/**
 * Implementation of the pure virtual declared in AutoLockBase.
 * This gets called by AutoLockBase.acquire() to actually request
 * the semaphore; in the AutoReadLock implementation, we request
 * the semaphore in read mode.
 */
/*virtual*/ void AutoReadLock::callLockImpl(LockHandle &l)
{
#ifdef VBOX_WITH_DEBUG_LOCK_VALIDATOR
    l.lockRead(m->pcszFile, m->uLine, m->pcszFunction);
#else
    l.lockRead();
#endif
}

/**
 * Implementation of the pure virtual declared in AutoLockBase.
 * This gets called by AutoLockBase.release() to actually release
 * the semaphore; in the AutoReadLock implementation, we release
 * the semaphore in read mode.
 */
/*virtual*/ void AutoReadLock::callUnlockImpl(LockHandle &l)
{
    l.unlockRead();
}

////////////////////////////////////////////////////////////////////////////////
//
// AutoWriteLockBase
//
////////////////////////////////////////////////////////////////////////////////

/**
 * Implementation of the pure virtual declared in AutoLockBase.
 * This gets called by AutoLockBase.acquire() to actually request
 * the semaphore; in the AutoWriteLock implementation, we request
 * the semaphore in write mode.
 */
/*virtual*/ void AutoWriteLockBase::callLockImpl(LockHandle &l)
{
#ifdef VBOX_WITH_DEBUG_LOCK_VALIDATOR
    l.lockWrite(m->pcszFile, m->uLine, m->pcszFunction);
#else
    l.lockWrite();
#endif
}

/**
 * Implementation of the pure virtual declared in AutoLockBase.
 * This gets called by AutoLockBase.release() to actually release
 * the semaphore; in the AutoWriteLock implementation, we release
 * the semaphore in write mode.
 */
/*virtual*/ void AutoWriteLockBase::callUnlockImpl(LockHandle &l)
{
    l.unlockWrite();
}

/**
 * Causes the current thread to completely release the write lock to make
 * the managed semaphore immediately available for locking by other threads.
 *
 * This implies that all nested write locks on the semaphore will be
 * released, even those that were acquired through the calls to #lock()
 * methods of all other AutoWriteLock/AutoReadLock instances managing the
 * <b>same</b> read/write semaphore.
 *
 * After calling this method, the only method you are allowed to call is
 * #enter(). It will acquire the write lock again and restore the same
 * level of nesting as it had before calling #leave().
 *
 * If this instance is destroyed without calling #enter(), the destructor
 * will try to restore the write lock level that existed when #leave() was
 * called minus the number of nested #lock() calls made on this instance
 * itself. This is done to preserve lock levels of other
 * AutoWriteLock/AutoReadLock instances managing the same semaphore (if
 * any). Tiis also means that the destructor may indefinitely block if a
 * write or a read lock is owned by some other thread by that time.
 */
void AutoWriteLockBase::leave()
{
    AssertMsg(m->fIsLocked, ("m->fIsLocked is false, cannot leave()!"));

    // unlock in reverse order!
    uint32_t i = 0;
    for (HandlesVector::reverse_iterator it = m->aHandles.rbegin();
         it != m->aHandles.rend();
         ++it)
    {
        LockHandle *pHandle = *it;
        if (pHandle)
        {
            AssertMsg(m->acUnlockedInLeave[i] == 0, ("m->cUnlockedInLeave[%d] is %d, must be 0! Called leave() twice?", i, m->acUnlockedInLeave[i]));
            m->acUnlockedInLeave[i] = pHandle->writeLockLevel();
            AssertMsg(m->acUnlockedInLeave[i] >= 1, ("m->cUnlockedInLeave[%d] is %d, must be >=1!", i, m->acUnlockedInLeave[i]));

            for (uint32_t left = m->acUnlockedInLeave[i];
                 left;
                 --left)
                callUnlockImpl(*pHandle);
        }
        ++i;
    }
}

/**
 * Causes the current thread to restore the write lock level after the
 * #leave() call. This call will indefinitely block if another thread has
 * successfully acquired a write or a read lock on the same semaphore in
 * between.
 */
void AutoWriteLockBase::enter()
{
    AssertMsg(m->fIsLocked, ("m->fIsLocked is false, cannot enter()!"));

    uint32_t i = 0;
    for (HandlesVector::iterator it = m->aHandles.begin();
         it != m->aHandles.end();
         ++it)
    {
        LockHandle *pHandle = *it;
        if (pHandle)
        {
            AssertMsg(m->acUnlockedInLeave[i] != 0, ("m->cUnlockedInLeave[%d] is 0! enter() without leave()?", i));

            for (; m->acUnlockedInLeave[i]; --m->acUnlockedInLeave[i])
                callLockImpl(*pHandle);
        }
        ++i;
    }
}

/**
 * Same as #leave() but checks if the current thread actally owns the lock
 * and only proceeds in this case. As a result, as opposed to #leave(),
 * doesn't assert when called with no lock being held.
 */
void AutoWriteLockBase::maybeLeave()
{
    // unlock in reverse order!
    uint32_t i = 0;
    for (HandlesVector::reverse_iterator it = m->aHandles.rbegin();
         it != m->aHandles.rend();
         ++it)
    {
        LockHandle *pHandle = *it;
        if (pHandle)
        {
            if (pHandle->isWriteLockOnCurrentThread())
            {
                m->acUnlockedInLeave[i] = pHandle->writeLockLevel();
                AssertMsg(m->acUnlockedInLeave[i] >= 1, ("m->cUnlockedInLeave[%d] is %d, must be >=1!", i, m->acUnlockedInLeave[i]));

                for (uint32_t left = m->acUnlockedInLeave[i];
                     left;
                     --left)
                    callUnlockImpl(*pHandle);
            }
        }
        ++i;
    }
}

/**
 * Same as #enter() but checks if the current thread actally owns the lock
 * and only proceeds if not. As a result, as opposed to #enter(), doesn't
 * assert when called with the lock already being held.
 */
void AutoWriteLockBase::maybeEnter()
{
    uint32_t i = 0;
    for (HandlesVector::iterator it = m->aHandles.begin();
         it != m->aHandles.end();
         ++it)
    {
        LockHandle *pHandle = *it;
        if (pHandle)
        {
            if (!pHandle->isWriteLockOnCurrentThread())
            {
                for (; m->acUnlockedInLeave[i]; --m->acUnlockedInLeave[i])
                    callLockImpl(*pHandle);
            }
        }
        ++i;
    }
}

////////////////////////////////////////////////////////////////////////////////
//
// AutoWriteLock
//
////////////////////////////////////////////////////////////////////////////////

/**
 * Attaches another handle to this auto lock instance.
 *
 * The previous object's lock is completely released before the new one is
 * acquired. The lock level of the new handle will be the same. This
 * also means that if the lock was not acquired at all before #attach(), it
 * will not be acquired on the new handle too.
 *
 * @param aHandle   New handle to attach.
 */
void AutoWriteLock::attach(LockHandle *aHandle)
{
    LockHandle *pHandle = m->aHandles[0];

    /* detect simple self-reattachment */
    if (pHandle != aHandle)
    {
        bool fWasLocked = m->fIsLocked;

        cleanup();

        m->aHandles[0] = aHandle;
        m->fIsLocked = fWasLocked;

        if (aHandle)
            if (fWasLocked)
                callLockImpl(*aHandle);
    }
}

/**
 * Returns @c true if the current thread holds a write lock on the managed
 * read/write semaphore. Returns @c false if the managed semaphore is @c
 * NULL.
 *
 * @note Intended for debugging only.
 */
bool AutoWriteLock::isWriteLockOnCurrentThread() const
{
    return m->aHandles[0] ? m->aHandles[0]->isWriteLockOnCurrentThread() : false;
}

 /**
 * Returns the current write lock level of the managed smaphore. The lock
 * level determines the number of nested #lock() calls on the given
 * semaphore handle. Returns @c 0 if the managed semaphore is @c
 * NULL.
 *
 * Note that this call is valid only when the current thread owns a write
 * lock on the given semaphore handle and will assert otherwise.
 *
 * @note Intended for debugging only.
 */
uint32_t AutoWriteLock::writeLockLevel() const
{
    return m->aHandles[0] ? m->aHandles[0]->writeLockLevel() : 0;
}

////////////////////////////////////////////////////////////////////////////////
//
// AutoMultiWriteLock*
//
////////////////////////////////////////////////////////////////////////////////

AutoMultiWriteLock2::AutoMultiWriteLock2(Lockable *pl1,
                                         Lockable *pl2
                                         COMMA_LOCKVAL_SRC_POS_DECL)
    : AutoWriteLockBase(2
                        COMMA_LOCKVAL_SRC_POS_ARGS)
{
    if (pl1)
        m->aHandles[0] = pl1->lockHandle();
    if (pl2)
        m->aHandles[1] = pl2->lockHandle();
    acquire();
}

AutoMultiWriteLock2::AutoMultiWriteLock2(LockHandle *pl1,
                                         LockHandle *pl2
                                         COMMA_LOCKVAL_SRC_POS_DECL)
    : AutoWriteLockBase(2
                        COMMA_LOCKVAL_SRC_POS_ARGS)
{
    m->aHandles[0] = pl1;
    m->aHandles[1] = pl2;
    acquire();
}

AutoMultiWriteLock3::AutoMultiWriteLock3(Lockable *pl1,
                                         Lockable *pl2,
                                         Lockable *pl3
                                         COMMA_LOCKVAL_SRC_POS_DECL)
    : AutoWriteLockBase(3
                        COMMA_LOCKVAL_SRC_POS_ARGS)
{
    if (pl1)
        m->aHandles[0] = pl1->lockHandle();
    if (pl2)
        m->aHandles[1] = pl2->lockHandle();
    if (pl3)
        m->aHandles[2] = pl3->lockHandle();
    acquire();
}

AutoMultiWriteLock3::AutoMultiWriteLock3(LockHandle *pl1,
                                         LockHandle *pl2,
                                         LockHandle *pl3
                                         COMMA_LOCKVAL_SRC_POS_DECL)
    : AutoWriteLockBase(3
                        COMMA_LOCKVAL_SRC_POS_ARGS)
{
    m->aHandles[0] = pl1;
    m->aHandles[1] = pl2;
    m->aHandles[2] = pl3;
    acquire();
}

} /* namespace util */
/* vi: set tabstop=4 shiftwidth=4 expandtab: */
