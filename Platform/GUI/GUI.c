#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BmpSupportLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcCpuLib.h>
#include <Library/OcPngLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "GUI.h"
#include "GuiIo.h"
#include "HwOps.h"

typedef struct {
  UINT32 MinX;
  UINT32 MinY;
  UINT32 MaxX;
  UINT32 MaxY;
} GUI_DRAW_REQUEST;

//
// I/O contexts
//
STATIC GUI_OUTPUT_CONTEXT            *mOutputContext    = NULL;
STATIC GUI_POINTER_CONTEXT           *mPointerContext   = NULL;
STATIC GUI_KEY_CONTEXT               *mKeyContext       = NULL;
//
// Screen buffer information
//
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL *mScreenBuffer     = NULL;
STATIC UINT32                        mScreenBufferDelta = 0;
STATIC GUI_SCREEN_CURSOR             mScreenViewCursor  = { 0, 0 };
//
// Frame timing information (60 FPS)
//
STATIC UINT64                        mDeltaTscTarget    = 0;
STATIC UINT64                        mStartTsc          = 0;
//
// Drawing rectangles information
//
STATIC UINT8                         mNumValidDrawReqs  = 0;
STATIC GUI_DRAW_REQUEST              mDrawRequests[4]   = { { 0 } };

BOOLEAN
GuiClipChildBounds (
  IN     INT64   ChildOffset,
  IN     UINT32  ChildLength,
  IN OUT UINT32  *ReqOffset,
  IN OUT UINT32  *ReqLength
  )
{
  UINT32 PosChildOffset;
  UINT32 OffsetDelta;
  UINT32 NewOffset;
  UINT32 NewLength;

  ASSERT (ReqOffset != NULL);
  ASSERT (ReqLength != NULL);

  if (ChildLength == 0) {
    return FALSE;
  }

  if (ChildOffset >= 0) {
    PosChildOffset = (UINT32)ChildOffset;
  } else {
    if ((INT64)ChildLength - (-ChildOffset) <= 0) {
      return FALSE;
    }

    PosChildOffset = 0;
    ChildLength    = (UINT32)(ChildLength - (-ChildOffset));
  }

  NewOffset = *ReqOffset;
  NewLength = *ReqLength;

  if (NewOffset >= PosChildOffset) {
    //
    // The requested offset starts within or past the child.
    //
    OffsetDelta = (NewOffset - PosChildOffset);
    if (ChildLength <= OffsetDelta) {
      //
      // The requested offset starts past the child.
      //
      return FALSE;
    }
    //
    // The requested offset starts within the child.
    //
    NewOffset -= PosChildOffset;
  } else {
    //
    // The requested offset ends within or before the child.
    //
    OffsetDelta = (PosChildOffset - NewOffset);
    if (NewLength <= OffsetDelta) {
      //
      // The requested offset ends before the child.
      //
      return FALSE;
    }
    //
    // The requested offset ends within the child.
    //
    NewOffset  = 0;
    NewLength -= OffsetDelta;
  }

  if (ChildOffset < 0) {
    NewOffset = (UINT32)(NewOffset + (-ChildOffset));
  }

  *ReqOffset = NewOffset;
  *ReqLength = NewLength;

  return TRUE;
}

VOID
GuiObjDrawDelegate (
  IN OUT GUI_OBJ              *This,
  IN OUT GUI_DRAWING_CONTEXT  *DrawContext,
  IN     VOID                 *Context OPTIONAL,
  IN     INT64                BaseX,
  IN     INT64                BaseY,
  IN     UINT32               OffsetX,
  IN     UINT32               OffsetY,
  IN     UINT32               Width,
  IN     UINT32               Height,
  IN     BOOLEAN              RequestDraw
  )
{
  BOOLEAN       Result;

  LIST_ENTRY    *ChildEntry;
  GUI_OBJ_CHILD *Child;

  UINT32        ChildDrawOffsetX;
  UINT32        ChildDrawOffsetY;
  UINT32        ChildDrawWidth;
  UINT32        ChildDrawHeight;

  ASSERT (This != NULL);
  ASSERT (This->Width  > OffsetX);
  ASSERT (This->Height > OffsetY);
  ASSERT (DrawContext != NULL);

  for (
    ChildEntry = GetPreviousNode (&This->Children, &This->Children);
    !IsNull (&This->Children, ChildEntry);
    ChildEntry = GetPreviousNode (&This->Children, ChildEntry)
    ) {
    Child = BASE_CR (ChildEntry, GUI_OBJ_CHILD, Link);

    ChildDrawOffsetX = OffsetX;
    ChildDrawWidth   = Width;
    Result = GuiClipChildBounds (
               Child->Obj.OffsetX,
               Child->Obj.Width,
               &ChildDrawOffsetX,
               &ChildDrawWidth
               );
    if (!Result) {
      continue;
    }

    ChildDrawOffsetY = OffsetY;
    ChildDrawHeight  = Height;
    Result = GuiClipChildBounds (
               Child->Obj.OffsetY,
               Child->Obj.Height,
               &ChildDrawOffsetY,
               &ChildDrawHeight
               );
    if (!Result) {
      continue;
    }

    ASSERT (Child->Obj.Draw != NULL);
    Child->Obj.Draw (
                 &Child->Obj,
                 DrawContext,
                 Context,
                 BaseX + Child->Obj.OffsetX,
                 BaseY + Child->Obj.OffsetY,
                 ChildDrawOffsetX,
                 ChildDrawOffsetY,
                 ChildDrawWidth,
                 ChildDrawHeight,
                 RequestDraw
                 );
  }
}

GUI_OBJ *
GuiObjDelegatePtrEvent (
  IN OUT GUI_OBJ              *This,
  IN OUT GUI_DRAWING_CONTEXT  *DrawContext,
  IN     VOID                 *Context OPTIONAL,
  IN     GUI_PTR_EVENT        Event,
  IN     INT64                BaseX,
  IN     INT64                BaseY,
  IN     INT64                OffsetX,
  IN     INT64                OffsetY
  )
{
  GUI_OBJ       *Obj;
  LIST_ENTRY    *ChildEntry;
  GUI_OBJ_CHILD *Child;

  ASSERT (This != NULL);
  ASSERT (This->Width  > OffsetX);
  ASSERT (This->Height > OffsetY);
  ASSERT (DrawContext != NULL);

  for (
    ChildEntry = GetFirstNode (&This->Children);
    !IsNull (&This->Children, ChildEntry);
    ChildEntry = GetNextNode (&This->Children, ChildEntry)
    ) {
    Child = BASE_CR (ChildEntry, GUI_OBJ_CHILD, Link);
    if (OffsetX  < Child->Obj.OffsetX
     || OffsetX >= Child->Obj.OffsetX + Child->Obj.Width
     || OffsetY  < Child->Obj.OffsetY
     || OffsetY >= Child->Obj.OffsetY + Child->Obj.Height) {
      continue;
    }

    ASSERT (Child->Obj.PtrEvent != NULL);
    Obj = Child->Obj.PtrEvent (
                       &Child->Obj,
                       DrawContext,
                       Context,
                       Event,
                       BaseX   + Child->Obj.OffsetX,
                       BaseY   + Child->Obj.OffsetY,
                       OffsetX - Child->Obj.OffsetX,
                       OffsetY - Child->Obj.OffsetY
                       );
    if (Obj != NULL) {
      return Obj;
    }
  }

  return NULL;
}

#define RGB_APPLY_OPACITY(Rgba, Opacity)  \
  (((Rgba) * (Opacity)) / 0xFF)

#define RGB_ALPHA_BLEND(Back, Front, InvFrontOpacity)  \
  ((Front) + RGB_APPLY_OPACITY (InvFrontOpacity, Back))

VOID
GuiBlendPixel (
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL        *BackPixel,
  IN     CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *FrontPixel,
  IN     UINT8                                Opacity
  )
{
  UINT8                               CombOpacity;
  UINT8                               InvFrontOpacity;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL       OpacFrontPixel;
  CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL *FinalFrontPixel;
  //
  // FIXME: Optimise with SIMD or such.
  // qt_blend_argb32_on_argb32 in QT
  //
  ASSERT (BackPixel != NULL);
  ASSERT (FrontPixel != NULL);

  if (FrontPixel->Reserved == 0) {
    return;
  }
  
  if (FrontPixel->Reserved == 0xFF) {
    if (Opacity == 0xFF) {
      BackPixel->Blue     = FrontPixel->Blue;
      BackPixel->Green    = FrontPixel->Green;
      BackPixel->Red      = FrontPixel->Red;
      BackPixel->Reserved = FrontPixel->Reserved;
      return;
    }

    CombOpacity = Opacity;
  } else {
    CombOpacity = RGB_APPLY_OPACITY (FrontPixel->Reserved, Opacity);
  }

  if (CombOpacity == 0) {
    return;
  } else if (CombOpacity == FrontPixel->Reserved) {
    FinalFrontPixel = FrontPixel;
  } else {
    OpacFrontPixel.Reserved = CombOpacity;
    OpacFrontPixel.Blue     = RGB_APPLY_OPACITY (FrontPixel->Blue,  Opacity);
    OpacFrontPixel.Green    = RGB_APPLY_OPACITY (FrontPixel->Green, Opacity);
    OpacFrontPixel.Red      = RGB_APPLY_OPACITY (FrontPixel->Red,   Opacity);

    FinalFrontPixel = &OpacFrontPixel;
  }

  InvFrontOpacity = (0xFF - CombOpacity);

  BackPixel->Blue = RGB_ALPHA_BLEND (
                      BackPixel->Blue,
                      FinalFrontPixel->Blue,
                      InvFrontOpacity
                      );
  BackPixel->Green = RGB_ALPHA_BLEND (
                       BackPixel->Green,
                       FinalFrontPixel->Green,
                       InvFrontOpacity
                       );
  BackPixel->Red = RGB_ALPHA_BLEND (
                     BackPixel->Red,
                     FinalFrontPixel->Red,
                     InvFrontOpacity
                     );

  if (BackPixel->Reserved != 0xFF) {
    BackPixel->Reserved = RGB_ALPHA_BLEND (
                            BackPixel->Reserved,
                            CombOpacity,
                            InvFrontOpacity
                            );
  }
}

VOID
GuiDrawToBuffer (
  IN     CONST GUI_IMAGE      *Image,
  IN     UINT8                Opacity,
  IN     BOOLEAN              Fill,
  IN OUT GUI_DRAWING_CONTEXT  *DrawContext,
  IN     INT64                BaseX,
  IN     INT64                BaseY,
  IN     UINT32               OffsetX,
  IN     UINT32               OffsetY,
  IN     UINT32               Width,
  IN     UINT32               Height,
  IN     BOOLEAN              RequestDraw
  )
{
  UINT32                              PosBaseX;
  UINT32                              PosBaseY;
  UINT32                              PosOffsetX;
  UINT32                              PosOffsetY;

  UINT32                              RowIndex;
  UINT32                              SourceRowOffset;
  UINT32                              TargetRowOffset;
  UINT32                              SourceColumnOffset;
  UINT32                              TargetColumnOffset;
  CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL *SourcePixel;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL       *TargetPixel;
  GUI_DRAW_REQUEST                    ThisReq;
  UINTN                               Index;

  UINT32                              ThisArea;

  UINT32                              ReqWidth;
  UINT32                              ReqHeight;
  UINT32                              ReqArea;

  UINT32                              CombMinX;
  UINT32                              CombMaxX;
  UINT32                              CombMinY;
  UINT32                              CombMaxY;
  UINT32                              CombWidth;
  UINT32                              CombHeight;
  UINT32                              CombArea;

  UINT32                              OverMinX;
  UINT32                              OverMaxX;
  UINT32                              OverMinY;
  UINT32                              OverMaxY;
  UINT32                              OverArea;
  UINT32                              OverWidth;
  UINT32                              OverHeight;

  UINT32                              ActualArea;

  ASSERT (Image != NULL);
  ASSERT (DrawContext != NULL);
  ASSERT (DrawContext->Screen != NULL);
  ASSERT (BaseX + OffsetX >= 0);
  ASSERT (BaseY + OffsetY >= 0);
  //
  // Only draw the onscreen parts.
  //
  if (BaseX >= 0) {
    PosBaseX   = (UINT32)BaseX;
    PosOffsetX = OffsetX;
  } else {
    ASSERT (BaseX + OffsetX + Width >= 0);

    PosBaseX = 0;

    if (OffsetX - (-BaseX) >= 0) {
      PosOffsetX = (UINT32)(OffsetX - (-BaseX));
    } else {
      PosOffsetX = 0;
      Width      = (UINT32)(Width - (-(OffsetX - (-BaseX))));
    }
  }

  if (BaseY >= 0) {
    PosBaseY   = (UINT32)BaseY;
    PosOffsetY = OffsetY;
  } else {
    ASSERT (BaseY + OffsetY + Height >= 0);

    PosBaseY = 0;

    if (OffsetY - (-BaseY) >= 0) {
      PosOffsetY = (UINT32)(OffsetY - (-BaseY));
    } else {
      PosOffsetY = 0;
      Height     = (UINT32)(Height - (-(OffsetY - (-BaseY))));
    }
  }

  if (!Fill) {
    ASSERT (Image->Width  > OffsetX);
    ASSERT (Image->Height > OffsetY);
    //
    // Only crop to the image's dimensions when not using fill-drawing.
    //
    Width  = MIN (Width,  Image->Width  - OffsetX);
    Height = MIN (Height, Image->Height - OffsetY);
  }
  //
  // Crop to the screen's dimensions.
  //
  ASSERT (DrawContext->Screen->Width  >= PosBaseX + PosOffsetX);
  ASSERT (DrawContext->Screen->Height >= PosBaseY + PosOffsetY);
  Width  = MIN (Width,  DrawContext->Screen->Width  - (PosBaseX + PosOffsetX));
  Height = MIN (Height, DrawContext->Screen->Height - (PosBaseY + PosOffsetY));

  if (Width == 0 || Height == 0) {
    return;
  }

  ASSERT (Image->Buffer != NULL);

  if (!Fill) {
    //
    // Iterate over each row of the request.
    //
    for (
      RowIndex = 0,
        SourceRowOffset = OffsetY * Image->Width,
        TargetRowOffset = (PosBaseY + PosOffsetY) * DrawContext->Screen->Width;
      RowIndex < Height;
      ++RowIndex,
        SourceRowOffset += Image->Width,
        TargetRowOffset += DrawContext->Screen->Width
      ) {
      //
      // Blend the row pixel-by-pixel.
      //
      for (
        TargetColumnOffset = PosOffsetX, SourceColumnOffset = OffsetX;
        TargetColumnOffset < PosOffsetX + Width;
        ++TargetColumnOffset, ++SourceColumnOffset
        ) {
        TargetPixel = &mScreenBuffer[TargetRowOffset + PosBaseX + TargetColumnOffset];
        SourcePixel = &Image->Buffer[SourceRowOffset + SourceColumnOffset];
        GuiBlendPixel (TargetPixel, SourcePixel, Opacity);
      }
    }
  } else {
    //
    // Iterate over each row of the request.
    //
    for (
      RowIndex = 0,
        TargetRowOffset = (PosBaseY + PosOffsetY) * DrawContext->Screen->Width;
      RowIndex < Height;
      ++RowIndex,
        TargetRowOffset += DrawContext->Screen->Width
      ) {
      //
      // Blend the row pixel-by-pixel with Source's (0,0).
      //
      for (
        TargetColumnOffset = PosOffsetX;
        TargetColumnOffset < PosOffsetX + Width;
        ++TargetColumnOffset
        ) {
        TargetPixel = &mScreenBuffer[TargetRowOffset + PosBaseX + TargetColumnOffset];
        GuiBlendPixel (TargetPixel, &Image->Buffer[0], Opacity);
      }
    }
  }

  if (RequestDraw) {
    //
    // Update the coordinates of the smallest rectangle covering all changes.
    //
    ThisReq.MinX = PosBaseX + PosOffsetX;
    ThisReq.MinY = PosBaseY + PosOffsetY;
    ThisReq.MaxX = PosBaseX + PosOffsetX + Width  - 1;
    ThisReq.MaxY = PosBaseY + PosOffsetY + Height - 1;

    ThisArea = Width * Height;

    for (Index = 0; Index < mNumValidDrawReqs; ++Index) {
      //
      // Calculate several dimensions to determine whether to merge the two
      // draw requests for improved flushing performance.
      //
      ReqWidth  = mDrawRequests[Index].MaxX - mDrawRequests[Index].MinX + 1;
      ReqHeight = mDrawRequests[Index].MaxY - mDrawRequests[Index].MinY + 1;
      ReqArea   = ReqWidth * ReqHeight;

      if (mDrawRequests[Index].MinX < ThisReq.MinX) {
        CombMinX = mDrawRequests[Index].MinX;
        OverMinX = ThisReq.MinX;
      } else {
        CombMinX = ThisReq.MinX;
        OverMinX = mDrawRequests[Index].MinX;
      }

      if (mDrawRequests[Index].MaxX > ThisReq.MaxX) {
        CombMaxX = mDrawRequests[Index].MaxX;
        OverMaxX = ThisReq.MaxX;
      } else {
        CombMaxX = ThisReq.MaxX;
        OverMaxX = mDrawRequests[Index].MaxX;
      }

      if (mDrawRequests[Index].MinY < ThisReq.MinY) {
        CombMinY = mDrawRequests[Index].MinY;
        OverMinY = ThisReq.MinY;
      } else {
        CombMinY = ThisReq.MinY;
        OverMinY = mDrawRequests[Index].MinY;
      }

      if (mDrawRequests[Index].MaxY > ThisReq.MaxY) {
        CombMaxY = mDrawRequests[Index].MaxY;
        OverMaxY = ThisReq.MaxY;
      } else {
        CombMaxY = ThisReq.MaxY;
        OverMaxY = mDrawRequests[Index].MaxY;
      }

      CombWidth  = CombMaxX - CombMinX + 1;
      CombHeight = CombMaxY - CombMinY + 1;
      CombArea   = CombWidth * CombHeight;
     
      OverArea = 0;
      if (OverMinX <= OverMaxX && OverMinY <= OverMaxY) {
        OverWidth  = OverMaxX - OverMinX + 1;
        OverHeight = OverMaxY - OverMinY + 1;
        OverArea   = OverWidth * OverHeight;
      }

      ActualArea = ThisArea + ReqArea - OverArea;
      //
      // Two requests are merged when their combined actual draw area is at
      // least 3/4 of the area needed to draw both at once.
      //
      if (4 * ActualArea >= 3 * CombArea) {
        mDrawRequests[Index].MinX = CombMinX;
        mDrawRequests[Index].MaxX = CombMaxX;
        mDrawRequests[Index].MinY = CombMinY;
        mDrawRequests[Index].MaxY = CombMaxY;
        return;
      }
    }

    if (mNumValidDrawReqs >= ARRAY_SIZE (mDrawRequests)) {
      ASSERT (FALSE);
      return;
    }

    CopyMem (&mDrawRequests[mNumValidDrawReqs], &ThisReq, sizeof (ThisReq));
    ++mNumValidDrawReqs;
  }
}

VOID
GuiDrawScreen (
  IN OUT GUI_DRAWING_CONTEXT  *DrawContext,
  IN     INT64                X,
  IN     INT64                Y,
  IN     UINT32               Width,
  IN     UINT32               Height,
  IN     BOOLEAN              RequestDraw
  )
{
  UINT32 PosX;
  UINT32 PosY;

  ASSERT (DrawContext != NULL);
  ASSERT (DrawContext->Screen != NULL);
  //
  // Only draw the onscreen parts.
  //
  if (X >= 0) {
    PosX = (UINT32)X;
  } else {
    if (X + Width <= 0) {
      return;
    }

    Width = (UINT32)(Width - (-X));
    PosX  = 0;
  }

  if (Y >= 0) {
    PosY = (UINT32)Y;
  } else {
    if (Y + Height <= 0) {
      return;
    }

    Height = (UINT32)(Height - (-Y));
    PosY  = 0;
  }

  if (PosX >= DrawContext->Screen->Width
   || PosY >= DrawContext->Screen->Height) {
    return;
  }

  Width  = MIN (Width,  DrawContext->Screen->Width  - PosX);
  Height = MIN (Height, DrawContext->Screen->Height - PosY);

  if (Width == 0 || Height == 0) {
    return;
  }

  ASSERT (DrawContext->Screen->OffsetX == 0);
  ASSERT (DrawContext->Screen->OffsetY == 0);
  ASSERT (DrawContext->Screen->Draw != NULL);
  DrawContext->Screen->Draw (
                         DrawContext->Screen,
                         DrawContext,
                         DrawContext->GuiContext,
                         0,
                         0,
                         PosX,
                         PosY,
                         Width,
                         Height,
                         RequestDraw
                         );
}

VOID
GuiRedrawObject (
  IN OUT GUI_OBJ              *This,
  IN OUT GUI_DRAWING_CONTEXT  *DrawContext,
  IN     INT64                BaseX,
  IN     INT64                BaseY,
  IN     BOOLEAN              RequestDraw
  )
{
  ASSERT (This != NULL);
  ASSERT (DrawContext != NULL);

  GuiDrawScreen (
    DrawContext,
    BaseX,
    BaseY,
    This->Width,
    This->Height,
    RequestDraw
    );
}

VOID
GuiRedrawPointer (
  IN OUT GUI_DRAWING_CONTEXT  *DrawContext
  )
{
  STATIC UINT32          CursorOldX      = 0;
  STATIC UINT32          CursorOldY      = 0;
  STATIC UINT32          CursorOldWidth  = 0;
  STATIC UINT32          CursorOldHeight = 0;
  STATIC CONST GUI_IMAGE *CursorOldImage = NULL;

  CONST GUI_IMAGE *CursorImage;
  BOOLEAN         RequestDraw;
  UINT32          MinX;
  UINT32          DeltaX;
  UINT32          MinY;
  UINT32          DeltaY;

  ASSERT (DrawContext != NULL);

  ASSERT (DrawContext->GetCursorImage != NULL);
  CursorImage = DrawContext->GetCursorImage (
                               &mScreenViewCursor,
                               DrawContext->GuiContext
                               );
  ASSERT (CursorImage != NULL);

  RequestDraw = FALSE;

  if (mScreenViewCursor.X != CursorOldX || mScreenViewCursor.Y != CursorOldY) {
    //
    // Redraw the cursor when it has been moved.
    //
    RequestDraw = TRUE;
  } else if (CursorImage != CursorOldImage) {
    //
    // Redraw the cursor if its image has changed.
    //
    RequestDraw = TRUE;
  } else if (mNumValidDrawReqs == 0) {
    //
    // Redraw the cursor if nothing else is drawn to always invoke GOP for a
    // more consistent framerate.
    //
    RequestDraw = TRUE;
  }
  //
  // Always drawing the cursor to the buffer increases consistency and is less
  // error-prone to situational hiding.
  //
  // Restore the rectangle previously covered by the cursor.
  // Cover the area of the new cursor too and do not request a draw of the new
  // cursor to not need to merge the requests later.
  //
  if (CursorOldX < mScreenViewCursor.X) {
    MinX   = CursorOldX;
    DeltaX = mScreenViewCursor.X - CursorOldX;
  } else {
    MinX   = mScreenViewCursor.X;
    DeltaX = CursorOldX - mScreenViewCursor.X;
  }

  if (CursorOldY < mScreenViewCursor.Y) {
    MinY   = CursorOldY;
    DeltaY = mScreenViewCursor.Y - CursorOldY;
  } else {
    MinY   = mScreenViewCursor.Y;
    DeltaY = CursorOldY - mScreenViewCursor.Y;
  }

  GuiDrawScreen (
    DrawContext,
    MinX,
    MinY,
    MAX (CursorOldWidth,  CursorImage->Width)  + DeltaX,
    MAX (CursorOldHeight, CursorImage->Height) + DeltaY,
    RequestDraw
    );
  GuiDrawToBuffer (
    CursorImage,
    0xFF,
    FALSE,
    DrawContext,
    mScreenViewCursor.X,
    mScreenViewCursor.Y,
    0,
    0,
    CursorImage->Width,
    CursorImage->Height,
    FALSE
    );

  if (RequestDraw) {
    CursorOldX      = mScreenViewCursor.X;
    CursorOldY      = mScreenViewCursor.Y;
    CursorOldWidth  = CursorImage->Width;
    CursorOldHeight = CursorImage->Height;
    CursorOldImage  = CursorImage;
  } else {
    ASSERT (CursorOldX      == mScreenViewCursor.X);
    ASSERT (CursorOldY      == mScreenViewCursor.Y);
    ASSERT (CursorOldWidth  == CursorImage->Width);
    ASSERT (CursorOldHeight == CursorImage->Height);
    ASSERT (CursorOldImage  == CursorImage);
  }
}

/**
  Stalls the CPU for at least the given number of ticks.

  Stalls the CPU for at least the given number of ticks. It's invoked by
  MicroSecondDelay() and NanoSecondDelay().

  @param  Delay     A period of time to delay in ticks.

**/
STATIC
UINT64
InternalCpuDelayTsc (
  IN UINT64  Delay
  )
{
  UINT64  Ticks;
  UINT64  Tsc;

  //
  // The target timer count is calculated here
  //
  Ticks = AsmReadTsc () + Delay;

  //
  // Wait until time out
  // Timer wrap-arounds are NOT handled correctly by this function.
  // Thus, this function must be called within 10 years of reset since
  // Intel guarantees a minimum of 10 years before the TSC wraps.
  //
  while ((Tsc = AsmReadTsc ()) < Ticks) {
    CpuPause ();
  }

  return Tsc;
}

VOID
GuiFlushScreen (
  IN OUT GUI_DRAWING_CONTEXT  *DrawContext
  )
{
  EFI_TPL OldTpl;

  UINTN   NumValidDrawReqs;
  UINTN   Index;

  UINT64  EndTsc;
  UINT64  DeltaTsc;

  BOOLEAN Interrupts;

  ASSERT (DrawContext != NULL);
  ASSERT (DrawContext->Screen != NULL);

  GuiRedrawPointer (DrawContext);

  NumValidDrawReqs = mNumValidDrawReqs;
  ASSERT (NumValidDrawReqs <= ARRAY_SIZE (mDrawRequests));

  mNumValidDrawReqs = 0;

  for (Index = 0; Index < NumValidDrawReqs; ++Index) {
    ASSERT (mDrawRequests[Index].MaxX >= mDrawRequests[Index].MinX);
    ASSERT (mDrawRequests[Index].MaxY >= mDrawRequests[Index].MinY);
    //
    // Set MaxX/Y to Width and Height as the requests are invalidated anyway.
    //
    mDrawRequests[Index].MaxX -= mDrawRequests[Index].MinX - 1;
    mDrawRequests[Index].MaxY -= mDrawRequests[Index].MinY - 1;
  }
  //
  // Raise the TPL to not interrupt timing or flushing.
  //
  OldTpl     = gBS->RaiseTPL (TPL_NOTIFY);
  Interrupts = GuiSaveAndDisableInterrupts ();

  EndTsc   = AsmReadTsc ();
  DeltaTsc = EndTsc - mStartTsc;
  if (DeltaTsc < mDeltaTscTarget) {
    EndTsc = InternalCpuDelayTsc (mDeltaTscTarget - DeltaTsc);
  }

  for (Index = 0; Index < NumValidDrawReqs; ++Index) {
    //
    // Due to above's loop, MaxX/Y correspond to Width and Height here.
    //
    GuiOutputBlt (
      mOutputContext,
      mScreenBuffer,
      EfiBltBufferToVideo,
      mDrawRequests[Index].MinX,
      mDrawRequests[Index].MinY,
      mDrawRequests[Index].MinX,
      mDrawRequests[Index].MinY,
      mDrawRequests[Index].MaxX,
      mDrawRequests[Index].MaxY,
      mScreenBufferDelta
      );
  }

  if (Interrupts) {
    GuiEnableInterrupts ();
  }
  gBS->RestoreTPL (OldTpl);
  //
  // Explicitly include BLT time in the timing calculation.
  // FIXME: GOP takes inconsistently long depending on dimensions.
  //
  mStartTsc = EndTsc;
}

VOID
GuiRedrawAndFlushScreen (
  IN OUT GUI_DRAWING_CONTEXT  *DrawContext
  )
{
  ASSERT (DrawContext != NULL);
  ASSERT (DrawContext->Screen != NULL);

  mStartTsc = AsmReadTsc ();

  GuiRedrawObject (DrawContext->Screen, DrawContext, 0, 0, TRUE);
  GuiFlushScreen (DrawContext);
}

// TODO: move
UINT64
GuiGetTSCFrequency (
  VOID
  );

EFI_STATUS
GuiLibConstruct (
  IN UINT32  CursorDefaultX,
  IN UINT32  CursorDefaultY
  )
{
  CONST EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *OutputInfo;

  mOutputContext = GuiOutputConstruct ();
  if (mOutputContext == NULL) {
    DEBUG ((DEBUG_ERROR, "Failed to initialise output\n"));
    return EFI_UNSUPPORTED;
  }

  OutputInfo = GuiOutputGetInfo (mOutputContext);
  ASSERT (OutputInfo != NULL);

  CursorDefaultX = MIN (CursorDefaultX, OutputInfo->HorizontalResolution - 1);
  CursorDefaultY = MIN (CursorDefaultY, OutputInfo->VerticalResolution   - 1);

  mPointerContext = GuiPointerConstruct (
                      CursorDefaultX,
                      CursorDefaultY,
                      OutputInfo->HorizontalResolution,
                      OutputInfo->VerticalResolution
                      );
  if (mPointerContext == NULL) {
    DEBUG ((DEBUG_WARN, "Failed to initialise pointer\n"));
  }

  mKeyContext = GuiKeyConstruct ();
  if (mKeyContext == NULL) {
    DEBUG ((DEBUG_WARN, "Failed to initialise key input\n"));
  }

  if (mPointerContext == NULL && mKeyContext == NULL) {
    GuiLibDestruct ();
    return EFI_UNSUPPORTED;
  }

  mScreenBufferDelta = OutputInfo->HorizontalResolution * sizeof (*mScreenBuffer);
  mScreenBuffer      = AllocatePool (OutputInfo->VerticalResolution * mScreenBufferDelta);
  if (mScreenBuffer == NULL) {
    DEBUG ((DEBUG_ERROR, "GUI alloc failure\n"));
    GuiLibDestruct ();
    return EFI_OUT_OF_RESOURCES;
  }

  GuiMtrrSetMemoryAttribute (
    (EFI_PHYSICAL_ADDRESS)mScreenBuffer,
    mScreenBufferDelta * OutputInfo->VerticalResolution,
    CacheWriteBack
    );

  mDeltaTscTarget =  DivU64x32 (GuiGetTSCFrequency (), 60);

  mScreenViewCursor.X = CursorDefaultX;
  mScreenViewCursor.Y = CursorDefaultY;

  return EFI_SUCCESS;
}

VOID
GuiLibDestruct (
  VOID
  )
{
  if (mOutputContext != NULL) {
    GuiOutputDestruct (mOutputContext);
    mOutputContext = NULL;
  }

  if (mPointerContext != NULL) {
    GuiPointerDestruct (mPointerContext);
    mPointerContext = NULL;
  }

  if (mKeyContext != NULL) {
    GuiKeyDestruct (mKeyContext);
    mKeyContext = NULL;
  }
}

VOID
GuiViewInitialize (
  OUT    GUI_DRAWING_CONTEXT   *DrawContext,
  IN OUT GUI_OBJ               *Screen,
  IN     GUI_CURSOR_GET_IMAGE  GetCursorImage,
  IN     GUI_EXIT_LOOP         ExitLoop,
  IN     VOID                  *GuiContext
  )
{
  CONST EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *OutputInfo;

  ASSERT (DrawContext != NULL);
  ASSERT (Screen != NULL);
  ASSERT (GetCursorImage != NULL);
  ASSERT (ExitLoop != NULL);

  OutputInfo = GuiOutputGetInfo (mOutputContext);
  ASSERT (OutputInfo != NULL);

  Screen->Width  = OutputInfo->HorizontalResolution;
  Screen->Height = OutputInfo->VerticalResolution;

  DrawContext->Screen         = Screen;
  DrawContext->GetCursorImage = GetCursorImage;
  DrawContext->ExitLoop       = ExitLoop;
  DrawContext->GuiContext     = GuiContext;
  InitializeListHead (&DrawContext->Animations);
}

VOID
GuiGetBaseCoords (
  IN  GUI_OBJ              *This,
  IN  GUI_DRAWING_CONTEXT  *DrawContext,
  OUT INT64                *BaseX,
  OUT INT64                *BaseY
  )
{
  GUI_OBJ       *Obj;
  GUI_OBJ_CHILD *ChildObj;
  INT64         X;
  INT64         Y;

  ASSERT (This != NULL);
  ASSERT (DrawContext != NULL);
  ASSERT (DrawContext->Screen->OffsetX == 0);
  ASSERT (DrawContext->Screen->OffsetY == 0);
  ASSERT (BaseX != NULL);
  ASSERT (BaseY != NULL);

  X   = 0;
  Y   = 0;
  Obj = This;
  while (Obj != DrawContext->Screen) {
    X += Obj->OffsetX;
    Y += Obj->OffsetY;

    ChildObj = BASE_CR (Obj, GUI_OBJ_CHILD, Obj);
    Obj      = ChildObj->Parent;
    ASSERT (Obj != NULL);
    ASSERT (IsNodeInList (&Obj->Children, &ChildObj->Link));
  }

  *BaseX = X;
  *BaseY = Y;
}

VOID
GuiDrawLoop (
  IN OUT GUI_DRAWING_CONTEXT  *DrawContext
  )
{
  EFI_STATUS          Status;
  BOOLEAN             Result;

  EFI_INPUT_KEY       InputKey;
  GUI_POINTER_STATE   PointerState;
  GUI_OBJ             *HoldObject;
  INT64               HoldObjBaseX;
  INT64               HoldObjBaseY;

  CONST LIST_ENTRY    *AnimEntry;
  CONST GUI_ANIMATION *Animation;

  ASSERT (DrawContext != NULL);

  mNumValidDrawReqs = 0;
  HoldObject        = NULL;

  GuiRedrawAndFlushScreen (DrawContext);
  //
  // Clear previous inputs.
  //
  GuiPointerReset (mPointerContext);
  GuiKeyReset (mKeyContext);
  //
  // Main drawing loop, time and derieve sub-frequencies as required.
  //
  mStartTsc = AsmReadTsc ();
  do {
    //UINT64 StartTsc = AsmReadTsc ();
    if (mPointerContext != NULL) {
      //
      // Process pointer events.
      //
      Status = GuiPointerGetState (mPointerContext, &PointerState);
      if (!EFI_ERROR (Status)) {
        mScreenViewCursor.X = PointerState.X;
        mScreenViewCursor.Y = PointerState.Y;

        if (HoldObject == NULL && PointerState.PrimaryDown) {
          HoldObject = GuiObjDelegatePtrEvent (
                          DrawContext->Screen,
                          DrawContext,
                          DrawContext->GuiContext,
                          GuiPointerPrimaryDown,
                          0,
                          0,
                          PointerState.X,
                          PointerState.Y
                          );
        }
      }

      if (HoldObject != NULL) {
        GuiGetBaseCoords (
          HoldObject,
          DrawContext,
          &HoldObjBaseX,
          &HoldObjBaseY
          );
        HoldObject->PtrEvent (
                      HoldObject,
                      DrawContext,
                      DrawContext->GuiContext,
                      !PointerState.PrimaryDown ? GuiPointerPrimaryUp : GuiPointerPrimaryHold,
                      HoldObjBaseX,
                      HoldObjBaseY,
                      (INT64)PointerState.X - HoldObjBaseX,
                      (INT64)PointerState.Y - HoldObjBaseY
                      );
        if (!PointerState.PrimaryDown) {
          HoldObject = NULL;
        }
      }
    }

    if (mKeyContext != NULL) {
      //
      // Process key events. Only allow one key at a time for now.
      //
      Status = GuiKeyRead (mKeyContext, &InputKey);
      if (!EFI_ERROR (Status)) {
        ASSERT (DrawContext->Screen->KeyEvent != NULL);
        DrawContext->Screen->KeyEvent (
                               DrawContext->Screen,
                               DrawContext,
                               DrawContext->GuiContext,
                               0,
                               0,
                               &InputKey
                               );
        //
        // HACK: MSVC complains about unreachable code.
        //
        if (Status != EFI_SUCCESS) {
          return;
        }
      }
    }

    STATIC UINT64 FrameTime = 0;
    //
    // Process queued animations.
    //
    AnimEntry = GetFirstNode (&DrawContext->Animations);
    while (!IsNull (&DrawContext->Animations, AnimEntry)) {
      Animation = BASE_CR (AnimEntry, GUI_ANIMATION, Link);
      Result = Animation->Animate (Animation->Context, DrawContext, FrameTime);

      AnimEntry = GetNextNode (&DrawContext->Animations, AnimEntry);

      if (Result) {
        RemoveEntryList (&Animation->Link);
      }
    }
    ++FrameTime;
    //
    // Flush the changes performed in this refresh iteration.
    //
    GuiFlushScreen (DrawContext);

    //UINT64 EndTsc = AsmReadTsc ();
    //DEBUG ((DEBUG_ERROR, "Loop delta TSC: %lld, target: %lld\n", EndTsc - StartTsc, mDeltaTscTarget));
  } while (!DrawContext->ExitLoop (DrawContext->GuiContext));
}

RETURN_STATUS
GuiPngToImage (
  IN OUT GUI_IMAGE  *Image,
  IN     VOID       *BmpImage,
  IN     UINTN      BmpImageSize
  )
{
  EFI_STATUS                       Status;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *BufferWalker;
  UINTN                            Index;
  UINT8                            TmpChannel;

  Status = DecodePng (
               BmpImage,
               BmpImageSize,
               (VOID **) &Image->Buffer,
               &Image->Width,
               &Image->Height,
               NULL
              );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCUI: DecodePNG...%r\n", Status));
    return Status;
  }

  BufferWalker = Image->Buffer;
  for (Index = 0; Index < (UINTN) Image->Width * Image->Height; ++Index) {
    TmpChannel             = (UINT8) ((BufferWalker->Blue * BufferWalker->Reserved) / 0xFF);
    BufferWalker->Blue     = (UINT8) ((BufferWalker->Red * BufferWalker->Reserved) / 0xFF);
    BufferWalker->Green    = (UINT8) ((BufferWalker->Green * BufferWalker->Reserved) / 0xFF);
    BufferWalker->Red      = TmpChannel;
    ++BufferWalker;
  }

  return RETURN_SUCCESS;
}

RETURN_STATUS
GuiCreateHighlightedImage (
  OUT GUI_IMAGE                            *SelectedImage,
  IN  CONST GUI_IMAGE                      *SourceImage,
  IN  CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *HighlightPixel
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL PremulPixel;

  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Buffer;
  UINT32                        ColumnOffset;
  BOOLEAN                       OneSet;
  UINT32                        FirstUnsetX;
  UINT32                        IndexUnsetX;
  UINT32                        IndexY;
  UINT32                        RowOffset;

  ASSERT (SelectedImage != NULL);
  ASSERT (SourceImage != NULL);
  ASSERT (SourceImage->Buffer != NULL);
  ASSERT (HighlightPixel != NULL);
  //
  // The multiplication cannot wrap around because the original allocation sane.
  //
  Buffer = AllocateCopyPool (
             SourceImage->Width * SourceImage->Height * sizeof (*SourceImage->Buffer),
             SourceImage->Buffer
             );
  if (Buffer == NULL) {
    return RETURN_OUT_OF_RESOURCES;
  }

  PremulPixel.Blue     = (UINT8)((HighlightPixel->Blue  * HighlightPixel->Reserved) / 0xFF);
  PremulPixel.Green    = (UINT8)((HighlightPixel->Green * HighlightPixel->Reserved) / 0xFF);
  PremulPixel.Red      = (UINT8)((HighlightPixel->Red   * HighlightPixel->Reserved) / 0xFF);
  PremulPixel.Reserved = HighlightPixel->Reserved;

  for (
    IndexY = 0, RowOffset = 0;
    IndexY < SourceImage->Height;
    ++IndexY, RowOffset += SourceImage->Width
    ) {
    FirstUnsetX = 0;
    OneSet      = FALSE;

    for (ColumnOffset = 0; ColumnOffset < SourceImage->Width; ++ColumnOffset) {
      if (SourceImage->Buffer[RowOffset + ColumnOffset].Reserved != 0) {
        OneSet = TRUE;
        GuiBlendPixel (
          &Buffer[RowOffset + ColumnOffset],
          &PremulPixel,
          0xFF
          );
        if (FirstUnsetX != 0) {
          //
          // Set all fully transparent pixels between two not fully transparent
          // pixels to the highlighter pixel.
          //
          for (
            IndexUnsetX = FirstUnsetX;
            FirstUnsetX < ColumnOffset;
            ++FirstUnsetX
            ) {
            CopyMem (
              &Buffer[RowOffset + FirstUnsetX],
              &PremulPixel,
              sizeof (*Buffer)
              );
          }

          FirstUnsetX = 0;
        }
      } else if (FirstUnsetX == 0 && OneSet) {
        FirstUnsetX = ColumnOffset;
      }
    }
  }

  SelectedImage->Width  = SourceImage->Width;
  SelectedImage->Height = SourceImage->Height;
  SelectedImage->Buffer = Buffer;
  return RETURN_SUCCESS;
}

/// A sine approximation via a third-order approx.
/// @param x    Angle (with 2^15 units/circle)
/// @return     Sine value (Q12)
STATIC
INT32
isin_S3 (
  IN INT32  x
  )
{
// S(x) = x * ( (3<<p) - (x*x>>r) ) >> s
// n : Q-pos for quarter circle             13
// A : Q-pos for output                     12
// p : Q-pos for parentheses intermediate   15
// r = 2n-p                                 11
// s = A-1-p-n                              17

#define n  13
#define A  12
#define p  15
#define r  ((2 * n) - p)
#define s  (n + p + 1 - A)

  x = x << (30 - n); // shift to full s32 range (Q13->Q30)

  if ((x ^ (x << 1)) < 0) // test for quadrant 1 or 2
    x = (1 << 31) - x;

  x = x >> (30 - n);

  return x * ((3 << p) - (x * x >> r)) >> s;

#undef n
#undef A
#undef p
#undef r
#undef s
}

#define INTERPOL_FP_TIME_FACTOR  (1U << 12U)

UINT32
GuiGetInterpolatedValue (
  IN CONST GUI_INTERPOLATION  *Interpol,
  IN       UINT64             CurrentTime
  )
{
  INT32  AnimTime;
  UINT32 DeltaTime;

  ASSERT (Interpol != NULL);
  ASSERT (Interpol->StartTime <= CurrentTime);
  ASSERT (Interpol->Duration > 0);

  if (CurrentTime == Interpol->StartTime) {
    return Interpol->StartValue;
  }

  DeltaTime = (UINT32)(CurrentTime - Interpol->StartTime);

  if (DeltaTime >= Interpol->Duration) {
    return Interpol->EndValue;
  }

  AnimTime = (INT32) DivU64x32 ((UINT64) INTERPOL_FP_TIME_FACTOR * DeltaTime, Interpol->Duration);
  if (Interpol->Type == GuiInterpolTypeSmooth) {
    //
    // One INTERPOL_FP_TIME_FACTOR unit corresponds to 45 degrees in the unit circle. Divide
    // the time by two because the integral of sin from 0 to Pi is equal to 2,
    // i.e. double speed.
    //
    AnimTime = isin_S3 (4 * AnimTime / 2);
    //
    // FP-square to further smoothen the animation.
    //
    AnimTime = (AnimTime * AnimTime) / INTERPOL_FP_TIME_FACTOR;
  } else {
    ASSERT (Interpol->Type == GuiInterpolTypeLinear);
  }

  return (Interpol->EndValue * AnimTime
    + (Interpol->StartValue * (INTERPOL_FP_TIME_FACTOR - AnimTime)))
    / INTERPOL_FP_TIME_FACTOR;
}

RETURN_STATUS
GuiPngToClickImage (
  IN OUT GUI_CLICK_IMAGE                      *Image,
  IN     VOID                                 *BmpImage,
  IN     UINTN                                BmpImageSize,
  IN     CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *HighlightPixel
  )
{
  RETURN_STATUS Status;

  ASSERT (Image != NULL);
  ASSERT (BmpImage != NULL);
  ASSERT (HighlightPixel != NULL);

  Status = GuiPngToImage (&Image->BaseImage, BmpImage, BmpImageSize);
  if (RETURN_ERROR (Status)) {
    return Status;
  }

  Status = GuiCreateHighlightedImage (
             &Image->HoldImage,
             &Image->BaseImage,
             HighlightPixel
             );
  if (RETURN_ERROR (Status)) {
    FreePool (Image->BaseImage.Buffer);
    Image->BaseImage.Buffer = NULL;
    Image->HoldImage.Buffer = NULL;
    return Status;
  }

  return Status;
}