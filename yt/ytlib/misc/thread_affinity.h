#pragma once

#include "common.h"
#include "assert.h"

#include <util/system/thread.h>

namespace NYT {
namespace NThreadAffinity {

////////////////////////////////////////////////////////////////////////////////

/*!
 * Allows to annotate certain functions with Thread affinity.
 * The checks are performed at run-time to ensure that each function
 * invocation that is annotated with a particular affinity slot
 * takes place in one thread.
 *
 * The usage is as follows.
 * - For each thread that may invoke your functions declare a slot with
 *   \code
 *   DECLARE_THREAD_AFFINITY_SLOT(Thread);
 *   \endcode
 * - Write
 *   \code
 *   VERIFY_THREAD_AFFINITY(Thread);
 *   \endcode
 *   at the beginning of each function in the group.
 *
 * Please refer to the unit test for an actual usage example
 * (unittests/thread_affinity_ut.cpp).
 */

// Check that the cast TThread::TId -> TAtomic is safe.
// NB: TAtomic is volatile intptr_t.
STATIC_ASSERT(sizeof(TThread::TId) == sizeof(intptr_t));

class TSlot
{
public:
    TSlot()
        : InvalidId(static_cast<intptr_t>(TThread::ImpossibleThreadId()))
        , BoundId(InvalidId)
    { }

    void Check()
    {
        intptr_t currentThreadId = static_cast<intptr_t>(TThread::CurrentThreadId());
        do {
            intptr_t boundThreadId = BoundId;
            if (boundThreadId != InvalidId) {
                YVERIFY(boundThreadId == currentThreadId);
                break;
            }
        } while (!AtomicCas(&BoundId, currentThreadId, InvalidId));
    }

private:
    intptr_t InvalidId;
    TAtomic BoundId;

};

#ifdef ENABLE_THREAD_AFFINITY_CHECK

#define DECLARE_THREAD_AFFINITY_SLOT(slot) \
    mutable ::NYT::NThreadAffinity::TSlot slot ## __Slot

#define VERIFY_THREAD_AFFINITY(slot) \
    slot ## __Slot.Check()

// TODO: remove this dirty hack.
STATIC_ASSERT(sizeof(TSpinLock) == sizeof(TAtomic));
#define VERIFY_SPINLOCK_AFFINITY(spinLock) \
    YASSERT(*reinterpret_cast<TAtomic*>(&(spinLock)) != 0);

#define VERIFY_INVOKER_AFFINITY(invoker, slot) \
    invoker->Invoke(FromMethod(&::NYT::NThreadAffinity::TSlot::Check, &slot ## __Slot))

#else

// Expand macros to null but take care of trailing semicolon.
#define DECLARE_THREAD_AFFINITY_SLOT(slot)     struct TNullThreadAffinitySlot__ ## __LINE__ { }
#define VERIFY_THREAD_AFFINITY(slot)           do { } while(0)
#define VERIFY_SPINLOCK_AFFINITY(spinLock)     do { } while(0)
#define VERIFY_INVOKER_AFFINITY(invoker, slot) do { } while(0)

#endif

//! This is a mere declaration and intentionally does not check anything.
#define VERIFY_THREAD_AFFINITY_ANY()         do { } while(0)

////////////////////////////////////////////////////////////////////////////////

} // namespace NThreadAffinity
} // namespace NYT
