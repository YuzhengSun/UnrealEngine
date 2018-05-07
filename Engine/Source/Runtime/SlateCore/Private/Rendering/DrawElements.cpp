// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.

#include "Rendering/DrawElements.h"
#include "Application/SlateApplicationBase.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"
#include "HAL/IConsoleManager.h"
#include "Types/ReflectionMetadata.h"

DECLARE_CYCLE_STAT(TEXT("FSlateDrawElement::Make Time"), STAT_SlateDrawElementMakeTime, STATGROUP_SlateVerbose);
DECLARE_CYCLE_STAT(TEXT("FSlateDrawElement::MakeCustomVerts Time"), STAT_SlateDrawElementMakeCustomVertsTime, STATGROUP_Slate);

DEFINE_STAT(STAT_SlateBufferPoolMemory);

FSlateShaderResourceManager* FSlateDataPayload::ResourceManager;


FSlateWindowElementList::FSlateWindowElementList(TSharedPtr<SWindow> InPaintWindow)
	: PaintWindow(InPaintWindow)
	, RenderTargetWindow(nullptr)
	, bNeedsDeferredResolve(false)
	, ResolveToDeferredIndex()
	, MemManager(0)
	, WindowSize(FVector2D(0.0f, 0.0f))
	, bReportReferences(true)
{
	DrawStack.Push(&RootDrawLayer);
	if (InPaintWindow.IsValid())
	{
		WindowSize = InPaintWindow->GetSizeInScreen();
	}

	// Only keep UObject resources alive if this window element list is born on the game thread.
	if (IsInGameThread())
	{
		ResourceGCRoot = MakeShared<FWindowElementGCObject>(this);
	}
}

void FSlateWindowElementList::AppendItems(FSlateWindowElementList* Other)
{
	FSlateDrawLayer* Layer = DrawStack.Last();

	TArray<FSlateDrawElement>& ActiveDrawElements = Layer->DrawElements;
	ActiveDrawElements.Append(Other->GetDrawElements());

	FSlateDrawLayer& OtherLayer = Other->GetRootDrawLayer();

	Layer->BoxElements.Append(OtherLayer.BoxElements);
	Layer->BorderElements.Append(OtherLayer.BorderElements);
	Layer->TextElements.Append(OtherLayer.TextElements);
	Layer->ShapedTextElements.Append(OtherLayer.ShapedTextElements);
	Layer->LineElements.Append(OtherLayer.LineElements);
}

void FSlateDrawBase::Setup(const FSlateWindowElementList& ElementList, int16 InLayer, const FPaintGeometry& PaintGeometry, ESlateDrawEffect InDrawEffects)
{
	RenderTransform = PaintGeometry.GetAccumulatedRenderTransform();
	Position = PaintGeometry.DrawPosition;
	Scale = PaintGeometry.DrawScale;
	LocalSize = PaintGeometry.GetLocalSize();
	ClippingIndex = ElementList.GetClippingIndex();
	Layer = InLayer;
	DrawEffects = InDrawEffects;

	// Calculate the layout to render transform as this is needed by several calculations downstream.
	const FSlateLayoutTransform InverseLayoutTransform(Inverse(FSlateLayoutTransform(Scale, Position)));
	LayoutToRenderTransform = Concatenate(InverseLayoutTransform, RenderTransform);

	// This is a workaround because we want to keep track of the various Scenes 
	// in use throughout the UI. We keep a synchronized set with the render thread on the SlateRenderer and 
	// use indices to synchronize between them.
	FSlateRenderer* Renderer = FSlateApplicationBase::Get().GetRenderer();
	checkSlow(Renderer);
	SceneIndex = Renderer->GetCurrentSceneIndex();

	BatchFlags = ESlateBatchDrawFlag::None;
	BatchFlags |= static_cast<ESlateBatchDrawFlag>(static_cast<uint32>(InDrawEffects) & static_cast<uint32>(ESlateDrawEffect::NoBlending | ESlateDrawEffect::PreMultipliedAlpha | ESlateDrawEffect::NoGamma | ESlateDrawEffect::InvertAlpha));

	static_assert(((__underlying_type(ESlateDrawEffect))ESlateDrawEffect::NoBlending) == ((__underlying_type(ESlateBatchDrawFlag))ESlateBatchDrawFlag::NoBlending), "Must keep ESlateBatchDrawFlag and ESlateDrawEffect partial matches");
	static_assert(((__underlying_type(ESlateDrawEffect))ESlateDrawEffect::PreMultipliedAlpha) == ((__underlying_type(ESlateBatchDrawFlag))ESlateBatchDrawFlag::PreMultipliedAlpha), "Must keep ESlateBatchDrawFlag and ESlateDrawEffect partial matches");
	static_assert(((__underlying_type(ESlateDrawEffect))ESlateDrawEffect::NoGamma) == ((__underlying_type(ESlateBatchDrawFlag))ESlateBatchDrawFlag::NoGamma), "Must keep ESlateBatchDrawFlag and ESlateDrawEffect partial matches");
	static_assert(((__underlying_type(ESlateDrawEffect))ESlateDrawEffect::InvertAlpha) == ((__underlying_type(ESlateBatchDrawFlag))ESlateBatchDrawFlag::InvertAlpha), "Must keep ESlateBatchDrawFlag and ESlateDrawEffect partial matches");
	if ((InDrawEffects & ESlateDrawEffect::ReverseGamma) != ESlateDrawEffect::None)
	{
		BatchFlags |= ESlateBatchDrawFlag::ReverseGamma;
	}
}

void FSlateDrawBase::ApplyPositionOffset(const FVector2D& InOffset)
{
	SetPosition(GetPosition() + InOffset);
	RenderTransform = Concatenate(RenderTransform, InOffset);

	// Recompute cached layout to render transform
	const FSlateLayoutTransform InverseLayoutTransform(Inverse(FSlateLayoutTransform(Scale, Position)));
	LayoutToRenderTransform = Concatenate(InverseLayoutTransform, RenderTransform);
}

void FSlateDrawText::SetText(FSlateWindowElementList& ElementList, const FString& InText, const FSlateFontInfo& InFontInfo, const int32 InStartIndex, const int32 InEndIndex)
{
	FontInfo = InFontInfo;
	const int32 StartIndex = FMath::Min<int32>(InStartIndex, InText.Len());
	const int32 EndIndex = FMath::Min<int32>(InEndIndex, InText.Len());
	TextLength = (EndIndex > StartIndex) ? EndIndex - StartIndex : 0;
	// Allocate memory and account for null terminator
	ImmutableText = (TCHAR*)ElementList.Alloc(sizeof(TCHAR) * (TextLength + 1), alignof(TCHAR));
	if (TextLength > 0)
	{
		FCString::Strncpy(ImmutableText, InText.GetCharArray().GetData() + StartIndex, TextLength + 1);
		check(!ImmutableText[TextLength]);
	}
	else
	{
		ImmutableText[0] = 0;
	}
}

void FSlateDrawLines::SetLines(FSlateWindowElementList& ElementList, const TArray<FVector2D>& InPoints, bool bInAntialias, const TArray<FLinearColor>* InPointColors)
{
	bAntialias = bInAntialias;

	if (bInAntialias)
	{
		// If the line is to be anti-aliased, we cannot reliably snap
		// the generated vertexes.
		DrawEffects |= ESlateDrawEffect::NoPixelSnapping;
	}

	NumPoints = InPoints.Num();
	if (NumPoints > 0)
	{
		Points = (FVector2D*)ElementList.Alloc(sizeof(FVector2D) * NumPoints, alignof(FVector2D));
		FMemory::Memcpy(Points, InPoints.GetData(), sizeof(FVector2D) * NumPoints);

		if (InPointColors && ensure(InPointColors->Num() == NumPoints))
		{
			PointColors = (FLinearColor*)ElementList.Alloc(sizeof(FLinearColor) * NumPoints, alignof(FLinearColor));
			FMemory::Memcpy(PointColors, InPointColors->GetData(), sizeof(FLinearColor) * NumPoints);
		}
		else
		{
			PointColors = nullptr;
		}
	}
	else
	{
		Points = nullptr;
	}
}

void FSlateDrawElement::Init(FSlateWindowElementList& ElementList, uint32 InLayer, const FPaintGeometry& PaintGeometry, ESlateDrawEffect InDrawEffects)
{
	RenderTransform = PaintGeometry.GetAccumulatedRenderTransform();
	Position = PaintGeometry.DrawPosition;
	Scale = PaintGeometry.DrawScale;
	LocalSize = PaintGeometry.GetLocalSize();
	ClippingIndex = ElementList.GetClippingIndex();
	Layer = InLayer;
	DrawEffects = InDrawEffects;

	// Calculate the layout to render transform as this is needed by several calculations downstream.
	const FSlateLayoutTransform InverseLayoutTransform(Inverse(FSlateLayoutTransform(Scale, Position)));
	LayoutToRenderTransform = Concatenate(InverseLayoutTransform, RenderTransform);

	// This is a workaround because we want to keep track of the various Scenes 
	// in use throughout the UI. We keep a synchronized set with the render thread on the SlateRenderer and 
	// use indices to synchronize between them.
	FSlateRenderer* Renderer = FSlateApplicationBase::Get().GetRenderer();
	checkSlow(Renderer);
	SceneIndex = Renderer->GetCurrentSceneIndex();

	DataPayload.BatchFlags = ESlateBatchDrawFlag::None;
	DataPayload.BatchFlags |= static_cast<ESlateBatchDrawFlag>( static_cast<uint32>( InDrawEffects ) & static_cast<uint32>( ESlateDrawEffect::NoBlending | ESlateDrawEffect::PreMultipliedAlpha | ESlateDrawEffect::NoGamma | ESlateDrawEffect::InvertAlpha ) );

	static_assert( ( ( __underlying_type(ESlateDrawEffect) )ESlateDrawEffect::NoBlending ) == ( ( __underlying_type(ESlateBatchDrawFlag) )ESlateBatchDrawFlag::NoBlending ), "Must keep ESlateBatchDrawFlag and ESlateDrawEffect partial matches" );
	static_assert( ( ( __underlying_type(ESlateDrawEffect) )ESlateDrawEffect::PreMultipliedAlpha ) == ( ( __underlying_type(ESlateBatchDrawFlag) )ESlateBatchDrawFlag::PreMultipliedAlpha ), "Must keep ESlateBatchDrawFlag and ESlateDrawEffect partial matches" );
	static_assert( ( ( __underlying_type(ESlateDrawEffect) )ESlateDrawEffect::NoGamma ) == ( ( __underlying_type(ESlateBatchDrawFlag) )ESlateBatchDrawFlag::NoGamma ), "Must keep ESlateBatchDrawFlag and ESlateDrawEffect partial matches" );
	static_assert( ( ( __underlying_type(ESlateDrawEffect) )ESlateDrawEffect::InvertAlpha ) == ( ( __underlying_type(ESlateBatchDrawFlag) )ESlateBatchDrawFlag::InvertAlpha ), "Must keep ESlateBatchDrawFlag and ESlateDrawEffect partial matches" );
	if ((InDrawEffects & ESlateDrawEffect::ReverseGamma) != ESlateDrawEffect::None)
	{
		DataPayload.BatchFlags |= ESlateBatchDrawFlag::ReverseGamma;
	}
}

void FSlateDrawElement::ApplyPositionOffset(FSlateDrawElement& Element, const FVector2D& InOffset)
{
	Element.SetPosition(Element.GetPosition() + InOffset);
	Element.RenderTransform = Concatenate(Element.RenderTransform, InOffset);

	// Recompute cached layout to render transform
	const FSlateLayoutTransform InverseLayoutTransform(Inverse(FSlateLayoutTransform(Element.Scale, Element.Position)));
	Element.LayoutToRenderTransform = Concatenate(InverseLayoutTransform, Element.RenderTransform);
}

#if SLATE_EXCESSIVE_CULLING

bool FSlateDrawElement::ShouldCull(const FSlateWindowElementList& ElementList)
{
	const FSlateClippingManager& ClippingManager = ElementList.GetClippingManager();
	const int32 CurrentIndex = ClippingManager.GetClippingIndex();
	if (CurrentIndex != INDEX_NONE)
	{
		const FSlateClippingState& ClippingState = ClippingManager.GetClippingStates()[CurrentIndex];
		return ClippingState.HasZeroArea();
	}

	return false;
}

#endif

void FSlateDrawElement::MakeDebugQuad( FSlateWindowElementList& ElementList, uint32 InLayer, const FPaintGeometry& PaintGeometry)
{
	PaintGeometry.CommitTransformsIfUsingLegacyConstructor();

	if (ShouldCull(ElementList))
	{
		return;
	}

	FSlateDrawElement& DrawElt = ElementList.AddUninitialized();
	DrawElt.Init(ElementList, InLayer, PaintGeometry, ESlateDrawEffect::None);
	DrawElt.ElementType = ET_DebugQuad;
}

void FSlateDrawElement::MakeBox( 
	FSlateWindowElementList& ElementList,
	uint32 InLayer, 
	const FPaintGeometry& PaintGeometry, 
	const FSlateBrush* InBrush,
	ESlateDrawEffect InDrawEffects, 
	const FLinearColor& InTint )
{
	PaintGeometry.CommitTransformsIfUsingLegacyConstructor();

	if (ShouldCull(ElementList, PaintGeometry, InBrush, InTint))
	{
		return;
	}
	
	if (InBrush->DrawAs == ESlateBrushDrawType::Border)
	{
		FSlateDrawBox& Box = ElementList.AddBorder();
		Box.Setup(ElementList, InLayer, PaintGeometry, InDrawEffects);
		Box.SetTint(InTint);
		Box.SetBrush(InBrush, FSlateDataPayload::ResourceManager);
	}
	else
	{
		FSlateDrawBox& Box = ElementList.AddBox();
		Box.Setup(ElementList, InLayer, PaintGeometry, InDrawEffects);
		Box.SetTint(InTint);
		Box.SetBrush(InBrush, FSlateDataPayload::ResourceManager);
	}
}

void FSlateDrawElement::MakeBox( 
	FSlateWindowElementList& ElementList,
	uint32 InLayer, 
	const FPaintGeometry& PaintGeometry, 
	const FSlateBrush* InBrush, 
	const FSlateResourceHandle& InRenderingHandle,
	ESlateDrawEffect InDrawEffects, 
	const FLinearColor& InTint )
{
	PaintGeometry.CommitTransformsIfUsingLegacyConstructor();

	// Ignore invalid rendering handles.
	if ( !InRenderingHandle.IsValid() )
	{
		return;
	}

	if (ShouldCull(ElementList, PaintGeometry, InBrush, InTint))
	{
		return;
	}

	FSlateShaderResourceProxy* RenderingProxy = InRenderingHandle.Data->Proxy;

	if (InBrush->DrawAs == ESlateBrushDrawType::Border)
	{
		FSlateDrawBox& Box = ElementList.AddBorder();
		Box.Setup(ElementList, InLayer, PaintGeometry, InDrawEffects);
		Box.SetTint(InTint);
		Box.SetBrush(InBrush, RenderingProxy);
	}
	else
	{
		FSlateDrawBox& Box = ElementList.AddBox();
		Box.Setup(ElementList, InLayer, PaintGeometry, InDrawEffects);
		Box.SetTint(InTint);
		Box.SetBrush(InBrush, RenderingProxy);
	}
}

void FSlateDrawElement::MakeRotatedBox( 
	FSlateWindowElementList& ElementList,
	uint32 InLayer, 
	const FPaintGeometry& PaintGeometry,
	const FSlateBrush* InBrush,
	ESlateDrawEffect InDrawEffects, 
	float Angle2D,
	TOptional<FVector2D> InRotationPoint,
	ERotationSpace RotationSpace,
	const FLinearColor& InTint )
{
	PaintGeometry.CommitTransformsIfUsingLegacyConstructor();

	FPaintGeometry RotatedPaintGeometry = PaintGeometry;
	
	if (Angle2D != 0.0f)
	{
		FVector2D RotationPoint = GetRotationPoint(PaintGeometry, InRotationPoint, RotationSpace);
		const FSlateRenderTransform RotationTransform = Concatenate(Inverse(RotationPoint), FQuat2D(Angle2D), RotationPoint);
		RotatedPaintGeometry.SetRenderTransform(Concatenate(RotationTransform, RotatedPaintGeometry.GetAccumulatedRenderTransform()));
	}

	MakeBox(
		ElementList,
		InLayer,
		RotatedPaintGeometry,
		InBrush,
		InDrawEffects,
		InTint);
}


void FSlateDrawElement::MakeText( FSlateWindowElementList& ElementList, uint32 InLayer, const FPaintGeometry& PaintGeometry, const FString& InText, const int32 StartIndex, const int32 EndIndex, const FSlateFontInfo& InFontInfo, ESlateDrawEffect InDrawEffects, const FLinearColor& InTint )
{
	SCOPE_CYCLE_COUNTER(STAT_SlateDrawElementMakeTime)
	PaintGeometry.CommitTransformsIfUsingLegacyConstructor();

	if (ShouldCull(ElementList, PaintGeometry, InTint))
	{
		return;
	}

	FSlateDrawText& Element = ElementList.AddText();
	Element.Setup(ElementList, InLayer, PaintGeometry, InDrawEffects);
	Element.SetTint(InTint);
	Element.SetText(ElementList, InText, InFontInfo, StartIndex, EndIndex);
}

void FSlateDrawElement::MakeText( FSlateWindowElementList& ElementList, uint32 InLayer, const FPaintGeometry& PaintGeometry, const FString& InText, const FSlateFontInfo& InFontInfo, ESlateDrawEffect InDrawEffects, const FLinearColor& InTint )
{
	SCOPE_CYCLE_COUNTER(STAT_SlateDrawElementMakeTime)
	PaintGeometry.CommitTransformsIfUsingLegacyConstructor();

	if (ShouldCull(ElementList, PaintGeometry, InTint))
	{
		return;
	}

	FSlateDrawText& Box = ElementList.AddText();
	Box.Setup(ElementList, InLayer, PaintGeometry, InDrawEffects);
	Box.SetTint(InTint);
	Box.SetText(ElementList, InText, InFontInfo);
}

void FSlateDrawElement::MakeText( FSlateWindowElementList& ElementList, uint32 InLayer, const FPaintGeometry& PaintGeometry, const FText& InText, const FSlateFontInfo& InFontInfo, ESlateDrawEffect InDrawEffects, const FLinearColor& InTint )
{
	SCOPE_CYCLE_COUNTER(STAT_SlateDrawElementMakeTime)
	PaintGeometry.CommitTransformsIfUsingLegacyConstructor();

	if (ShouldCull(ElementList, PaintGeometry, InTint))
	{
		return;
	}

	FSlateDrawText& Element = ElementList.AddText();
	Element.Setup(ElementList, InLayer, PaintGeometry, InDrawEffects);
	Element.SetTint(InTint);
	Element.SetText(ElementList, InText.ToString(), InFontInfo);
}

void FSlateDrawElement::MakeShapedText( FSlateWindowElementList& ElementList, uint32 InLayer, const FPaintGeometry& PaintGeometry, const FShapedGlyphSequenceRef& InShapedGlyphSequence, ESlateDrawEffect InDrawEffects, const FLinearColor& BaseTint, const FLinearColor& OutlineTint )
{
	SCOPE_CYCLE_COUNTER(STAT_SlateDrawElementMakeTime)
	PaintGeometry.CommitTransformsIfUsingLegacyConstructor();

	if (ShouldCull(ElementList, PaintGeometry))
	{
		return;
	}

	FSlateDrawShapedText& Element = ElementList.AddShapedText();
	Element.Setup(ElementList, InLayer, PaintGeometry, InDrawEffects);
	Element.SetTint(BaseTint);
	Element.SetShapedText(InShapedGlyphSequence, OutlineTint);
}

void FSlateDrawElement::MakeGradient( FSlateWindowElementList& ElementList, uint32 InLayer, const FPaintGeometry& PaintGeometry, TArray<FSlateGradientStop> InGradientStops, EOrientation InGradientType, ESlateDrawEffect InDrawEffects )
{
	PaintGeometry.CommitTransformsIfUsingLegacyConstructor();

	if (ShouldCull(ElementList, PaintGeometry))
	{
		return;
	}

	FSlateDrawElement& DrawElt = ElementList.AddUninitialized();
	DrawElt.Init(ElementList, InLayer, PaintGeometry, InDrawEffects);
	DrawElt.ElementType = ET_Gradient;
	DrawElt.DataPayload.SetGradientPayloadProperties( InGradientStops, InGradientType );
}


void FSlateDrawElement::MakeSpline( FSlateWindowElementList& ElementList, uint32 InLayer, const FPaintGeometry& PaintGeometry, const FVector2D& InStart, const FVector2D& InStartDir, const FVector2D& InEnd, const FVector2D& InEndDir, float InThickness, ESlateDrawEffect InDrawEffects, const FLinearColor& InTint )
{
	PaintGeometry.CommitTransformsIfUsingLegacyConstructor();

	if (ShouldCull(ElementList))
	{
		return;
	}
	FSlateDrawElement& DrawElt = ElementList.AddUninitialized();
	DrawElt.Init(ElementList, InLayer, PaintGeometry, InDrawEffects);
	DrawElt.ElementType = ET_Spline;
	DrawElt.DataPayload.SetHermiteSplinePayloadProperties( InStart, InStartDir, InEnd, InEndDir, InThickness, InTint );
}

void FSlateDrawElement::MakeCubicBezierSpline(FSlateWindowElementList & ElementList, uint32 InLayer, const FPaintGeometry & PaintGeometry, const FVector2D & P0, const FVector2D & P1, const FVector2D & P2, const FVector2D & P3, float InThickness, ESlateDrawEffect InDrawEffects, const FLinearColor & InTint)
{
	PaintGeometry.CommitTransformsIfUsingLegacyConstructor();

	if (ShouldCull(ElementList))
	{
		return;
	}
	FSlateDrawElement& DrawElt = ElementList.AddUninitialized();
	DrawElt.Init(ElementList, InLayer, PaintGeometry, InDrawEffects);
	DrawElt.ElementType = ET_Spline;
	DrawElt.DataPayload.SetCubicBezierPayloadProperties(P0, P1, P2, P3, InThickness, InTint);
}


void FSlateDrawElement::MakeDrawSpaceSpline( FSlateWindowElementList& ElementList, uint32 InLayer, const FVector2D& InStart, const FVector2D& InStartDir, const FVector2D& InEnd, const FVector2D& InEndDir, float InThickness, ESlateDrawEffect InDrawEffects, const FLinearColor& InTint )
{
	MakeSpline( ElementList, InLayer, FPaintGeometry(), InStart, InStartDir, InEnd, InEndDir, InThickness, InDrawEffects, InTint );
}

void FSlateDrawElement::MakeDrawSpaceGradientSpline(FSlateWindowElementList& ElementList, uint32 InLayer, const FVector2D& InStart, const FVector2D& InStartDir, const FVector2D& InEnd, const FVector2D& InEndDir, const TArray<FSlateGradientStop>& InGradientStops, float InThickness, ESlateDrawEffect InDrawEffects)
{
	const FPaintGeometry PaintGeometry;
	PaintGeometry.CommitTransformsIfUsingLegacyConstructor();

	if (ShouldCull(ElementList))
	{
		return;
	}

	FSlateDrawElement& DrawElt = ElementList.AddUninitialized();
	DrawElt.Init(ElementList, InLayer, PaintGeometry, InDrawEffects);
	DrawElt.ElementType = ET_Spline;
	DrawElt.DataPayload.SetGradientHermiteSplinePayloadProperties(InStart, InStartDir, InEnd, InEndDir, InThickness, InGradientStops);
}

void FSlateDrawElement::MakeDrawSpaceGradientSpline(FSlateWindowElementList& ElementList, uint32 InLayer, const FVector2D& InStart, const FVector2D& InStartDir, const FVector2D& InEnd, const FVector2D& InEndDir, const FSlateRect InClippingRect, const TArray<FSlateGradientStop>& InGradientStops, float InThickness, ESlateDrawEffect InDrawEffects)
{
	const FPaintGeometry PaintGeometry;
	PaintGeometry.CommitTransformsIfUsingLegacyConstructor();

	if (ShouldCull(ElementList))
	{
		return;
	}

	FSlateDrawElement& DrawElt = ElementList.AddUninitialized();
	DrawElt.Init(ElementList, InLayer, PaintGeometry, InDrawEffects);
	DrawElt.ElementType = ET_Spline;
	DrawElt.DataPayload.SetGradientHermiteSplinePayloadProperties(InStart, InStartDir, InEnd, InEndDir, InThickness, InGradientStops);
}


void FSlateDrawElement::MakeLines(FSlateWindowElementList& ElementList, uint32 InLayer, const FPaintGeometry& PaintGeometry, const TArray<FVector2D>& Points, ESlateDrawEffect InDrawEffects, const FLinearColor& InTint, bool bAntialias, float Thickness)
{
	PaintGeometry.CommitTransformsIfUsingLegacyConstructor();

	if (ShouldCull(ElementList))
	{
		return;
	}

	FSlateDrawLines& Element = ElementList.AddLines();
	Element.Setup(ElementList, InLayer, PaintGeometry, InDrawEffects);
	Element.SetTint(InTint);
	Element.SetThickness(Thickness);
	Element.SetLines(ElementList, Points, bAntialias, nullptr);
}

void FSlateDrawElement::MakeLines( FSlateWindowElementList& ElementList, uint32 InLayer, const FPaintGeometry& PaintGeometry, const TArray<FVector2D>& Points, const TArray<FLinearColor>& PointColors, ESlateDrawEffect InDrawEffects, const FLinearColor& InTint, bool bAntialias, float Thickness )
{
	PaintGeometry.CommitTransformsIfUsingLegacyConstructor();

	if (ShouldCull(ElementList))
	{
		return;
	}

	FSlateDrawLines& Element = ElementList.AddLines();
	Element.Setup(ElementList, InLayer, PaintGeometry, InDrawEffects);
	Element.SetTint(InTint);
	Element.SetThickness(Thickness);
	Element.SetLines(ElementList, Points, bAntialias, &PointColors);
}

void FSlateDrawElement::MakeViewport( FSlateWindowElementList& ElementList, uint32 InLayer, const FPaintGeometry& PaintGeometry, TSharedPtr<const ISlateViewport> Viewport, ESlateDrawEffect InDrawEffects, const FLinearColor& InTint )
{
	PaintGeometry.CommitTransformsIfUsingLegacyConstructor();

	if (ShouldCull(ElementList))
	{
		return;
	}

	FSlateDrawElement& DrawElt = ElementList.AddUninitialized();
	DrawElt.Init(ElementList, InLayer, PaintGeometry, InDrawEffects);
	DrawElt.ElementType = ET_Viewport;
	DrawElt.DataPayload.SetViewportPayloadProperties( Viewport, InTint );
}


void FSlateDrawElement::MakeCustom( FSlateWindowElementList& ElementList, uint32 InLayer, TSharedPtr<ICustomSlateElement, ESPMode::ThreadSafe> CustomDrawer )
{
	if (ShouldCull(ElementList))
	{
		return;
	}

	FSlateDrawElement& DrawElt = ElementList.AddUninitialized();
	DrawElt.Init(ElementList, InLayer, FPaintGeometry(), ESlateDrawEffect::None);
	DrawElt.RenderTransform = FSlateRenderTransform();
	DrawElt.ElementType = ET_Custom;
	DrawElt.DataPayload.SetCustomDrawerPayloadProperties( CustomDrawer );
}


void FSlateDrawElement::MakeCustomVerts(FSlateWindowElementList& ElementList, uint32 InLayer, const FSlateResourceHandle& InRenderResourceHandle, const TArray<FSlateVertex>& InVerts, const TArray<SlateIndex>& InIndexes, ISlateUpdatableInstanceBuffer* InInstanceData, uint32 InInstanceOffset, uint32 InNumInstances, ESlateDrawEffect InDrawEffects)
{
	SCOPE_CYCLE_COUNTER(STAT_SlateDrawElementMakeCustomVertsTime)

	if (ShouldCull(ElementList))
	{
		return;
	}

	FSlateDrawElement& DrawElt = ElementList.AddUninitialized();
	DrawElt.Init(ElementList, InLayer, FPaintGeometry(), InDrawEffects);
	DrawElt.RenderTransform = FSlateRenderTransform();
	DrawElt.ElementType = ET_CustomVerts;

	FSlateShaderResourceProxy* RenderingProxy = InRenderResourceHandle.Data->Proxy;

	DrawElt.DataPayload.SetCustomVertsPayloadProperties(RenderingProxy, InVerts, InIndexes, InInstanceData, InInstanceOffset, InNumInstances);
}

void FSlateDrawElement::MakeCachedBuffer(FSlateWindowElementList& ElementList, uint32 InLayer, TSharedPtr<FSlateRenderDataHandle, ESPMode::ThreadSafe>& CachedRenderDataHandle, const FVector2D& Offset)
{
	if (ShouldCull(ElementList))
	{
		return;
	}

	FSlateDrawElement& DrawElt = ElementList.AddUninitialized();
	DrawElt.Init(ElementList, InLayer, FPaintGeometry(), ESlateDrawEffect::None);
	DrawElt.RenderTransform = FSlateRenderTransform();
	DrawElt.ElementType = ET_CachedBuffer;
	DrawElt.DataPayload.SetCachedBufferPayloadProperties(CachedRenderDataHandle.Get(), Offset);

	ElementList.BeginUsingCachedBuffer(CachedRenderDataHandle);
}

void FSlateDrawElement::MakeLayer(FSlateWindowElementList& ElementList, uint32 InLayer, TSharedPtr<FSlateDrawLayerHandle, ESPMode::ThreadSafe>& DrawLayerHandle)
{
	FSlateDrawElement& DrawElt = ElementList.AddUninitialized();
	DrawElt.Init(ElementList, InLayer, FPaintGeometry(), ESlateDrawEffect::None);
	DrawElt.RenderTransform = FSlateRenderTransform();
	DrawElt.ElementType = ET_Layer;
	DrawElt.DataPayload.SetLayerPayloadProperties(DrawLayerHandle.Get());
}

void FSlateDrawElement::MakePostProcessPass(FSlateWindowElementList& ElementList, uint32 InLayer, const FPaintGeometry& PaintGeometry, const FVector4& Params, int32 DownsampleAmount)
{
	PaintGeometry.CommitTransformsIfUsingLegacyConstructor();

	if (ShouldCull(ElementList))
	{
		return;
	}

	FSlateDrawElement& DrawElt = ElementList.AddUninitialized();
	DrawElt.Init(ElementList, InLayer, PaintGeometry, ESlateDrawEffect::None);
	DrawElt.ElementType = ET_PostProcessPass;
	DrawElt.DataPayload.DownsampleAmount = DownsampleAmount;
	DrawElt.DataPayload.PostProcessData = Params;
}

FVector2D FSlateDrawElement::GetRotationPoint(const FPaintGeometry& PaintGeometry, const TOptional<FVector2D>& UserRotationPoint, ERotationSpace RotationSpace)
{
	FVector2D RotationPoint(0, 0);

	const FVector2D& LocalSize = PaintGeometry.GetLocalSize();

	switch (RotationSpace)
	{
		case RelativeToElement:
		{
			// If the user did not specify a rotation point, we rotate about the center of the element
			RotationPoint = UserRotationPoint.Get(LocalSize * 0.5f);
		}
		break;
		case RelativeToWorld:
		{
			// its in world space, must convert the point to local space.
			RotationPoint = TransformPoint(Inverse(PaintGeometry.GetAccumulatedRenderTransform()), UserRotationPoint.Get(FVector2D::ZeroVector));
		}
		break;
	default:
		check(0);
		break;
	}

	return RotationPoint;
}

void FSlateBatchData::Reset()
{
	RenderBatches.Reset();
	
	// note: LayerToElementBatches is not reset here as the same layers are 
	// more than likely reused and we can save memory allocations by not resetting the map every frame

	NumBatchedVertices = 0;
	NumBatchedIndices = 0;
	NumLayers = 0;

	bIsStencilBufferRequired = false;

	RenderDataHandle.Reset();
}

#define MAX_VERT_ARRAY_RECYCLE (200)
#define MAX_INDEX_ARRAY_RECYCLE (500)

bool FSlateBatchData::IsStencilClippingRequired() const
{
	return bIsStencilBufferRequired;
}

void FSlateBatchData::DetermineIsStencilClippingRequired(const TArray<FSlateClippingState>& ClippingStates)
{
	bIsStencilBufferRequired = false;

	for (const FSlateClippingState& Clipping : ClippingStates)
	{
		if (Clipping.GetClippingMethod() == EClippingMethod::Stencil)
		{
			bIsStencilBufferRequired = true;
			return;
		}
	}
}

void FSlateBatchData::AssignVertexArrayToBatch( FSlateElementBatch& Batch )
{
	// Get a free vertex array
	if (VertexArrayFreeList.Num() > 0)
	{
		Batch.VertexArrayIndex = VertexArrayFreeList.Pop(/*bAllowShrinking=*/ false);
	}
	else
	{
		// There are no free vertex arrays so we must add one		
		uint32 NewIndex = BatchVertexArrays.Add(FSlateVertexArray());
		ResetVertexArray(BatchVertexArrays[NewIndex]);

		Batch.VertexArrayIndex = NewIndex;
	}
}

void FSlateBatchData::AssignIndexArrayToBatch( FSlateElementBatch& Batch )
{
	// Get a free index array
	if (IndexArrayFreeList.Num() > 0)
	{
		Batch.IndexArrayIndex = IndexArrayFreeList.Pop(/*bAllowShrinking=*/ false);
	}
	else
	{
		// There are no free index arrays so we must add one
		uint32 NewIndex = BatchIndexArrays.Add(FSlateIndexArray());
		ResetIndexArray(BatchIndexArrays[NewIndex]);

		Batch.IndexArrayIndex = NewIndex;
	}
}

void FSlateBatchData::FillVertexAndIndexBuffer(uint8* VertexBuffer, uint8* IndexBuffer, bool bAbsoluteIndices)
{
	int32 IndexOffset = 0;
	int32 VertexOffset = 0;
	int32 BaseVertexIndex = 0;

	for ( const FSlateRenderBatch& Batch : RenderBatches )
	{
		// Ignore foreign batches that are inserted into our render set.
		if ( RenderDataHandle != Batch.CachedRenderHandle )
		{
			continue;
		}

		if ( Batch.VertexArrayIndex != INDEX_NONE && Batch.IndexArrayIndex != INDEX_NONE )
		{
			FSlateVertexArray& Vertices = BatchVertexArrays[Batch.VertexArrayIndex];
			FSlateIndexArray& Indices = BatchIndexArrays[Batch.IndexArrayIndex];
	
			if ( Vertices.Num() && Indices.Num() )
			{
				uint32 RequiredVertexSize = Vertices.Num() * Vertices.GetTypeSize();
				uint32 RequiredIndexSize = Indices.Num() * Indices.GetTypeSize();

				FMemory::Memcpy(VertexBuffer + VertexOffset, Vertices.GetData(), RequiredVertexSize);
				if (BaseVertexIndex == 0 || !bAbsoluteIndices)
				{
					FMemory::Memcpy(IndexBuffer + IndexOffset, Indices.GetData(), RequiredIndexSize);
				}
				else
				{
					SlateIndex* TargetIndexBuffer = (SlateIndex*)(IndexBuffer + IndexOffset);
					for (int32 i = 0; i < Indices.Num(); ++i)
					{
						TargetIndexBuffer[i] = Indices[i] + BaseVertexIndex;
					}
				}
				
				BaseVertexIndex+= Vertices.Num();
				IndexOffset += ( Indices.Num()*sizeof(SlateIndex) );
				VertexOffset += ( Vertices.Num()*sizeof(FSlateVertex) );

				Vertices.Reset();
				Indices.Reset();

				if ( Vertices.GetSlack() > MAX_VERT_ARRAY_RECYCLE )
				{
					ResetVertexArray(Vertices);
				}

				if ( Indices.GetSlack() > MAX_INDEX_ARRAY_RECYCLE )
				{
					ResetIndexArray(Indices);
				}
			}

			VertexArrayFreeList.Add(Batch.VertexArrayIndex);
			IndexArrayFreeList.Add(Batch.IndexArrayIndex);
		}
	}
}

void FSlateBatchData::CreateRenderBatches(FElementBatchMap& LayerToElementBatches)
{
	checkSlow(IsInRenderingThread());

	uint32 VertexOffset = 0;
	uint32 IndexOffset = 0;

	{
		SCOPED_NAMED_EVENT_TEXT("SlateRT::CreateRenderBatches", FColor::Magenta);
		Merge(LayerToElementBatches, VertexOffset, IndexOffset);
	}

	// 
	if ( RenderDataHandle.IsValid() )
	{
		RenderDataHandle->SetRenderBatches(&RenderBatches);
	}
}

void FSlateBatchData::AddRenderBatch(uint32 InLayer, const FSlateElementBatch& InElementBatch, int32 InNumVertices, int32 InNumIndices, int32 InVertexOffset, int32 InIndexOffset)
{
	NumBatchedVertices += InNumVertices;
	NumBatchedIndices += InNumIndices;

	const int32 Index = RenderBatches.Add(FSlateRenderBatch(InLayer, InElementBatch, RenderDataHandle, InNumVertices, InNumIndices, InVertexOffset, InIndexOffset));
	RenderBatches[Index].DynamicOffset = FVector2D::ZeroVector;
}

void FSlateBatchData::ResetVertexArray(FSlateVertexArray& InOutVertexArray)
{
	InOutVertexArray.Empty(MAX_VERT_ARRAY_RECYCLE);
}

void FSlateBatchData::ResetIndexArray(FSlateIndexArray& InOutIndexArray)
{
	InOutIndexArray.Empty(MAX_INDEX_ARRAY_RECYCLE);
}


void FSlateBatchData::Merge(FElementBatchMap& InLayerToElementBatches, uint32& VertexOffset, uint32& IndexOffset)
{
	InLayerToElementBatches.Sort();

	const bool bExpandLayersAndCachedHandles = RenderDataHandle.IsValid() == false;

	InLayerToElementBatches.ForEachLayer([&] (uint32 Layer, FElementBatchArray& ElementBatches)
	{
		++NumLayers;
		for ( FElementBatchArray::TIterator BatchIt(ElementBatches); BatchIt; ++BatchIt )
		{
			FSlateElementBatch& ElementBatch = *BatchIt;

			if ( ElementBatch.GetCustomDrawer().IsValid() )
			{
				AddRenderBatch(Layer, ElementBatch, 0, 0, 0, 0);
			}
			else if(ElementBatch.GetShaderType() == ESlateShader::PostProcess)
			{
				AddRenderBatch(Layer, ElementBatch, 0, 0, 0, 0);
			}
			else
			{
				if ( bExpandLayersAndCachedHandles )
				{
					if ( FSlateRenderDataHandle* RenderHandle = ElementBatch.GetCachedRenderHandle().Get() )
					{
						TArray<FSlateRenderBatch>* ForeignBatches = RenderHandle->GetRenderBatches();
						//TArray<FSlateClippingState>* ForeignClipState = RenderHandle->GetClipStates();
						if (ForeignBatches /*&& ForeignClipState*/)
						{
							//int32 Offset;
							//RenderClipStates.Append(*ForeignClipState);

							TArray<FSlateRenderBatch>& ForeignBatchesRef = *ForeignBatches;
							for ( int32 i = 0; i < ForeignBatches->Num(); i++ )
							{
								TSharedPtr<FSlateDrawLayerHandle, ESPMode::ThreadSafe> LayerHandle = ForeignBatchesRef[i].LayerHandle.Pin();
								if ( LayerHandle.IsValid() )
								{
									// If a record was added for a layer, but nothing was ever drawn for it, the batch map will be null.
									if ( LayerHandle->BatchMap )
									{
										Merge(*LayerHandle->BatchMap, VertexOffset, IndexOffset);
										LayerHandle->BatchMap = nullptr;
									}
								}
								else
								{
									const int32 Index = RenderBatches.Add(ForeignBatchesRef[i]);
									RenderBatches[Index].DynamicOffset = ElementBatch.GetCachedRenderDataOffset();
									//RenderBatches[Index].ClippingIndex = ElementBatch.GetClippingIndex();
								}
							}
						}

						continue;
					}
				}
				else
				{
					// Insert if we're not expanding
					if ( FSlateDrawLayerHandle* LayerHandle = ElementBatch.GetLayerHandle().Get() )
					{
						AddRenderBatch(Layer, ElementBatch, 0, 0, 0, 0);
						continue;
					}
				}
				
				// This is the normal path, for draw buffers that just contain Vertices and Indices.
				if ( ElementBatch.VertexArrayIndex != INDEX_NONE && ElementBatch.IndexArrayIndex != INDEX_NONE )
				{
					FSlateVertexArray& BatchVertices = GetBatchVertexList(ElementBatch);
					FSlateIndexArray& BatchIndices = GetBatchIndexList(ElementBatch);

					// We should have at least some vertices and indices in the batch or none at all
					check(BatchVertices.Num() > 0 && BatchIndices.Num() > 0 || BatchVertices.Num() == 0 && BatchIndices.Num() == 0);

					if ( BatchVertices.Num() > 0 && BatchIndices.Num() > 0 )
					{
						const int32 NumVertices = BatchVertices.Num();
						const int32 NumIndices = BatchIndices.Num();

						AddRenderBatch(Layer, ElementBatch, NumVertices, NumIndices, VertexOffset, IndexOffset);

						VertexOffset += BatchVertices.Num();
						IndexOffset += BatchIndices.Num();
					}
					else
					{
						VertexArrayFreeList.Add(ElementBatch.VertexArrayIndex);
						IndexArrayFreeList.Add(ElementBatch.IndexArrayIndex);
					}
				}
			}
		}

		ElementBatches.Reset();
	});

	InLayerToElementBatches.Reset();
}

void FSlateWindowElementList::MergeResources(const TSet<UObject*>& AssociatedResources)
{
	ExtraResources.Append(AssociatedResources);
}

void FSlateWindowElementList::MergeElementList(FSlateWindowElementList* ElementList, FVector2D AbsoluteOffset)
{
	const bool bMoved = !AbsoluteOffset.IsZero();

	if (bMoved)
	{
		const TArray<FSlateDrawElement>& CachedElements = ElementList->GetDrawElements();
		const int32 CachedElementCount = CachedElements.Num();

		for (int32 Index = 0; Index < CachedElementCount; Index++)
		{
			const FSlateDrawElement& LocalElement = CachedElements[Index];
			FSlateDrawElement AbsElement = LocalElement;
			FSlateDrawElement::ApplyPositionOffset(AbsElement, AbsoluteOffset);
			AddItem(AbsElement);
		}

		FSlateDrawLayer& OtherLayer = ElementList->GetRootDrawLayer();
		FSlateDrawLayer* Layer = DrawStack.Last();

		for (const FSlateDrawBox& Element : OtherLayer.BoxElements)
		{
			Layer->BoxElements.Add_GetRef(Element).ApplyPositionOffset(AbsoluteOffset);
		}

		for (const FSlateDrawBox& Element : OtherLayer.BorderElements)
		{
			Layer->BorderElements.Add_GetRef(Element).ApplyPositionOffset(AbsoluteOffset);
		}

		for (const FSlateDrawText& Element : OtherLayer.TextElements)
		{
			Layer->TextElements.Add_GetRef(Element).ApplyPositionOffset(AbsoluteOffset);
		}

		for (const FSlateDrawShapedText& Element : OtherLayer.ShapedTextElements)
		{
			Layer->ShapedTextElements.Add_GetRef(Element).ApplyPositionOffset(AbsoluteOffset);
		}

		for (const FSlateDrawLines& Element : OtherLayer.LineElements)
		{
			Layer->LineElements.Add_GetRef(Element).ApplyPositionOffset(AbsoluteOffset);
		}
	}
	else
	{
		AppendItems(ElementList);
	}
}

FSlateWindowElementList::FDeferredPaint::FDeferredPaint( const TSharedRef<const SWidget>& InWidgetToPaint, const FPaintArgs& InArgs, const FGeometry InAllottedGeometry, const FWidgetStyle& InWidgetStyle, bool InParentEnabled )
	: WidgetToPaintPtr( InWidgetToPaint )
	, Args( InArgs )
	, AllottedGeometry( InAllottedGeometry )
	, WidgetStyle( InWidgetStyle )
	, bParentEnabled( InParentEnabled )
{
}

FSlateWindowElementList::FDeferredPaint::FDeferredPaint(const FDeferredPaint& Copy, const FPaintArgs& InArgs)
	: WidgetToPaintPtr(Copy.WidgetToPaintPtr)
	, Args(InArgs)
	, AllottedGeometry(Copy.AllottedGeometry)
	, WidgetStyle(Copy.WidgetStyle)
	, bParentEnabled(Copy.bParentEnabled)
{
}

int32 FSlateWindowElementList::FDeferredPaint::ExecutePaint(int32 LayerId, FSlateWindowElementList& OutDrawElements, const FSlateRect& MyCullingRect) const
{
	TSharedPtr<const SWidget> WidgetToPaint = WidgetToPaintPtr.Pin();
	if ( WidgetToPaint.IsValid() )
	{
		return WidgetToPaint->Paint( Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, WidgetStyle, bParentEnabled );
	}

	return LayerId;
}

FSlateWindowElementList::FDeferredPaint FSlateWindowElementList::FDeferredPaint::Copy(const FPaintArgs& InArgs)
{
	return FDeferredPaint(*this, InArgs);
}


void FSlateWindowElementList::QueueDeferredPainting( const FDeferredPaint& InDeferredPaint )
{
	DeferredPaintList.Add(MakeShareable(new FDeferredPaint(InDeferredPaint)));
}

int32 FSlateWindowElementList::PaintDeferred(int32 LayerId, const FSlateRect& MyCullingRect)
{
	bNeedsDeferredResolve = false;

	int32 ResolveIndex = ResolveToDeferredIndex.Pop(false);

	for ( int32 i = ResolveIndex; i < DeferredPaintList.Num(); ++i )
	{
		LayerId = DeferredPaintList[i]->ExecutePaint(LayerId, *this, MyCullingRect);
	}

	for ( int32 i = DeferredPaintList.Num() - 1; i >= ResolveIndex; --i )
	{
		DeferredPaintList.RemoveAt(i, 1, false);
	}

	return LayerId;
}

void FSlateWindowElementList::BeginDeferredGroup()
{
	ResolveToDeferredIndex.Add(DeferredPaintList.Num());
}

void FSlateWindowElementList::EndDeferredGroup()
{
	bNeedsDeferredResolve = true;
}



FSlateWindowElementList::FVolatilePaint::FVolatilePaint(const TSharedRef<const SWidget>& InWidgetToPaint, const FPaintArgs& InArgs, const FGeometry InAllottedGeometry, const FSlateRect InMyCullingRect, const TOptional<FSlateClippingState>& InClippingState, int32 InLayerId, const FWidgetStyle& InWidgetStyle, bool InParentEnabled)
	: WidgetToPaintPtr(InWidgetToPaint)
	, Args(InArgs.EnableCaching(InArgs.GetLayoutCache(), InArgs.GetParentCacheNode(), false, true))
	, AllottedGeometry(InAllottedGeometry)
	, MyCullingRect(InMyCullingRect)
	, ClippingState(InClippingState)
	, LayerId(InLayerId)
	, WidgetStyle(InWidgetStyle)
	, bParentEnabled(InParentEnabled)
{
}

static const FName InvalidationPanelName(TEXT("SInvalidationPanel"));

int32 FSlateWindowElementList::FVolatilePaint::ExecutePaint(FSlateWindowElementList& OutDrawElements, double InCurrentTime, float InDeltaTime, const FVector2D& InDynamicOffset) const
{
	TSharedPtr<const SWidget> WidgetToPaint = WidgetToPaintPtr.Pin();
	if ( WidgetToPaint.IsValid() )
	{
#if SLATE_VERBOSE_NAMED_EVENTS
		FScopedNamedEvent Event(FColor::Orange, *FReflectionMetaData::GetWidgetDebugInfo(WidgetToPaint.Get()));
#endif

		// Have to run a slate pre-pass for all volatile elements, some widgets cache information like 
		// the STextBlock.  This may be all kinds of terrible an idea to do during paint.
		SWidget* MutableWidget = const_cast<SWidget*>( WidgetToPaint.Get() );

		if ( MutableWidget->GetType() != InvalidationPanelName )
		{
			MutableWidget->SlatePrepass(AllottedGeometry.Scale);
		}

		FPaintArgs PaintArgs = Args.WithNewTime(InCurrentTime, InDeltaTime);

		if (ClippingState.IsSet())
		{
			FSlateClippingState ExistingClippingState = ClippingState.GetValue();
			OutDrawElements.GetClippingManager().PushClippingState(ExistingClippingState);
		}

		int32 NewLayer = 0;
		if (InDynamicOffset.IsZero())
		{
			NewLayer = WidgetToPaint->Paint(PaintArgs, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, WidgetStyle, bParentEnabled);
		}
		else
		{
			FSlateRect LocalRect = MyCullingRect.OffsetBy(InDynamicOffset);
			FGeometry LocalGeometry = AllottedGeometry;
			LocalGeometry.AppendTransform(FSlateLayoutTransform(InDynamicOffset));
			
			NewLayer = WidgetToPaint->Paint(PaintArgs, LocalGeometry, LocalRect, OutDrawElements, LayerId, WidgetStyle, bParentEnabled);
		}

		if (ClippingState.IsSet())
		{
			OutDrawElements.GetClippingManager().PopClip();
		}

		return NewLayer;
	}

	return LayerId;
}

void FSlateWindowElementList::QueueVolatilePainting(const FVolatilePaint& InVolatilePaint)
{
	TSharedPtr< FSlateDrawLayerHandle, ESPMode::ThreadSafe > LayerHandle = MakeShareable(new FSlateDrawLayerHandle());

	FSlateDrawElement::MakeLayer(*this, InVolatilePaint.GetLayerId(), LayerHandle);

	const int32 NewEntryIndex = VolatilePaintList.Add(MakeShareable(new FVolatilePaint(InVolatilePaint)));
	VolatilePaintList[NewEntryIndex]->LayerHandle = LayerHandle;
}

int32 FSlateWindowElementList::PaintVolatile(FSlateWindowElementList& OutElementList, double InCurrentTime, float InDeltaTime, const FVector2D& InDynamicOffset)
{
	int32 MaxLayerId = 0;

	for ( int32 VolatileIndex = 0; VolatileIndex < VolatilePaintList.Num(); ++VolatileIndex )
	{
		const TSharedPtr<FVolatilePaint>& Args = VolatilePaintList[VolatileIndex];

		OutElementList.BeginLogicalLayer(Args->LayerHandle);
		MaxLayerId = FMath::Max(MaxLayerId, Args->ExecutePaint(OutElementList, InCurrentTime, InDeltaTime, InDynamicOffset));
		OutElementList.EndLogicalLayer();
	}

	return MaxLayerId;
}

int32 FSlateWindowElementList::PaintVolatileRootLayer(FSlateWindowElementList& OutElementList, double InCurrentTime, float InDeltaTime, const FVector2D& InDynamicOffset)
{
	int32 MaxLayerId = 0;

	for (int32 VolatileIndex = 0; VolatileIndex < VolatilePaintList.Num(); ++VolatileIndex)
	{
		const TSharedPtr<FVolatilePaint>& Args = VolatilePaintList[VolatileIndex];
		MaxLayerId = FMath::Max(MaxLayerId, Args->ExecutePaint(OutElementList, InCurrentTime, InDeltaTime, InDynamicOffset));
	}

	return MaxLayerId;
}


void FSlateWindowElementList::BeginLogicalLayer(const TSharedPtr<FSlateDrawLayerHandle, ESPMode::ThreadSafe>& LayerHandle)
{
	// Don't attempt to begin logical layers inside a cached view of the data.
	checkSlow(!IsCachedRenderDataInUse());

	TSharedPtr<FSlateDrawLayer> Layer;
	{
		//SCOPED_NAMED_EVENT(FindLayer, FColor::Orange);
		Layer = DrawLayers.FindRef(LayerHandle);
	}

	if ( !Layer.IsValid() )
	{
		if ( DrawLayerPool.Num() > 0 )
		{
			Layer = DrawLayerPool.Pop(false);
		}
		else
		{
			Layer = MakeShareable(new FSlateDrawLayer());
		}

		//SCOPED_NAMED_EVENT(AddLayer, FColor::Orange);
		DrawLayers.Add(LayerHandle, Layer);
	}

	//SCOPED_NAMED_EVENT(PushLayer, FColor::Orange);
	DrawStack.Push(Layer.Get());
}

void FSlateWindowElementList::EndLogicalLayer()
{
	DrawStack.Pop();
}

void FSlateWindowElementList::PushClip(const FSlateClippingZone& InClipZone)
{
	ClippingManager.PushClip(InClipZone);
}

int32 FSlateWindowElementList::GetClippingIndex() const
{
	return ClippingManager.GetClippingIndex();
}

TOptional<FSlateClippingState> FSlateWindowElementList::GetClippingState() const
{
	const int32 CurrentIndex = ClippingManager.GetClippingIndex();
	if (CurrentIndex != INDEX_NONE)
	{
		return ClippingManager.GetClippingStates()[CurrentIndex];
	}

	return TOptional<FSlateClippingState>();
}

void FSlateWindowElementList::PopClip()
{
	ClippingManager.PopClip();
}

FSlateRenderDataHandle::FSlateRenderDataHandle(const ILayoutCache* InCacher, ISlateRenderDataManager* InManager)
	: Cacher(InCacher)
	, Manager(InManager)
	, RenderBatches(nullptr)
	, UsageCount(0)
{
}

FSlateRenderDataHandle::~FSlateRenderDataHandle()
{
	if ( Manager )
	{
		Manager->BeginReleasingRenderData(this);
	}
}

void FSlateRenderDataHandle::Disconnect()
{
	Manager = nullptr;
	RenderBatches = nullptr;
}

TSharedRef<FSlateRenderDataHandle, ESPMode::ThreadSafe> FSlateWindowElementList::CacheRenderData(const ILayoutCache* Cacher)
{
	// Don't attempt to use this slate window element list if the cache is still being used.
	checkSlow(!IsCachedRenderDataInUse());

	FSlateRenderer* Renderer = FSlateApplicationBase::Get().GetRenderer();

	TSharedRef<FSlateRenderDataHandle, ESPMode::ThreadSafe> CachedRenderDataHandleRef = Renderer->CacheElementRenderData(Cacher, *this);
	CachedRenderDataHandle = CachedRenderDataHandleRef;

	return CachedRenderDataHandleRef;
}

void FSlateWindowElementList::PreDraw_ParallelThread()
{
	check(IsInParallelRenderingThread());

	for ( auto& Entry : DrawLayers )
	{
		checkSlow(Entry.Key->BatchMap == nullptr);
		Entry.Key->BatchMap = &Entry.Value->GetElementBatchMap();
	}
}

void FSlateWindowElementList::PostDraw_ParallelThread()
{
	check(IsInParallelRenderingThread());

	for ( auto& Entry : DrawLayers )
	{
		Entry.Key->BatchMap = nullptr;
	}

	for ( TSharedPtr<FSlateRenderDataHandle, ESPMode::ThreadSafe>& Handle : CachedRenderHandlesInUse )
	{
		Handle->EndUsing();
	}

	CachedRenderHandlesInUse.Reset();
	bReportReferences = false;
}

SLATECORE_API void FSlateWindowElementList::SetRenderTargetWindow(SWindow* InRenderTargetWindow)
{
	check(IsThreadSafeForSlateRendering());
	RenderTargetWindow = InRenderTargetWindow;
}

DECLARE_MEMORY_STAT(TEXT("FSlateWindowElementList MemManager"), STAT_FSlateWindowElementListMemManager, STATGROUP_SlateVerbose);
DECLARE_DWORD_COUNTER_STAT(TEXT("FSlateWindowElementList MemManager Count"), STAT_FSlateWindowElementListMemManagerCount, STATGROUP_SlateVerbose);

void FSlateWindowElementList::ResetElementBuffers()
{
	// Don't attempt to use this slate window element list if the cache is still being used.
	checkSlow(!IsCachedRenderDataInUse());
	check(IsThreadSafeForSlateRendering());

	bReportReferences = true;

	// Reset the Main Thread Resources, because we no longer need to keep these referenced objects alive.
	ExtraResources.Reset();

	DeferredPaintList.Reset();
	VolatilePaintList.Reset();
	BatchData.Reset();

	// Reset the draw elements on the root draw layer
	RootDrawLayer.ResetLayer();
	ClippingManager.ResetClippingState();

	// Return child draw layers to the pool, and reset their draw elements.
	for ( auto& Entry : DrawLayers )
	{
		FSlateDrawLayer* Layer = Entry.Value.Get();
		Layer->ResetLayer();
		DrawLayerPool.Add(Entry.Value);
	}

	DrawLayers.Reset();

	DrawStack.Reset();
	DrawStack.Push(&RootDrawLayer);

	INC_DWORD_STAT(STAT_FSlateWindowElementListMemManagerCount);
	INC_MEMORY_STAT_BY(STAT_FSlateWindowElementListMemManager, MemManager.GetByteCount());

	MemManager.Flush();

	RenderTargetWindow = nullptr;
}

FSlateWindowElementList::FWindowElementGCObject::FWindowElementGCObject(FSlateWindowElementList* InOwner)
	: Owner(InOwner)
{
}

void FSlateWindowElementList::FWindowElementGCObject::AddReferencedObjects(FReferenceCollector& Collector)
{
	if (GGameThreadId == FPlatformTLS::GetCurrentThreadId() && Owner->bReportReferences)
	{
		AddReferencedObjectsForLayer(Collector, Owner->RootDrawLayer);

		for (auto& DrawLayerEntry : Owner->DrawLayers)
		{
			AddReferencedObjectsForLayer(Collector, *DrawLayerEntry.Value.Get());
		}

		Collector.AddReferencedObjects(Owner->ExtraResources);
	}
}

void FSlateWindowElementList::FWindowElementGCObject::AddReferencedObjectsForLayer(FReferenceCollector& Collector, FSlateDrawLayer& DrawLayer)
{
	for (FSlateDrawElement& DrawElement : DrawLayer.DrawElements)
	{
		if (const FSlateBrush* BrushResource = DrawElement.GetDataPayload().BrushResource)
		{
			if (UObject* ResourceObject = BrushResource->GetResourceObject())
			{
				Collector.AddReferencedObject(ResourceObject);
			}
		}
	}

	for (FSlateDrawBox& BoxElement : DrawLayer.BoxElements)
	{
		BoxElement.AddReferencedObjects(Collector);
	}

	for (FSlateDrawBox& BorderElement : DrawLayer.BorderElements)
	{
		BorderElement.AddReferencedObjects(Collector);
	}
}
