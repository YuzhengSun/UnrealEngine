// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ScaleBox.generated.h"

/**
 * Allows you to place content with a desired size and have it scale to meet the constraints placed on this box's alloted area.  If
 * you needed to have a background image scale to fill an area but not become distorted with different aspect ratios, or if you need
 * to auto fit some text to an area, this is the control for you.
 */
UCLASS(ClassGroup=UserInterface)
class UMG_API UScaleBox : public UContentWidget
{
	GENERATED_UCLASS_BODY()

public:

	/** Controls in what direction content can be scaled */
	UPROPERTY(EditDefaultsOnly, Category="Stretching")
	TEnumAsByte<EStretchDirection::Type> StretchDirection;

	/** The stretching rule to apply when content is stretched */
	UPROPERTY(EditDefaultsOnly, Category="Stretching")
	TEnumAsByte<EStretch::Type> Stretch;

public:

	// UWidget interface
	virtual void SynchronizeProperties() override;
	// End of UWidget interface

	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

#if WITH_EDITOR
	virtual const FSlateBrush* GetEditorIcon() override;
	virtual const FText GetPaletteCategory() override;
#endif

protected:

	// UPanelWidget
	virtual UClass* GetSlotClass() const override;
	virtual void OnSlotAdded(UPanelSlot* Slot) override;
	virtual void OnSlotRemoved(UPanelSlot* Slot) override;
	// End UPanelWidget

protected:
	TSharedPtr<SScaleBox> MyScaleBox;

protected:
	// UWidget interface
	virtual TSharedRef<SWidget> RebuildWidget() override;
	// End of UWidget interface
};
