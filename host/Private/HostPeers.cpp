// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostPeers.h"

#include "AutoRTFM.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "GodotClasses.h"
#include "GodotClassNames.gen.h"
#include "HostInstances.h"
#include "HostRuntime.h"
#include "HostScript.h"
#include "HostScriptState.h"
#include "HostTypeModel.h"
#include "UObject/UObjectHash.h"
#include "VerseVM/Inline/VVMVerseClassInline.h"
#include "VerseVM/VVMClass.h"
#include "VerseVM/VVMPackage.h"
#include "VerseVM/VVMVerseClass.h"

using GodotVerse::BindingsVersePath;
using GodotVerse::FindBindingClass;
using GodotVerse::FindMirroredClass;
using GodotVerse::FHostPeer;
using GodotVerse::GodotVersePath;
using GodotVerse::NewHostObject;

namespace {

/// The Godot object a vh_object under construction should adopt instead of minting one.
///
/// Since R-NODE-3 every vh_object runs a block clause that asks the host for a peer, and the host
/// itself is much the commoner constructor: a script instance for a node Godot already made, a
/// mirror wrapper for a handle crossing in, the transient instance the export defaults are read
/// off. Every one of those already has its object -- or deliberately has none -- so an
/// unconditional mint would leak a Godot object per construction while a working scene looked
/// entirely normal (docs/phase-4b-design.md 4.3).
///
/// NewHostObject opens one around every host-side construction that is not a reading device. The
/// class is carried as well as the handle so that a *member* of the class being built -- `Helper :=
/// helper{}` in a script -- still mints its own: the record answers the construction it was opened
/// for and nothing else, whichever of the two the VM runs first.
///
/// thread_local because a class's declared defaults are read off an instance built by whichever
/// thread ran the analysis, which is not the game thread.
struct FAdoptRecord
{
    const UClass* Class = nullptr;
    int64 Handle = 0;
    bool bActive = false;
};
thread_local FAdoptRecord GAdopt;

/// Scoped, and restoring rather than clearing: NewObject of a script class runs member
/// initializers that can construct more objects, and one of those can be another script class.
class FAdoptPeerScope
{
public:
    AUTORTFM_DISABLE FAdoptPeerScope(const UClass* Class, int64 Handle)
        : Saved(GAdopt)
    {
        GAdopt = FAdoptRecord{Class, Handle, true};
    }
    AUTORTFM_DISABLE ~FAdoptPeerScope() { GAdopt = Saved; }

    FAdoptPeerScope(const FAdoptPeerScope&) = delete;
    FAdoptPeerScope& operator=(const FAdoptPeerScope&) = delete;

private:
    FAdoptRecord Saved;
};

/// Nothing constructed while this stands gets a Godot object, however deep.
///
/// One caller, and it is the case FAdoptPeerScope cannot answer: the throwaway instance the export
/// defaults are read off. That instance is a *reading device*, not something an author asked for,
/// and its members' initializers run in full -- so a class whose member is `var Held:node2d =
/// node2d{}` would mint a real node on every analysis, once per exporting class, per keystroke.
/// Nodes are deliberately not freed when Verse drops them (Godot's rule, docs/phase-4b-design.md
/// 4.4), so each of those would be a leak nothing reports.
///
/// The peers read back as handle 0, which is what a declared default meant before any of this and
/// what the export list already refuses for an object-typed member (VH_EXPORT_OBJECT_NOT_OPTIONAL).
thread_local int32 GSuppressMintDepth = 0;

class FSuppressMintScope
{
public:
    AUTORTFM_DISABLE FSuppressMintScope() { ++GSuppressMintDepth; }
    AUTORTFM_DISABLE ~FSuppressMintScope() { --GSuppressMintDepth; }

    FSuppressMintScope(const FSuppressMintScope&) = delete;
    FSuppressMintScope& operator=(const FSuppressMintScope&) = delete;
};

} // namespace

AUTORTFM_DISABLE UObject* GodotVerse::NewHostObject(UClass* Class, FHostPeer Peer)
{
    if (!Class)
    {
        return nullptr;
    }
    if (Peer.IsSuppressed())
    {
        FSuppressMintScope Reading;
        return NewObject<UObject>(GetTransientPackage(), Class);
    }
    FAdoptPeerScope Adopting(Class, Peer.GetHandle());
    return NewObject<UObject>(GetTransientPackage(), Class);
}

namespace {

/// A fresh Verse wrapper around a Godot handle, which is what a mirrored-class member holds.
///
/// Built the way Instantiate builds a script's own object, and buildable that way for the same
/// reason: a mirrored class is ordinary Verse over the one native `object`, so its instance *is* a
/// UObject and none of the VM's own object allocation comes into it.
/// UVerseClass::PostInitInstance has run the class's Verse constructor by the time NewObject
/// returns, which leaves only the field C++ owns to fill in.
AUTORTFM_DISABLE UObject* NewMirroredWrapper(UClass* NativeClass, int64 Handle)
{
    // The handle this wrapper is *for*: the block clause runs inside NewObject and writes it, and
    // the assignment below then writes the same value a second time.
    UObject* const Wrapper = NewHostObject(NativeClass, FHostPeer::Adopt(Handle));
    verse::vh_object* Shadow = Cast<verse::vh_object>(Wrapper);
    if (!Shadow)
    {
        return nullptr;
    }
    Shadow->Handle.Set(Handle, Shadow);
    return Wrapper;
}

/// Handle -> the Verse object that minted it (R-NODE-3). Two jobs at once: it is what
/// `BeginDestroy` consults to know that this peer is the host's to release -- every object crossing
/// *from* Godot is a vh_object too, and freeing one would take a node the scene owns -- and it is
/// what keeps `H` the same Verse object after a round trip through Godot, the way a scripted node's
/// own instance does. Neither keeps anything alive: the row exists to be asked about.
///
/// Two pointers to one object, answering different questions. The weak one answers "is it still
/// alive", which is what handing the object back to Godot needs. The raw one answers "is this the
/// object that made this row", which the weak one **cannot**: by the time BeginDestroy runs the
/// object is already unreachable and every weak pointer to it reads as null, so a release keyed on
/// the weak pointer matched nothing and released nothing -- a leak that looked exactly like the
/// mechanism not working. It is compared and never dereferenced.
struct FMintedPeer
{
    TWeakObjectPtr<UObject> Object;
    const UObject* Owner = nullptr;
};
TMap<int64, FMintedPeer> GMintedByHandle;

/// Handle -> the mirrored UClass an object of it crosses as, asked of Godot once per Godot object
/// rather than once per crossing.
///
/// Safe to keep for the life of the process because Godot does not reuse an instance id within a
/// run: a freed object leaves a row naming a class nothing will ask about again. A null answer is
/// cached too -- a class the mirror does not carry is not worth asking Godot about twice.
TMap<int64, UClass*> GHandleClassCache;

/// The Godot class a mirrored Verse class stands for, out of the generated table's other half.
///
/// The emitted classes only, so this is an inverse rather than a second lookup: the table above
/// folds every Godot class onto its nearest *emitted* ancestor, and several rows of it can share a
/// Verse name. Binary search, because that half is sorted by Verse name.
const char* GodotNameForMirroredClass(FUtf8StringView VerseName)
{
    int32 Low = 0;
    int32 High = UE_ARRAY_COUNT(verse_classes::mirrored_classes) - 1;
    while (Low <= High)
    {
        const int32 Mid = Low + ((High - Low) / 2);
        const FUtf8StringView Candidate(
            reinterpret_cast<const UTF8CHAR*>(verse_classes::mirrored_classes[Mid].verse_name));
        const int32 Order = Candidate.Compare(VerseName);
        if (Order == 0)
        {
            return verse_classes::mirrored_classes[Mid].godot_name;
        }
        if (Order < 0)
        {
            Low = Mid + 1;
        }
        else
        {
            High = Mid - 1;
        }
    }
    return nullptr;
}

/// The Godot class to mint a peer of for an object of this Verse class: the nearest ancestor that
/// is one of the mirror's own, which for a script's `class(ref_counted)` is `RefCounted`.
///
/// The walk tests the *package* rather than the name, because the mirror's names are ordinary
/// identifiers a project could in principle reuse, and a script class that happened to be called
/// `node2d` would otherwise be minted as a Node2D. A VClass knows which package declared it; a
/// UClass's name does not.
///
/// Cached per UClass, so the walk happens once per script class rather than once per `helper{}`.
/// Safe to keep: a UClass is never reused for another Verse class, and a hot-reload generation
/// publishes new ones.
TMap<const UClass*, const char*> GPeerClassCache;

} // namespace

AUTORTFM_DISABLE void GodotVerse::ForgetCachedClasses()
{
    GHandleClassCache.Empty();
    GPeerClassCache.Empty();
}

namespace {

AUTORTFM_DISABLE const char* GodotPeerClassFor(const UClass* Class)
{
    if (const char** Cached = GPeerClassCache.Find(Class))
    {
        return *Cached;
    }

    const char* Found = nullptr;
    // The bindings package comes first, and it has to: a binding's whole point is that Godot has a
    // class the mirror does not, so the mirrored ancestor this walk would otherwise reach is the
    // *wrong* class to mint -- a RigidBody2D where a RapierBody2D was meant, silently, while a
    // working scene looks entirely normal (docs/generated-bindings.md 6).
    for (const UClass* Cursor = Class; Cursor && !Found; Cursor = Cursor->GetSuperClass())
    {
        const UVerseClass* const VerseClass = Cast<UVerseClass>(Cursor);
        const Verse::VClass* const VClass = VerseClass ? VerseClass->Class.Get() : nullptr;
        if (!VClass
            || !VClass->GetPackage().GetRootPath().AsStringView().Equals(FUtf8StringView(BindingsVersePath)))
        {
            continue;
        }
        if (const FUtf8String* const MintName = GodotVerse::MintNameForBinding(VClass->GetBaseName().AsStringView()))
        {
            // Held by the map for as long as the roster does, and the map outlives the cache: both
            // are emptied together by SetBindings.
            Found = reinterpret_cast<const char*>(**MintName);
        }
    }

    for (const UClass* Cursor = Class; Cursor && !Found; Cursor = Cursor->GetSuperClass())
    {
        const UVerseClass* const VerseClass = Cast<UVerseClass>(Cursor);
        const Verse::VClass* const VClass = VerseClass ? VerseClass->Class.Get() : nullptr;
        if (!VClass
            || !VClass->GetPackage().GetRootPath().AsStringView().Equals(FUtf8StringView(GodotVersePath)))
        {
            continue;
        }
        Found = GodotNameForMirroredClass(VClass->GetBaseName().AsStringView());
    }

    GPeerClassCache.Add(Class, Found);
    return Found;
}

/// The mirrored Verse class name for a Godot class name, out of the generated table.
///
/// Every Godot class is in that table, not only the emitted ones: with --classes-file a subset is
/// generated, and each row then names the nearest ancestor that was. Binary search, because the
/// table is sorted by Godot name and this is asked once per Godot object.
const char* MirroredNameForGodotClass(FUtf8StringView GodotName)
{
    int32 Low = 0;
    int32 High = UE_ARRAY_COUNT(verse_classes::class_names) - 1;
    while (Low <= High)
    {
        const int32 Mid = Low + ((High - Low) / 2);
        const FUtf8StringView Candidate(reinterpret_cast<const UTF8CHAR*>(verse_classes::class_names[Mid].godot_name));
        const int32 Order = Candidate.Compare(GodotName);
        if (Order == 0)
        {
            return verse_classes::class_names[Mid].verse_name;
        }
        if (Order < 0)
        {
            Low = Mid + 1;
        }
        else
        {
            High = Mid - 1;
        }
    }
    return nullptr;
}

/// The class a live handle should cross as, cached per handle.
///
/// Four questions, in this order, and the order is the point: a binding is *more derived* than the
/// mirrored class the handle would otherwise become, and a script binding is more derived still.
///
///   1. Does a script on this object name a binding? `get_class()` answers a script's native base,
///      so this one can only be asked of GetScriptClassOf.
///   2. Does this object's ClassDB class name a binding? A third-party GDExtension's class.
///   3. Is it one of the mirror's own?
///   4. Nothing -- the caller's declared type, or a bare vh_object for a cast to decline.
AUTORTFM_DISABLE UClass* MirroredClassForHandle(int64 Handle)
{
    if (UClass** Cached = GHandleClassCache.Find(Handle))
    {
        return *Cached;
    }

    UClass* Found = nullptr;
    GodotVerse::FHostState& Host = GodotVerse::GetHost();

    if (GodotVerse::HasScriptClassBindings() && Host.Godot.GetScriptClassOf)
    {
        GodotVerse::FCallArena Arena;
        vh_value ScriptName{};
        if (Host.Godot.GetScriptClassOf(Host.Godot.Ctx, Handle, &Arena, &ScriptName) == VH_CALL_OK
            && ScriptName.Type == VH_TYPE_STRING)
        {
            const FUtf8String Named(GodotVerse::MakeView(ScriptName.String.Utf8, ScriptName.String.Len));
            if (const FUtf8String* const VerseName = GodotVerse::BindingForScriptClass(Named))
            {
                Found = FindBindingClass(*VerseName);
            }
        }
    }

    if (!Found && Host.Godot.GetClassOf)
    {
        GodotVerse::FCallArena Arena;
        vh_value ClassName{};
        if (Host.Godot.GetClassOf(Host.Godot.Ctx, Handle, &Arena, &ClassName) == VH_CALL_OK
            && ClassName.Type == VH_TYPE_STRING)
        {
            const FUtf8StringView GodotName = GodotVerse::MakeView(ClassName.String.Utf8, ClassName.String.Len);
            if (const FUtf8String* const VerseName = GodotVerse::BindingForGodotClass(GodotName))
            {
                Found = FindBindingClass(*VerseName);
            }
            if (!Found)
            {
                if (const char* VerseName = MirroredNameForGodotClass(GodotName))
                {
                    Found = FindMirroredClass(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(VerseName)));
                }
            }
        }
    }

    GHandleClassCache.Add(Handle, Found);
    return Found;
}

/// The Godot container a wrapper class's own `Ref` default mints -- an Array for `godot_array` and
/// every `typed_array(t)`, a Dictionary for `dictionary` and every `typed_dictionary(k, v)` -- or 0
/// for a wrapper whose default names nothing, which is `callable` and `signal_ref`.
///
/// Keyed on the package and the base name, the test GodotPeerClassFor makes, because a parametric
/// instantiation is a class of its own with nothing else in common with its siblings.
AUTORTFM_DISABLE int32 MintedContainerTag(const UClass* Class)
{
    for (const UClass* Cursor = Class; Cursor; Cursor = Cursor->GetSuperClass())
    {
        const UVerseClass* const VerseClass = Cast<UVerseClass>(Cursor);
        const Verse::VClass* const VClass = VerseClass ? VerseClass->Class.Get() : nullptr;
        if (!VClass
            || !VClass->GetPackage().GetRootPath().AsStringView().Equals(FUtf8StringView(GodotVersePath)))
        {
            continue;
        }
        const FUtf8StringView Name = VClass->GetBaseName().AsStringView();
        if (Name.Equals(UTF8TEXT("godot_array")) || Name.StartsWith(UTF8TEXT("typed_array")))
        {
            return VH_VARIANT_ARRAY;
        }
        if (Name.Equals(UTF8TEXT("dictionary")) || Name.StartsWith(UTF8TEXT("typed_dictionary")))
        {
            return VH_VARIANT_DICTIONARY;
        }
    }
    return 0;
}

/// Gives each container an object's member defaults hold a reference id of its own (B42).
///
/// NewObject does not evaluate a class's member defaults. It copies them off the class default
/// object, and a wrapper that default expression built is instanced into the new object as a
/// subobject of it: a fresh wrapper holding the *same* id. Worse, that id is not even the class
/// default's own -- a wrapper built while a class default is being constructed takes `Ref` off its
/// own class's default object rather than running `VhRefNewDefault` -- so every scripted node shared
/// one Godot Array with every other, and in a cooked game, whose defaults the cooker read with no
/// Godot to mint against, held the dead reference 0.
///
/// Decided per wrapper by what its default was:
///
///  - still its own class's default id: the member's default was a fresh container (`godot_array{}`,
///    `MakeNodeArray()`), so this object gets a fresh one, as evaluating it would have given it.
///  - any other id: the default named a container that already existed, which is shared by
///    design, so the copy keeps it and takes a claim of its own -- without which its collection
///    released a claim it never held.
///  - under FSuppressMintScope, 0, which is what `VhRefNewDefault` answers there, and which leaves
///    the reading device nothing to release. That instance is built off the game thread, where the
///    reference table cannot be asked for anything.
///
/// The minted or retained id is the wrapper's to release, by the godot_ref::BeginDestroy rule.
/// Nothing compensates on abort, and nothing needs to: the write is open, so the wrapper keeps the
/// id whatever the construction's transaction does and releases it when it is collected.
///
/// Must run before any block clause reads a member, which is why AdoptOrMintPeer's adopt branch
/// calls it: vh_object's own block is the first to run. A member *object's* block runs earlier
/// still, while the instance is being built, and sees the copied id for the length of that block.
AUTORTFM_DISABLE void ClaimDefaultContainers(UObject* Object, bool bSuppressed)
{
    GodotVerse::FHostState& Host = GodotVerse::GetHost();
    ForEachObjectWithOuter(
        Object,
        [&](UObject* Subobject) {
            verse::godot_ref* const Wrapper = Cast<verse::godot_ref>(Subobject);
            if (!Wrapper)
            {
                return;
            }
            if (bSuppressed)
            {
                Wrapper->Ref.Init(0, Wrapper);
                return;
            }

            const int64 Held = Wrapper->Ref.Get();
            const verse::godot_ref* const ClassDefault =
                Cast<verse::godot_ref>(Wrapper->GetClass()->GetDefaultObject(/*bCreateIfNeeded*/ false));
            const bool bIsClassDefault = !ClassDefault || Held == ClassDefault->Ref.Get();
            const int32 Tag = MintedContainerTag(Wrapper->GetClass());
            if (Tag != 0 && bIsClassDefault)
            {
                Wrapper->Ref.Init(Host.Godot.NewRef ? Host.Godot.NewRef(Host.Godot.Ctx, Tag) : 0, Wrapper);
            }
            else if (Held != 0)
            {
                Wrapper->Ref.Init(Host.Godot.RetainRef ? Host.Godot.RetainRef(Host.Godot.Ctx, Held) : 0, Wrapper);
            }
        },
        EGetObjectsFlags::IncludeNestedObjects);
}

} // namespace

AUTORTFM_DISABLE UClass* GodotVerse::MirroredClassFor(FUtf8StringView GodotClassName)
{
    const char* const VerseName = MirroredNameForGodotClass(GodotClassName);
    return VerseName
        ? FindMirroredClass(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(VerseName)))
        : nullptr;
}

/// The Verse object a Godot handle crosses as: the script's own instance where the node carries
/// one, and a fresh mirror wrapper of the handle's actual Godot class otherwise (R-SCN-6).
///
/// Never null. Fallback is what to build when Godot will not say what the handle is -- an object
/// it has already freed, or a Godot class outside this mirror. A caller with a declared type to
/// fall back on passes it; the generated cast path passes null and gets a bare `vh_object`, which
/// every downcast then declines. That is the shape the cast asked for: a failure is an answer,
/// where a raise from here would be a different question's error.
///
/// Mirror wrappers are deliberately *not* cached, so two crossings of one un-scripted handle are
/// two Verse objects. Equality is by handle, and a wrapper cache would need a rule for what
/// happens to one across a hot-reload generation, which nothing yet needs.
AUTORTFM_DISABLE UObject* GodotVerse::ObjectForHandle(int64 Handle, UClass* Fallback)
{
    if (UObject* const Bound = InstanceObjectForHandle(Handle))
    {
        return Bound;
    }

    // An object Verse minted crosses back as the very object that minted it, which is what makes
    // `H` the same value after a round trip through a Godot Array. The same rule the scripted-node
    // row above states, for the other half of R-SCN-6's identity question.
    if (FMintedPeer* Minted = GMintedByHandle.Find(Handle))
    {
        if (UObject* Live = Minted->Object.Get())
        {
            return Live;
        }
    }

    UClass* Resolved = MirroredClassForHandle(Handle);
    if (!Resolved)
    {
        Resolved = Fallback;
    }
    if (UObject* Wrapper = NewMirroredWrapper(Resolved, Handle))
    {
        return Wrapper;
    }

    // A bare vh_object, which every cast then declines. It has no peer and must not mint one:
    // this is the answer to "Godot would not say what that handle is", not a request for an object.
    return NewHostObject(verse::vh_object::StaticClass(), FHostPeer::Adopt(0));
}

AUTORTFM_DISABLE int64 GodotVerse::AdoptOrMintPeer(verse::vh_object* Self, const char*& OutRefusedClass)
{
    OutRefusedClass = nullptr;
    if (!Self)
    {
        return 0;
    }

    // A class default object is not a live object and has nothing to be the peer of. It is guarded
    // rather than assumed: UVerseClass::NeedsInit runs the init functions for one, and this bridge
    // creates a CDO for every mirrored class it ever names.
    if (Self->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
    {
        return 0;
    }

    // The host is the side doing the constructing, and the peer -- or the deliberate absence of one
    // -- is already decided. Consumed, so that a member of the class being built still mints its
    // own; the class test is what makes that true whichever order the VM runs the two in.
    if (GAdopt.bActive && GAdopt.Class == Self->GetClass())
    {
        GAdopt.bActive = false;
        ClaimDefaultContainers(Self, GSuppressMintDepth > 0);
        return GAdopt.Handle;
    }

    if (GSuppressMintDepth > 0)
    {
        ClaimDefaultContainers(Self, /*bSuppressed*/ true);
        return 0;
    }

    const char* const GodotClass = GodotPeerClassFor(Self->GetClass());
    if (!GodotClass)
    {
        // `vh_object` itself, which is the one class below the mirror and which nothing a script
        // writes should name. Not an error: ObjectForHandle builds one deliberately when Godot will
        // not say what a handle is, and that is a value for a cast to decline rather than a request
        // for an object.
        return 0;
    }

    FHostState& Host = GetHost();
    const FUtf8StringView ClassView(reinterpret_cast<const UTF8CHAR*>(GodotClass));
    const int64 Handle = Host.Godot.InstantiateClass
        ? Host.Godot.InstantiateClass(Host.Godot.Ctx, GodotClass, ClassView.Len())
        : 0;
    if (Handle == 0)
    {
        OutRefusedClass = GodotClass;
        return 0;
    }

    GMintedByHandle.Add(Handle, FMintedPeer{TWeakObjectPtr<UObject>(Self), Self});

    // Compensated rather than deferred, exactly as SubscribeSignal is and for the same reason: this
    // mutates Godot and answers a value. `SameAsClosed` is the load-bearing half -- the mint above
    // ran inside an AutoRTFM::Open, and a plain OnAbort from open code is ignored.
    //
    // bDiscard, because an aborted transaction's object is one nothing outside it can ever have
    // seen: even the Object-derived peer this bridge otherwise leaks on purpose (GDScript's own
    // rule) is freed here, where there is nobody left to free it by hand.
    AutoRTFM::OnAbort<AutoRTFM::EOpenBehavior::SameAsClosed>(
        [Handle] { GodotVerse::ReleaseMintedPeer(nullptr, Handle, /*bDiscard*/ true); });
    return Handle;
}

AUTORTFM_DISABLE FUtf8String GodotVerse::ClassBaseType(FUtf8StringView ClassName)
{
    // Resolving the class is a VM entry, and a running analysis has the VM blocked. The consumer
    // only asks where the source text is gone, which is a runtime host, which never analyses --
    // but the rule is the file's and not the caller's, so it is checked here.
    if (IsBackgroundCheckRunning())
    {
        return FUtf8String();
    }
    const char* const GodotClass = GodotPeerClassFor(FindGodotClass(ClassName));
    return GodotClass ? FUtf8String(GodotClass) : FUtf8String();
}

AUTORTFM_DISABLE bool GodotVerse::IsMintSuppressed()
{
    return GSuppressMintDepth > 0;
}

AUTORTFM_DISABLE void GodotVerse::ReleaseMintedPeer(const UObject* Owner, int64 Handle, bool bDiscard)
{
    if (Handle == 0)
    {
        return;
    }
    const FMintedPeer* const Minted = GMintedByHandle.Find(Handle);
    if (!Minted)
    {
        // Not ours. Every object crossing *from* Godot is a vh_object too, and this is the path its
        // collection takes -- releasing here would free a node the scene owns.
        return;
    }
    // Owner is null from the abort compensation, which has no object left to name: the transaction
    // that built it is being undone. From BeginDestroy it is the object being collected, and the
    // row has to be the one it made.
    if (Owner && Minted->Owner != Owner)
    {
        return;
    }
    GMintedByHandle.Remove(Handle);

    FHostState& Host = GetHost();
    if (Host.Godot.ReleaseObject)
    {
        Host.Godot.ReleaseObject(Host.Godot.Ctx, Handle, bDiscard ? 1 : 0);
    }
}

