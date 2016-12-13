// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Vehicles/VehicleAnimInstance.h"
#include "AnimNode_SkeletalControlBase.h"
#include "AnimNode_WheelHandler.generated.h"

/**
 *	Simple controller that replaces or adds to the translation/rotation of a single bone.
 */
USTRUCT()
struct ANIMGRAPHRUNTIME_API FAnimNode_WheelHandler : public FAnimNode_SkeletalControlBase
{
	GENERATED_USTRUCT_BODY()

	FAnimNode_WheelHandler();

	// FAnimNode_Base interface
	virtual void GatherDebugData(FNodeDebugData& DebugData) override;
	// End of FAnimNode_Base interface

	// FAnimNode_SkeletalControlBase interface
	virtual void EvaluateBoneTransforms(USkeletalMeshComponent* SkelComp, FCSPose<FCompactPose>& MeshBases, TArray<FBoneTransform>& OutBoneTransforms) override;
	virtual bool IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones) override;
	virtual void Initialize(const FAnimationInitializeContext& Context) override;
	// End of FAnimNode_SkeletalControlBase interface

private:
	// FAnimNode_SkeletalControlBase interface
	virtual void InitializeBoneReferences(const FBoneContainer& RequiredBones) override;
	// End of FAnimNode_SkeletalControlBase interface

	struct FWheelLookupData
	{
		int32 WheelIndex;
		FBoneReference BoneReference;
	};

	TArray<FWheelLookupData> Wheels;
	const FVehicleAnimInstanceProxy* AnimInstanceProxy;	//TODO: we only cache this to use in eval where it's safe. Should change API to pass proxy into eval
};