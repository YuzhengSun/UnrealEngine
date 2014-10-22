// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#pragma once


/**
 * Slate tool tip widget
 */
class SLATE_API SToolTip
	: public SCompoundWidget
	, public IToolTip
{
public:

	SLATE_BEGIN_ARGS( SToolTip )
		: _Text()
		, _Content()
		, _Font(FCoreStyle::Get().GetFontStyle("ToolTip.Font"))
		, _ColorAndOpacity( FSlateColor::UseForeground())
		, _TextMargin(FMargin(8.0f))
		, _BorderImage(FCoreStyle::Get().GetBrush("ToolTip.Background"))
		, _IsInteractive(false)
	{ }

		/** The text displayed in this tool tip */
		SLATE_TEXT_ATTRIBUTE(Text)

		/** Arbitrary content to be displayed in the tool tip; overrides any text that may be set. */
		SLATE_DEFAULT_SLOT(FArguments, Content)

		/** The font to use for this tool tip */
		SLATE_ATTRIBUTE(FSlateFontInfo, Font)
		
		/** Font color and opacity */
		SLATE_ATTRIBUTE(FSlateColor, ColorAndOpacity)
		
		/** Margin between the tool tip border and the text content */
		SLATE_ATTRIBUTE(FMargin, TextMargin)

		/** The background/border image to display */
		SLATE_ATTRIBUTE(const FSlateBrush*, BorderImage)

		/** Whether the tooltip should be considered interactive */
		SLATE_ATTRIBUTE(bool, IsInteractive)

	SLATE_END_ARGS()

	/**
	 * Construct this widget
	 *
	 * @param	InArgs	The declaration data for this widget
	 */
	void Construct( const FArguments& InArgs );

public:

	// IToolTip interface

	virtual TSharedRef<class SWidget> AsWidget( ) override
	{
		return AsShared();
	}

	virtual TSharedRef<SWidget> GetContentWidget( ) override
	{
		return ToolTipContent.ToSharedRef();
	}

	virtual void SetContentWidget(const TSharedRef<SWidget>& InContentWidget) override;

	virtual bool IsEmpty( ) const override;
	virtual bool IsInteractive( ) const override;

	virtual const FText& GetTextTooltip() const
	{
		return TextContent.Get();
	}

public:

	static float GetToolTipWrapWidth();

private:

	// Text block widget.
	TAttribute<FText> TextContent;

	// Content widget.
	TWeakPtr<SWidget> WidgetContent;

	// Wrapped content within the widget;
	TSharedPtr<SWidget> ToolTipContent;

	// Font used for the text displayed (where applicable)
	TAttribute<FSlateFontInfo> Font;
	
	// Color and opacity used for the text displayed (where applicable)
	TAttribute<FSlateColor> ColorAndOpacity;

	// Margin between the tool tip border and the text content
	TAttribute<FMargin> TextMargin;

	// The background/border image to display
	TAttribute<const FSlateBrush*> BorderImage;
	
	// Whether the tooltip should be considered interactive.
	TAttribute<bool> bIsInteractive;
};
