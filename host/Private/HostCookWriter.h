// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "verse_host_abi.h"

#if VH_HOST_KIND == VH_HOST_KIND_COOKER

#include "Serialization/FilePackageWriterUtil.h"
#include "Serialization/PackageWriterToSharedBuffer.h"

/// The one thing a cooking UPackage::Save cannot be given a substitute for.
///
/// SavePackage2.cpp:3616-3619 does `SaveContext.GetPackageWriter()->AsCookedPackageWriter()` and
/// `check`s the result -- "Cooking requires a CookedPackageWriter" -- so an IPackageWriter is not
/// enough however well it writes files. UnrealEd's public FDefaultCookedFilePackageWriter is named
/// as though it were one and is a plain TPackageWriterToSharedBuffer<FBasePackageWriter>: the
/// first cook asserted on that line, three lines into its first package.
///
/// So this is that class again over FBaseCookedPackageWriter, plus the thirteen pure virtuals
/// ICookedPackageWriter adds. All thirteen are about a cook *session* -- an oplog, incremental
/// invalidation, multiprocess messaging, hashes -- and this cooker has none: it saves the packages
/// one process compiled, once, and exits. They are stubbed rather than implemented, and each stub
/// is the honest answer for a session that does not exist rather than a placeholder.
class FVerseCookedPackageWriter final : public TPackageWriterToSharedBuffer<FBaseCookedPackageWriter>
{
public:
	using Super = TPackageWriterToSharedBuffer<FBaseCookedPackageWriter>;

	/// The file to write. Its extension is replaced per output, the way a cook splits a package
	/// into `.uasset` and `.uexp`.
	explicit FVerseCookedPackageWriter(const FString& InBaseFilename)
		: BaseFilename(InBaseFilename)
	{
	}

	// -- IPackageWriter, the half that decides where the bytes land ------------------------------

	virtual void WritePackageData(const FPackageInfo& Info, FLargeMemoryWriter& ExportsArchive,
		const TArray<FFileRegion>& FileRegions) override
	{
		FPackageInfo Redirected = Info;
		Redirected.LooseFilePath = FPaths::SetExtension(BaseFilename, FPaths::GetExtension(Info.LooseFilePath, false));
		Super::WritePackageData(Redirected, ExportsArchive, FileRegions);
	}

	virtual void WriteBulkData(const FBulkDataInfo& Info, const FIoBuffer& BulkData,
		const TArray<FFileRegion>& FileRegions) override
	{
		FBulkDataInfo Redirected = Info;
		Redirected.LooseFilePath = FPaths::SetExtension(BaseFilename, FPaths::GetExtension(Info.LooseFilePath, false));
		Super::WriteBulkData(Redirected, BulkData, FileRegions);
	}

	// -- ICookedPackageWriter, the half a cook session would fill in -----------------------------

	virtual void SetCooker(UE::PackageWriter::Private::ICookerInterface* InCooker) override { Cooker = InCooker; }
	virtual void Initialize(const FCookInfo&) override {}
	virtual void BeginCook(const FCookInfo&) override {}
	virtual void EndCook(const FCookInfo&) override {}

	/// There is no previous cook to populate from: this writes into a directory the export plugin
	/// made and removes again.
	virtual void PopulateOplog(const FAssetRegistryState&, int32& OutNumPackagesInOplog) override
	{
		OutNumPackagesInOplog = 0;
	}
	virtual void UpdateLastReferenceDateAndPruneStaleOps(UE::Cook::Artifact::FUpdateOplogPackagesContext&) override {}
	virtual TArray<FName> GetOplogPackageNames() override { return TArray<FName>(); }
	virtual FCbObject GetOplogAttachment(FName, FUtf8StringView) override { return FCbObject(); }
	virtual void GetOplogAttachments(TArrayView<FName>, TArrayView<FUtf8StringView>,
		TUniqueFunction<void(FName, FUtf8StringView, FCbObject&&)>&&) override {}
	virtual void GetBaseGameOplogAttachments(TArrayView<FName>, TArrayView<FUtf8StringView>,
		TUniqueFunction<void(FName, FUtf8StringView, FCbObject&&)>&&) override {}

	/// Nothing was committed before this process started, which is what an empty oplog means.
	virtual ECommitStatus GetCommitStatus(FName) override { return ECommitStatus::NotCommitted; }
	virtual void RemoveCookedPackages(TArrayView<const FName>) override {}
	virtual void RemoveCookedPackages() override {}

	/// Multiprocess cooking: one process, no messages.
	virtual TFuture<FCbObject> WriteMPCookMessageForPackage(FName) override
	{
		TPromise<FCbObject> Promise;
		Promise.SetValue(FCbObject());
		return Promise.GetFuture();
	}
	virtual bool TryReadMPCookMessageForPackage(FName, FCbObjectView) override { return false; }

	virtual TMap<FName, TRefCountPtr<FPackageHashes>>& GetPackageHashes() override { return PackageHashes; }

	/// Not pure, but its default body is `unimplemented()` -- a cook is expected to route this to
	/// the cook server, and the second cook asserted here. What the routing amounts to is asking
	/// each object to prepare its platform data, so this asks them directly.
	///
	/// Every object in a Verse package is a UVerseClass or one of its members and none of them has
	/// platform data to cache, so this is a formality. It is written as the loop rather than as a
	/// bare Success because a future package with a texture in it should not silently ship one
	/// that was never cached.
	virtual EPackageWriterResult BeginCacheForCookedPlatformData(FBeginCacheForCookedPlatformDataInfo& Info) override
	{
		for (UObject* Object : Info.SaveableObjects)
		{
			if (Object && !Object->IsCachedCookedPlatformDataLoaded(Info.TargetPlatform))
			{
				Object->BeginCacheForCookedPlatformData(Info.TargetPlatform);
			}
		}
		return EPackageWriterResult::Success;
	}

protected:
	virtual void CommitPackageInternal(FPackageWriterRecords::FPackage&& BaseRecord,
		const FCommitPackageInfo& Info) override
	{
		FFilePackageWriterUtil::FRecord& Record = static_cast<FFilePackageWriterUtil::FRecord&>(BaseRecord);
		FFilePackageWriterUtil::FWritePackageParameters Parameters(Record, Info);
		Parameters.Cooker = Cooker;
		Parameters.AllPackageHashes = &PackageHashes;
		Parameters.PackageHashesLock = &PackageHashesLock;
		FFilePackageWriterUtil::WritePackage(Parameters);
	}

	/// FFilePackageWriterUtil::WritePackage static_casts the record to its own subclass, so this
	/// has to be the one it expects.
	virtual FPackageWriterRecords::FPackage* ConstructRecord() override
	{
		return new FFilePackageWriterUtil::FRecord();
	}

private:
	FString BaseFilename;
	UE::PackageWriter::Private::ICookerInterface* Cooker = nullptr;
	TMap<FName, TRefCountPtr<FPackageHashes>> PackageHashes;
	FCriticalSection PackageHashesLock;
};

#endif // VH_HOST_KIND == VH_HOST_KIND_COOKER
