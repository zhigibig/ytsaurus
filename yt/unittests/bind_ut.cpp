#include "stdafx.h"

#include <ytlib/misc/new.h>
#include <ytlib/actions/bind.h>
#include <ytlib/actions/callback.h>

#include <contrib/testing/framework.h>

using ::testing::Mock;
using ::testing::Return;
using ::testing::AllOf;
using ::testing::StrictMock;

namespace NYT {
namespace {

////////////////////////////////////////////////////////////////////////////////
// Auxiliary types and functions.

// An incomplete type (really).
class TIncompleteType;

// A simple mock.
class TObject
{
public:
    TObject()
    { }

    MOCK_METHOD0(VoidMethod0, void(void));
    MOCK_CONST_METHOD0(VoidConstMethod0, void(void));

    MOCK_METHOD0(IntMethod0, int(void));
    MOCK_CONST_METHOD0(IntConstMethod0, int(void));

private:
    // Explicitly non-copyable and non-movable.
    // Particularly important in this test to ensure that no copies are made.
    TObject(const TObject&);
    TObject(TObject&&);
    TObject& operator=(const TObject&);
    TObject& operator=(TObject&&);
};

// A simple mock which also mocks Ref()/UnRef() hence mocking reference counting
// behaviour.
class TObjectWithRC
    : public TObject
{
public:
    typedef TIntrusivePtr<TObjectWithRC> TPtr;

    TObjectWithRC()
    { }

    MOCK_CONST_METHOD0(Ref, void(void));
    MOCK_CONST_METHOD0(UnRef, void(void));

private:
    // Explicitly non-copyable and non-movable.
    // Particularly important in this test to ensure that no copies are made.
    TObjectWithRC(const TObjectWithRC&);
    TObjectWithRC(TObjectWithRC&&);
    TObjectWithRC& operator=(const TObjectWithRC&);
    TObjectWithRC& operator=(TObjectWithRC&&);
};

// A simple mock object which mocks Ref()/UnRef() and prohibits
// public destruction.
class TObjectWithRCAndPrivateDtor
    : public TObjectWithRC 
{
private:
    ~TObjectWithRCAndPrivateDtor()
    { }
};

// A simple mock object with real extrinsic reference counting.
class TObjectWithExtrinsicRC
    : public TObject
    , public TExtrinsicRefCounted
{
public:
    typedef TIntrusivePtr<TObjectWithExtrinsicRC> TPtr;
    typedef TIntrusivePtr<const TObjectWithExtrinsicRC> TConstPtr;
    typedef TWeakPtr<TObjectWithExtrinsicRC> TWkPtr;
    typedef TWeakPtr<const TObjectWithExtrinsicRC> TConstWkPtr;
};

// Below there is a serie of either reference-counted or not classes
// with simple inheritance and both virtual and non-virtual methods.

static const int SomeParentValue = 1;
static const int SomeChildValue = 2;

class RefParent
{
public:
    // Stub methods for reference counting.
    void Ref(void)
    { }
    void UnRef(void)
    { }

    virtual void VirtualSet()
    {
        Value = SomeParentValue;
    }
    void NonVirtualSet()
    {
        Value = SomeParentValue;
    }

    int Value;
};

class RefChild
    : public RefParent
{
public:
    virtual void VirtualSet()
    {
        Value = SomeChildValue;
    }
    void NonVirtualSet()
    {
        Value = SomeChildValue;
    }
};

class NoRefParent
{
public:
    virtual void VirtualSet()
    {
        Value = SomeParentValue;
    }
    void NonVirtualSet()
    {
        Value = SomeParentValue;
    }

    int Value;
};

class NoRefChild
    : public NoRefParent
{
    virtual void VirtualSet()
    {
        Value = SomeChildValue;
    }
    void NonVirtualSet()
    {
        Value = SomeChildValue;
    }
};

int UnwrapNoRefParent(NoRefParent p)
{
    return p.Value;
}

int UnwrapNoRefParentPtr(NoRefParent* p)
{
    return p->Value;
}

int UnwrapNoRefParentConstRef(const NoRefParent& p)
{
    return p.Value;
}

// Below there is a serie of probe classes.

// A state for probes that keeps various calls counts.
struct TProbeState
{
    int Constructors;
    int Destructors;
    int CopyConstructors;
    int CopyAssignments;
    int MoveConstructors;
    int MoveAssignments;

    TProbeState()
        : Constructors(0)
        , Destructors(0)
        , CopyConstructors(0)
        , CopyAssignments(0)
        , MoveConstructors(0)
        , MoveAssignments(0)
    { }

    void Reset()
    {
        ::memset(this, 0, sizeof(*this));
    }
};

// Used for probing the number of copies that occur if a type must be coerced
// during argument forwarding in the Run() methods.
class TCoercibleToProbe
{
public:
    TProbeState* State;

public:
    explicit TCoercibleToProbe(TProbeState* state)
        : State(state)
    { }

private:
    TCoercibleToProbe(const TCoercibleToProbe&);
    TCoercibleToProbe(TCoercibleToProbe&&);
    TCoercibleToProbe& operator=(const TCoercibleToProbe&);
    TCoercibleToProbe& operator=(TCoercibleToProbe&&);
};

// Used for probing the number of copies in an argument.
class TProbe
{
public:
    TProbeState* State;

public:
    static TProbe ExplicitlyCreateInvalidProbe()
    {
        return TProbe();
    }

    explicit TProbe(TProbeState* state)
        : State(state)
    {
        YASSERT(State);
        ++State->Constructors;
    }

    ~TProbe()
    {
        if (State) {
            ++State->Destructors;
        }
    }

    TProbe(const TProbe& other)
        : State(other.State)
    {
        YASSERT(State);
        ++State->CopyConstructors;
    }

    TProbe(TProbe&& other)
        : State(other.State)
    {
        YASSERT(State);
        other.State = NULL;
        ++State->MoveConstructors;
    }

    TProbe(const TCoercibleToProbe& other)
        : State(other.State)
    {
        YASSERT(State);
        ++State->CopyConstructors;
    }

    TProbe(TCoercibleToProbe&& other)
        : State(other.State)
    {
        YASSERT(State);
        other.State = NULL;
        ++State->MoveConstructors;
    }

    TProbe& operator=(const TProbe& other)
    {
        State = other.State;
        YASSERT(State);
        ++State->CopyAssignments;
        return *this;
    }

    TProbe& operator=(TProbe&& other)
    {
        State = other.State;
        YASSERT(State);
        other.State = NULL;
        ++State->MoveAssignments;
        return *this;
    }

    void Tackle() const
    {
        (void)0;
    }

    bool IsValid() const
    {
        return State != NULL;
    }

private:
    TProbe()
        : State(NULL)
    { }
};

void Tackle(const TProbe& probe)
{
    probe.Tackle();
}

// A helper functor which extracts from probe-like objectss their state.
struct TProbableTraits
{
    static const TProbeState& ExtractState(const TProbeState& arg)
    {
        return arg;
    }

    static const TProbeState& ExtractState(const TProbeState* arg)
    {
        return *arg;
    }

    static const TProbeState& ExtractState(const TProbe& arg)
    {
        return *arg.State;
    }

    static const TProbeState& ExtractState(const TCoercibleToProbe& arg)
    {
        return *arg.State;
    }
};

MATCHER_P2(HasCopyMoveCounts, copyCount, moveCount, "" + \
    ::testing::PrintToString(copyCount) + " copy constructors and " + \
    ::testing::PrintToString(moveCount) + " move constructors were called")
{
    UNUSED(result_listener);
    const TProbeState& state = TProbableTraits::ExtractState(arg);
    return
        state.CopyConstructors == copyCount &&
        state.MoveConstructors == moveCount;
}

MATCHER(NoCopies, "no copies were made")
{
    UNUSED(result_listener);
    const TProbeState& state = TProbableTraits::ExtractState(arg);
    return state.CopyConstructors == 0 && state.CopyAssignments == 0;
}

MATCHER(NoMoves, "no moves were made")
{
    UNUSED(result_listener);
    const TProbeState& state = TProbableTraits::ExtractState(arg);
    return state.MoveConstructors == 0 && state.MoveAssignments == 0;
}

MATCHER(NoAssignments, "no assignments were made")
{
    UNUSED(result_listener);
    const TProbeState& state = TProbableTraits::ExtractState(arg);
    return state.CopyAssignments == 0 && state.MoveAssignments == 0;
}

void PrintTo(const TProbeState& state, ::std::ostream* os)
{
    int copies = state.CopyConstructors + state.CopyAssignments;
    int moves = state.MoveConstructors + state.MoveAssignments;

    *os << Sprintf(
        "%d ctors, %d dtors; "
        "copyable semantics: %d = %d + %d; "
         "movable semantics: %d = %d + %d",
         state.Constructors, state.Destructors,
         copies, state.CopyConstructors, state.CopyAssignments,
         moves, state.MoveConstructors, state.MoveAssignments
    ).c_str();
}

void PrintTo(const TProbe& arg, ::std::ostream* os)
{
    PrintTo(TProbableTraits::ExtractState(arg), os);
}

void PrintTo(const TCoercibleToProbe& arg, ::std::ostream* os)
{
    PrintTo(TProbableTraits::ExtractState(arg), os);
}

// Various functions for testing purposes.

int IntegerIdentity(int n)
{
    return n;
}

const char* StringIdentity(const char* s)
{
    return s;
}

template <class T>
T PolymorphicIdentity(T t)
{
    return t; // Copy
}

template <class T>
T PolymorphicPassThrough(T t)
{
    return MoveRV(t); // Move
}

template <class T>
void VoidPolymorphic1(T t)
{
    UNUSED(t); 
}

int ArrayGet(const int array[], int n)
{
    return array[n];
}

int Sum(int a, int b, int c, int d, int e, int f)
{
    // Sum(1, 2, 3, 4, 5, 6) -> 123456.
    return f + 10 * (e + 10 * (d + 10 * (c + 10 * (b + 10 * a))));
}

void SetIntViaRef(int& n)
{
    n = 2012;
}

void SetIntViaPtr(int* n)
{
    *n = 2012;
}

template <class T>
int FunctionWithWeakParam(TWeakPtr<T> ptr, int n)
{
    return n;
}

void InvokeClosure(const TClosure& callback)
{
    callback.Run();
}

////////////////////////////////////////////////////////////////////////////////
// Test fixture.

class TBindTest : public ::testing::Test {
public:
    TBindTest()
    { }

    virtual void SetUp()
    {
        ConstObjectWithRCPtr = &ObjectWithRC;
        ConstObjectPtr = &Object;
        StaticObjectPtr = &StaticObject;
    }

    // Helper methods.
    static void StaticVoidFunc0(void)
    {
        StaticObjectPtr->VoidMethod0();
    }

    static int StaticIntFunc0(void)
    {
        return StaticObjectPtr->IntMethod0();
    }

protected:
    StrictMock<TObject> Object;
    StrictMock<TObjectWithRC> ObjectWithRC;

    const TObjectWithRC* ConstObjectWithRCPtr;
    const TObject* ConstObjectPtr;

    StrictMock<TObject> StaticObject;

    // Used by the static functions.
    static StrictMock<TObject>* StaticObjectPtr;

private:
    // Explicitly non-copyable and non-movable.
    // Thus we prevent Bind() from taking copy of the target (i. e. this fixture).
    TBindTest(const TBindTest&);
    TBindTest(TBindTest&&);
    TBindTest& operator=(const TBindTest&);
    TBindTest& operator=(TBindTest&&);
};

StrictMock<TObject>* TBindTest::StaticObjectPtr = 0;

////////////////////////////////////////////////////////////////////////////////
// Test definitions.

// Sanity check that we can instantiate a callback for each arity.
TEST_F(TBindTest, ArityTest)
{
    TCallback<int(void)> c0 = Bind(&Sum, 5, 4, 3, 2, 1, 0);
    EXPECT_EQ(543210, c0.Run());

    TCallback<int(int)> c1 = Bind(&Sum, 5, 4, 3, 2, 1);
    EXPECT_EQ(543219, c1.Run(9));

    TCallback<int(int,int)> c2 = Bind(&Sum, 5, 4, 3, 2);
    EXPECT_EQ(543298, c2.Run(9, 8));

    TCallback<int(int,int,int)> c3 = Bind(&Sum, 5, 4, 3);
    EXPECT_EQ(543987, c3.Run(9, 8, 7));

    TCallback<int(int,int,int,int)> c4 = Bind(&Sum, 5, 4);
    EXPECT_EQ(549876, c4.Run(9, 8, 7, 6));

    TCallback<int(int,int,int,int,int)> c5 = Bind(&Sum, 5);
    EXPECT_EQ(598765, c5.Run(9, 8, 7, 6, 5));

    TCallback<int(int,int,int,int,int,int)> c6 = Bind(&Sum);
    EXPECT_EQ(987654, c6.Run(9, 8, 7, 6, 5, 4));
}

// Test the currying ability of the Bind().
TEST_F(TBindTest, CurryingTest)
{
    TCallback<int(int,int,int,int,int,int)> c6 = Bind(&Sum);
    EXPECT_EQ(987654, c6.Run(9, 8, 7, 6, 5, 4));

    TCallback<int(int,int,int,int,int)> c5 = Bind(c6, 5);
    EXPECT_EQ(598765, c5.Run(9, 8, 7, 6, 5));

    TCallback<int(int,int,int,int)> c4 = Bind(c5, 4);
    EXPECT_EQ(549876, c4.Run(9, 8, 7, 6));

    TCallback<int(int,int,int)> c3 = Bind(c4, 3);
    EXPECT_EQ(543987, c3.Run(9, 8, 7));

    TCallback<int(int,int)> c2 = Bind(c3, 2);
    EXPECT_EQ(543298, c2.Run(9, 8));

    TCallback<int(int)> c1 = Bind(c2, 1);
    EXPECT_EQ(543219, c1.Run(9));

    TCallback<int(void)> c0 = Bind(c1, 0);
    EXPECT_EQ(543210, c0.Run());
}

// Test that currying the rvalue result of another Bind() works correctly.
//   - Rvalue should be usable as an argument to the Bind().
//   - Multiple runs of resulting TCallback remain valid.
TEST_F(TBindTest, CurryingRvalueResultOfBind)
{
    int x;
    TClosure cb = NYT::Bind(&InvokeClosure, NYT::Bind(&SetIntViaPtr, &x));

    // If we implement Bind() such that the return value has auto_ptr-like
    // semantics, the second call here will fail because ownership of
    // the internal BindState<> would have been transfered to a *temporary*
    // constructon of a TCallback object on the first call.
    x = 0;
    cb.Run();
    EXPECT_EQ(2012, x);

    x = 0;
    cb.Run();
    EXPECT_EQ(2012, x);
}

// Now we have to test that there are proper instantinations for various use cases.
// The following test cases try to cover most of the used cases.

// Function type support.
//   - Normal function.
//   - Normal function bound with a non-refcounted first argument.
//   - Method bound to an object via raw pointer.
//   - Method bound to an object via intrusive pointer.
//   - Const method bound to a non-const object.
//   - Const method bound to a const object.
//   - Derived classes can be used with pointers to non-virtual base functions.
//   - Derived classes can be used with pointers to virtual base functions
//     (preserves virtual dispatch).
TEST_F(TBindTest, FunctionTypeSupport)
{
    EXPECT_CALL(StaticObject, VoidMethod0());

    EXPECT_CALL(ObjectWithRC, Ref()).Times(4);
    EXPECT_CALL(ObjectWithRC, UnRef()).Times(4);

    EXPECT_CALL(ObjectWithRC, VoidMethod0()).Times(2);
    EXPECT_CALL(ObjectWithRC, VoidConstMethod0()).Times(2);

    // Normal functions.
    TClosure normalFunc =
        Bind(&StaticVoidFunc0);
    TCallback<TObject*(void)> normalFuncNonRC =
        Bind(&PolymorphicIdentity<TObject*>, &Object);

    normalFunc.Run();
    EXPECT_EQ(&Object, normalFuncNonRC.Run());

    // Bound methods.
    TClosure boundMethodViaRawPtr =
        Bind(&TObjectWithRC::VoidMethod0, &ObjectWithRC); // (Ref)
    TClosure boundMethodViaRefPtr =
        Bind(&TObjectWithRC::VoidMethod0, TObjectWithRC::TPtr(&ObjectWithRC)); // (Ref)

    boundMethodViaRawPtr.Run();
    boundMethodViaRefPtr.Run();

    // Const-methods.
    TClosure constMethodNonConstObject =
        Bind(&TObjectWithRC::VoidConstMethod0, &ObjectWithRC); // (Ref)
    TClosure constMethodConstObject =
        Bind(&TObjectWithRC::VoidConstMethod0, ConstObjectWithRCPtr); // (Ref)

    constMethodNonConstObject.Run();
    constMethodConstObject.Run();

    // Virtual calls.
    RefChild child;

    child.Value = 0;
    TClosure virtualSet = Bind(&RefParent::VirtualSet, &child);
    virtualSet.Run();
    EXPECT_EQ(SomeChildValue, child.Value);

    child.Value = 0;
    TClosure nonVirtualSet = Bind(&RefParent::NonVirtualSet, &child);
    nonVirtualSet.Run();
    EXPECT_EQ(SomeParentValue, child.Value);
}

// Return value support.
//   - Function with a return value.
//   - Method with a return value.
//   - Const method with a return value.
TEST_F(TBindTest, ReturnValuesSupport)
{
    EXPECT_CALL(StaticObject, IntMethod0()).WillOnce(Return(13));

    EXPECT_CALL(ObjectWithRC, Ref()).Times(3);
    EXPECT_CALL(ObjectWithRC, UnRef()).Times(3);

    EXPECT_CALL(ObjectWithRC, IntMethod0()).WillOnce(Return(17));
    EXPECT_CALL(ObjectWithRC, IntConstMethod0())
        .WillOnce(Return(19))
        .WillOnce(Return(23));

    TCallback<int(void)> normalFunc =
        Bind(&StaticIntFunc0);
    TCallback<int(void)> boundMethod =
        Bind(&TObjectWithRC::IntMethod0, &ObjectWithRC); // (Ref)

    EXPECT_EQ(13, normalFunc.Run());
    EXPECT_EQ(17, boundMethod.Run());

    TCallback<int(void)> constMethodNonConstObject =
        Bind(&TObjectWithRC::IntConstMethod0, &ObjectWithRC); // (Ref)
    TCallback<int(void)> constMethodConstObject =
        Bind(&TObjectWithRC::IntConstMethod0, ConstObjectWithRCPtr); // (Ref)

    EXPECT_EQ(19, constMethodNonConstObject.Run());
    EXPECT_EQ(23, constMethodConstObject.Run());
}

// An ability to ignore returned value.
//   - Function with a return value.
//   - Method with a return value.
//   - Const Method with a return value.
//   - Method with a return value bound to a weak pointer.
//   - Const Method with a return value bound to a weak pointer.
TEST_F(TBindTest, IgnoreResultWrapper)
{
    EXPECT_CALL(StaticObject, IntMethod0()).WillOnce(Return(13));

    EXPECT_CALL(ObjectWithRC, Ref()).Times(2);
    EXPECT_CALL(ObjectWithRC, UnRef()).Times(2);

    EXPECT_CALL(ObjectWithRC, IntMethod0()).WillOnce(Return(17));
    EXPECT_CALL(ObjectWithRC, IntConstMethod0()).WillOnce(Return(19));

    TClosure normalFunc =
        Bind(IgnoreResult(&StaticIntFunc0));
    normalFunc.Run();

    TClosure boundMethod =
        Bind(IgnoreResult(&TObjectWithRC::IntMethod0), &ObjectWithRC);
    boundMethod.Run();

    TClosure constBoundMethod =
        Bind(IgnoreResult(&TObjectWithRC::IntConstMethod0), &ObjectWithRC);
    constBoundMethod.Run();
}

// Argument binding tests.
//   - Argument binding to a primitive.
//   - Argument binding to a primitive pointer.
//   - Argument binding to a literal integer.
//   - Argument binding to a literal string.
//   - Argument binding with template function.
//   - Argument binding to an object.
//   - Argument binding to a pointer to an incomplete type.
//   - Argument upcasts when required.
TEST_F(TBindTest, ArgumentBindingSupport)
{
    int n = 1;

    TCallback<int(void)> primitiveBind =
        Bind(&IntegerIdentity, n);
    EXPECT_EQ(n, primitiveBind.Run());

    TCallback<int*(void)> primitivePointerBind =
        Bind(&PolymorphicIdentity<int*>, &n);
    EXPECT_EQ(&n, primitivePointerBind.Run());

    TCallback<int(void)> literalIntegerBind
        = Bind(&IntegerIdentity, 2);
    EXPECT_EQ(2, literalIntegerBind.Run());

    TCallback<const char*(void)> literalStringBind =
        Bind(&StringIdentity, "Dire Straits");
    EXPECT_STREQ("Dire Straits", literalStringBind.Run());

    TCallback<int(void)> templateFunctionBind =
        Bind(&PolymorphicIdentity<int>, 3);
    EXPECT_EQ(3, templateFunctionBind.Run());

    NoRefParent p;
    p.Value = 4;

    TCallback<int(void)> objectBind = Bind(&UnwrapNoRefParent, p);
    EXPECT_EQ(4, objectBind.Run());

    TIncompleteType* dummyPtr = reinterpret_cast<TIncompleteType*>(123);
    TCallback<TIncompleteType*(void)> incompleteTypeBind =
        Bind(&PolymorphicIdentity<TIncompleteType*>, dummyPtr);
    EXPECT_EQ(dummyPtr, incompleteTypeBind.Run());

    NoRefChild c;

    c.Value = 5;
    TCallback<int(void)> upcastBind =
        Bind(&UnwrapNoRefParent, c);
    EXPECT_EQ(5, upcastBind.Run());

    c.Value = 6;
    TCallback<int(void)> upcastPtrBind =
        Bind(&UnwrapNoRefParentPtr, &c);
    EXPECT_EQ(6, upcastPtrBind.Run());

    c.Value = 7;
    TCallback<int(void)> upcastConstRefBind =
        Bind(&UnwrapNoRefParentConstRef, c);
    EXPECT_EQ(7, upcastConstRefBind.Run());
}

// Unbound argument type support tests.
//   - Unbound value.
//   - Unbound pointer.
//   - Unbound reference.
//   - Unbound const reference.
//   - Unbound unsized array.
//   - Unbound sized array.
//   - Unbound array-of-arrays.
TEST_F(TBindTest, UnboundArgumentTypeSupport)
{
    // Check only for valid instatination.
    TCallback<void(int)> unboundValue =
        Bind(&VoidPolymorphic1<int>);
    TCallback<void(int*)> unboundPtr =
        Bind(&VoidPolymorphic1<int*>);
    TCallback<void(int&)> unboundRef =
        Bind(&VoidPolymorphic1<int&>);
    TCallback<void(const int&)> unboundConstRef =
        Bind(&VoidPolymorphic1<const int&>);
    TCallback<void(int[])> unboundUnsizedArray =
        Bind(&VoidPolymorphic1<int[]>);
    TCallback<void(int[3])> unboundSizedArray =
        Bind(&VoidPolymorphic1<int[3]>);
    TCallback<void(int[][3])> unboundArrayOfArrays =
        Bind(&VoidPolymorphic1<int[][3]>);

    SUCCEED();
}

// Function with unbound reference parameter.
//   - Original parameter is modified by the callback.
TEST_F(TBindTest, UnboundReference)
{
    int n = 0;
    TCallback<void(int&)> unboundRef = Bind(&SetIntViaRef);
    unboundRef.Run(n);
    EXPECT_EQ(2012, n);
}

// Functions that take reference parameters.
//   - Forced reference parameter type still stores a copy.
//   - Forced const reference parameter type still stores a copy.
TEST_F(TBindTest, ReferenceArgumentBinding)
{
    int myInt = 1;
    int& myIntRef = myInt;
    const int& myIntConstRef = myInt;

    TCallback<int(void)> firstAction =
        Bind(&IntegerIdentity, myIntRef);
    EXPECT_EQ(1, firstAction.Run());
    myInt++;
    EXPECT_EQ(1, firstAction.Run());

    TCallback<int(void)> secondAction =
        Bind(&IntegerIdentity, myIntConstRef);
    EXPECT_EQ(2, secondAction.Run());
    myInt++;
    EXPECT_EQ(2, secondAction.Run());

    EXPECT_EQ(3, myInt);
}

// Check that we can pass in arrays and have them be stored as a pointer.
//   - Array of values stores a pointer.
//   - Array of const values stores a pointer.
TEST_F(TBindTest, ArrayArgumentBinding)
{
    int array[4] = { 1, 2, 3, 4 };
    const int (*constArrayPtr)[4] = &array;

    TCallback<int(int)> arrayPolyGet = Bind(&ArrayGet, array);
    EXPECT_EQ(1, arrayPolyGet.Run(0));
    EXPECT_EQ(2, arrayPolyGet.Run(1));
    EXPECT_EQ(3, arrayPolyGet.Run(2));
    EXPECT_EQ(4, arrayPolyGet.Run(3));

    TCallback<int(void)> arrayGet = Bind(&ArrayGet, array, 1);
    EXPECT_EQ(2, arrayGet.Run());

    TCallback<int(void)> constArrayGet = Bind(&ArrayGet, *constArrayPtr, 1);
    EXPECT_EQ(2, constArrayGet.Run());

    array[1] = 7;
    EXPECT_EQ(7, arrayGet.Run());
    EXPECT_EQ(7, constArrayGet.Run());
}

// Verify THasRefAndUnrefMethods correctly introspects the class type for a pair of
// Ref() and UnRef().
//   - Class with Ref() and UnRef().
//   - Class without Ref() and UnRef().
//   - Derived class with Ref() and UnRef().
//   - Derived class without Ref() and UnRef().
//   - Derived class with Ref() and UnRef() and a private destructor.
TEST_F(TBindTest, HasRefAndUnrefMethods)
{
    EXPECT_IS_TRUE(NDetail::THasRefAndUnrefMethods<TObjectWithRC>::Value);
    EXPECT_IS_FALSE(NDetail::THasRefAndUnrefMethods<TObject>::Value);

    // StrictMock<T> is a derived class of T.
    // So, we use StrictMock<TObjectWithRC> and StrictMock<TObject> to test that
    // THasRefAndUnrefMethods works over inheritance.
    EXPECT_IS_TRUE(NDetail::THasRefAndUnrefMethods< StrictMock<TObjectWithRC> >::Value);
    EXPECT_IS_FALSE(NDetail::THasRefAndUnrefMethods< StrictMock<TObject> >::Value);

    // This matters because the implementation creates a dummy class that
    // inherits from the template type.
    EXPECT_IS_TRUE(NDetail::THasRefAndUnrefMethods<TObjectWithRCAndPrivateDtor>::Value);
}

// Unretained() wrapper support.
//   - Method bound to Unretained() non-const object.
//   - Const method bound to Unretained() non-const object.
//   - Const method bound to Unretained() const object.
TEST_F(TBindTest, UnretainedWrapper)
{
    EXPECT_CALL(Object, VoidMethod0()).Times(1);
    EXPECT_CALL(Object, VoidConstMethod0()).Times(2);

    EXPECT_CALL(ObjectWithRC, Ref()).Times(0);
    EXPECT_CALL(ObjectWithRC, UnRef()).Times(0);
    EXPECT_CALL(ObjectWithRC, VoidMethod0()).Times(1);
    EXPECT_CALL(ObjectWithRC, VoidConstMethod0()).Times(2);

    TCallback<void(void)> boundMethod =
        Bind(&TObject::VoidMethod0, Unretained(&Object));
    boundMethod.Run();

    TCallback<void(void)> constMethodNonConstObject =
        Bind(&TObject::VoidConstMethod0, Unretained(&Object));
    constMethodNonConstObject.Run();

    TCallback<void(void)> constMethodConstObject =
        Bind(&TObject::VoidConstMethod0, Unretained(ConstObjectPtr));
    constMethodConstObject.Run();

    TCallback<void(void)> boundMethodWithoutRC =
        Bind(&TObjectWithRC::VoidMethod0, Unretained(&ObjectWithRC)); // (NoRef)
    boundMethodWithoutRC.Run();

    TCallback<void(void)> constMethodNonConstObjectWithoutRC =
        Bind(&TObjectWithRC::VoidConstMethod0, Unretained(&ObjectWithRC)); // (NoRef)
    constMethodNonConstObjectWithoutRC.Run();

    TCallback<void(void)> constMethodConstObjectWithoutRC =
        Bind(&TObjectWithRC::VoidConstMethod0, Unretained(ConstObjectWithRCPtr)); // (NoRef)
    constMethodConstObjectWithoutRC.Run();
}

// Weak pointer support.
//   - Method bound to a weak pointer to a non-const object.
//   - Const method bound to a weak pointer to a non-const object.
//   - Const method bound to a weak pointer to a const object.
//   - Normal Function with WeakPtr<> as P1 can have return type and is
//     not canceled.
TEST_F(TBindTest, WeakPtr)
{
    TObjectWithExtrinsicRC::TPtr object = New<TObjectWithExtrinsicRC>();
    TObjectWithExtrinsicRC::TWkPtr objectWk(object);

    EXPECT_CALL(*object, VoidMethod0());
    EXPECT_CALL(*object, VoidConstMethod0()).Times(2);

    TClosure boundMethod =
        Bind(
            &TObjectWithExtrinsicRC::VoidMethod0,
            TObjectWithExtrinsicRC::TWkPtr(object));
    boundMethod.Run();

    TClosure constMethodNonConstObject =
        Bind(
            &TObject::VoidConstMethod0,
            TObjectWithExtrinsicRC::TWkPtr(object));
    constMethodNonConstObject.Run();

    TClosure constMethodConstObject =
        Bind(
            &TObject::VoidConstMethod0,
            TObjectWithExtrinsicRC::TConstWkPtr(object));
    constMethodConstObject.Run();

    TCallback<int(int)> normalFunc =
        Bind(
            &FunctionWithWeakParam<TObjectWithExtrinsicRC>,
            TObjectWithExtrinsicRC::TWkPtr(object));

    EXPECT_EQ(1, normalFunc.Run(1));

    object.Reset();
    ASSERT_TRUE(objectWk.IsExpired());

    boundMethod.Run();
    constMethodNonConstObject.Run();
    constMethodConstObject.Run();

    EXPECT_EQ(2, normalFunc.Run(2));
}

// ConstRef() wrapper support.
//   - Binding without ConstRef() takes a copy.
//   - Binding with a ConstRef() takes a reference.
//   - Binding ConstRef() to a function that accepts const reference does not copy on invoke.
TEST_F(TBindTest, ConstRefWrapper)
{
    int n = 1;

    TCallback<int(void)> withoutConstRef =
        Bind(&IntegerIdentity, n);
    TCallback<int(void)> withConstRef =
        Bind(&IntegerIdentity, ConstRef(n));

    EXPECT_EQ(1, withoutConstRef.Run());
    EXPECT_EQ(1, withConstRef.Run());
    n++;
    EXPECT_EQ(1, withoutConstRef.Run());
    EXPECT_EQ(2, withConstRef.Run());

    TProbeState state;
    TProbe probe(&state);

    TClosure everywhereConstRef =
        Bind(&Tackle, ConstRef(probe));
    everywhereConstRef.Run();

    EXPECT_THAT(probe, HasCopyMoveCounts(0, 0));
    EXPECT_THAT(probe, NoAssignments());
}

// Owned() wrapper support.
TEST_F(TBindTest, OwnedWrapper)
{
    TProbeState state;
    TProbe* probe;
    
    // If we don't capture, delete happens on TCallback destruction/reset.
    // return the same value.
    state.Reset();
    probe = new TProbe(&state);
    
    TCallback<TProbe*(void)> capturedArgument =
        Bind(&PolymorphicIdentity<TProbe*>, Owned(probe));

    ASSERT_EQ(probe, capturedArgument.Run());
    ASSERT_EQ(probe, capturedArgument.Run());
    EXPECT_EQ(0, state.Destructors);
    capturedArgument.Reset(); // This should trigger a delete.
    EXPECT_EQ(1, state.Destructors);

    state.Reset();
    probe = new TProbe(&state);
    TCallback<void(void)> capturedTarget =
        Bind(&TProbe::Tackle, Owned(probe));

    capturedTarget.Run();
    EXPECT_EQ(0, state.Destructors);
    capturedTarget.Reset();
    EXPECT_EQ(1, state.Destructors);
}

// Passed() wrapper support.
//   - Passed() can be constructed from a pointer to scoper.
//   - Passed() can be constructed from a scoper rvalue.
//   - Using Passed() gives TCallback ownership.
//   - Ownership is transferred from TCallback to callee on the first Run().
TEST_F(TBindTest, PassedWrapper)
{
    TProbeState state;

    // Tests the Passed() function's support for pointers.
    {
        TProbe probe(&state);

        TCallback<TProbe(void)> cb =
            Bind(
                &PolymorphicPassThrough<TProbe>,
                Passed(&probe));

        // The argument has been passed.
        EXPECT_IS_FALSE(probe.IsValid());
        EXPECT_EQ(0, state.Destructors);
        EXPECT_THAT(state, NoCopies());

        // If we never invoke the TCallback, it retains ownership and deletes.
        cb.Reset();

        EXPECT_EQ(1, state.Destructors);
    }

    state.Reset();

    // Tests the Passed() function's support for rvalues.
    {
        TProbe probe(&state);

        TCallback<TProbe(void)> cb =
            Bind(
                &PolymorphicPassThrough<TProbe>,
                Passed(MoveRV(probe)));
        
        // The argument has been passed.
        EXPECT_IS_FALSE(probe.IsValid());
        EXPECT_EQ(0, state.Destructors);
        EXPECT_THAT(state, NoCopies());

        {
            // Check that ownership can be transferred back out.
            int n = state.MoveConstructors;
            TProbe result = cb.Run();
            EXPECT_EQ(0, state.Destructors);
            EXPECT_EQ(n + 2, state.MoveConstructors);
            EXPECT_THAT(state, NoCopies());

            // Resetting does not delete since ownership was transferred.
            cb.Reset();
            EXPECT_EQ(0, state.Destructors);
            EXPECT_THAT(state, NoCopies());
        }

        // Ensure that we actually did get ownership (from the last scope).
        EXPECT_EQ(1, state.Destructors);
    }

    state.Reset();

    // Yet another test for movable semantics.
    {
        TProbe sender(&state);
        TProbe receiver(TProbe::ExplicitlyCreateInvalidProbe());

        TCallback<TProbe(TProbe)> cb =
            Bind(&PolymorphicPassThrough<TProbe>);

        EXPECT_IS_TRUE(sender.IsValid());
        EXPECT_IS_FALSE(receiver.IsValid());

        EXPECT_EQ(0, state.Destructors);
        EXPECT_THAT(state, NoCopies());

        receiver = cb.Run(MoveRV(sender));

        EXPECT_IS_FALSE(sender.IsValid());
        EXPECT_IS_TRUE(receiver.IsValid());

        EXPECT_EQ(0, state.Destructors);
        EXPECT_THAT(state, NoCopies());
    }
}

// Argument constructor usage for non-reference and const reference parameters.
TEST_F(TBindTest, ArgumentProbing)
{
    TProbeState state;
    TProbe probe(&state);

    TProbe& probeRef = probe;
    const TProbe& probeConstRef = probe;

    // {T, T&, const T&, T&&} -> T
    {
        // Bind T
        state.Reset();
        TClosure boundValue =
            Bind(&VoidPolymorphic1<TProbe>, probe);
        EXPECT_THAT(probe, AllOf(HasCopyMoveCounts(1, 0), NoAssignments()));
        boundValue.Run();
        EXPECT_THAT(probe, AllOf(HasCopyMoveCounts(2, 1), NoAssignments()));

        // Bind T&
        state.Reset();
        TClosure boundRef =
            Bind(&VoidPolymorphic1<TProbe>, probeRef);
        EXPECT_THAT(probe, AllOf(HasCopyMoveCounts(1, 0), NoAssignments()));
        boundRef.Run();
        EXPECT_THAT(probe, AllOf(HasCopyMoveCounts(2, 1), NoAssignments()));

        // Bind const T&
        state.Reset();
        TClosure boundConstRef =
            Bind(&VoidPolymorphic1<TProbe>, probeConstRef);
        EXPECT_THAT(probe, AllOf(HasCopyMoveCounts(1, 0), NoAssignments()));
        boundConstRef.Run();
        EXPECT_THAT(probe, AllOf(HasCopyMoveCounts(2, 1), NoAssignments()));

        // Bind T&&
        state.Reset();
        TClosure boundRvRef =
            Bind(&VoidPolymorphic1<TProbe>, static_cast<TProbe&&>(TProbe(&state)));
        EXPECT_THAT(probe, AllOf(HasCopyMoveCounts(0, 1), NoAssignments()));
        boundRvRef.Run();
        EXPECT_THAT(probe, AllOf(HasCopyMoveCounts(1, 2), NoAssignments()));

        // Pass all of above as a forwarded argument.
        // We expect almost perfect forwarding (copy + move)
        state.Reset();
        TCallback<void(TProbe)> forward = Bind(&VoidPolymorphic1<TProbe>);

        EXPECT_THAT(probe, HasCopyMoveCounts(0, 0));
        forward.Run(probe);
        EXPECT_THAT(probe, HasCopyMoveCounts(1, 1));
        forward.Run(probeRef);
        EXPECT_THAT(probe, HasCopyMoveCounts(2, 2));
        forward.Run(probeConstRef);
        EXPECT_THAT(probe, HasCopyMoveCounts(3, 3));
        forward.Run(static_cast<TProbe&&>(TProbe(&state)));
        EXPECT_THAT(probe, HasCopyMoveCounts(3, 4));

        EXPECT_THAT(probe, NoAssignments());
    }

    // {T, T&, const T&, T&&} -> const T&
    {
        // Bind T
        state.Reset();
        TClosure boundValue =
            Bind(&VoidPolymorphic1<const TProbe&>, probe);
        EXPECT_THAT(probe, AllOf(HasCopyMoveCounts(1, 0), NoAssignments()));
        boundValue.Run();
        EXPECT_THAT(probe, AllOf(HasCopyMoveCounts(1, 0), NoAssignments()));

        // Bind T&
        state.Reset();
        TClosure boundRef =
            Bind(&VoidPolymorphic1<const TProbe&>, probeRef);
        EXPECT_THAT(probe, AllOf(HasCopyMoveCounts(1, 0), NoAssignments()));
        boundRef.Run();
        EXPECT_THAT(probe, AllOf(HasCopyMoveCounts(1, 0), NoAssignments()));

        // Bind const T&
        state.Reset();
        TClosure boundConstRef =
            Bind(&VoidPolymorphic1<const TProbe&>, probeConstRef);
        EXPECT_THAT(probe, AllOf(HasCopyMoveCounts(1, 0), NoAssignments()));
        boundConstRef.Run();
        EXPECT_THAT(probe, AllOf(HasCopyMoveCounts(1, 0), NoAssignments()));

        // Bind T&&
        state.Reset();
        TClosure boundRvRef =
            Bind(&VoidPolymorphic1<const TProbe&>, static_cast<TProbe&&>(TProbe(&state)));
        EXPECT_THAT(probe, AllOf(HasCopyMoveCounts(0, 1), NoAssignments()));
        boundRvRef.Run();
        EXPECT_THAT(probe, AllOf(HasCopyMoveCounts(0, 1), NoAssignments()));

        // Pass all of above as a forwarded argument.
        // We expect perfect forwarding.
        state.Reset();
        TCallback<void(const TProbe&)> forward = Bind(&VoidPolymorphic1<const TProbe&>);

        EXPECT_THAT(probe, HasCopyMoveCounts(0, 0));
        forward.Run(probe);
        EXPECT_THAT(probe, HasCopyMoveCounts(0, 0));
        forward.Run(probeRef);
        EXPECT_THAT(probe, HasCopyMoveCounts(0, 0));
        forward.Run(probeConstRef);
        EXPECT_THAT(probe, HasCopyMoveCounts(0, 0));
        forward.Run(static_cast<TProbe&&>(TProbe(&state)));
        EXPECT_THAT(probe, HasCopyMoveCounts(0, 0));

        EXPECT_THAT(probe, NoAssignments());
    }
}

// Argument constructor usage for non-reference and const reference parameters.
TEST_F(TBindTest, CoercibleArgumentProbing)
{
    TProbeState state;
    TCoercibleToProbe probe(&state);

    TCoercibleToProbe& probeRef = probe;
    const TCoercibleToProbe& probeConstRef = probe;

    // Pass {T, T&, const T&, T&&} as a forwarded argument.
    // We expect almost perfect forwarding (copy + move).
    state.Reset();
    TCallback<void(TProbe)> forward = Bind(&VoidPolymorphic1<TProbe>);

    EXPECT_THAT(state, HasCopyMoveCounts(0, 0));
    forward.Run(probe);
    EXPECT_THAT(state, HasCopyMoveCounts(1, 1));
    forward.Run(probeRef);
    EXPECT_THAT(state, HasCopyMoveCounts(2, 2));
    forward.Run(probeConstRef);
    EXPECT_THAT(state, HasCopyMoveCounts(3, 3));
    forward.Run(static_cast<TProbe&&>(TProbe(&state)));
    EXPECT_THAT(state, HasCopyMoveCounts(3, 4));

    EXPECT_THAT(state, NoAssignments());
}

// TCallback construction and assignment tests.
//   - Construction from an InvokerStorageHolder should not cause ref/deref.
//   - Assignment from other callback should only cause one ref
//
// TODO(ajwong): Is there actually a way to test this?

////////////////////////////////////////////////////////////////////////////////

}  // namespace
}  // namespace NYT
