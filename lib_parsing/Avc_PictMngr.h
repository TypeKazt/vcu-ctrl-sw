/******************************************************************************
*
* Copyright (C) 2017 Allegro DVT2.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* Use of the Software is limited solely to applications:
* (a) running on a Xilinx device, or
* (b) that interact with a Xilinx device through a bus or interconnect.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* XILINX OR ALLEGRO DVT2 BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
* OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
* Except as contained in this notice, the name of  Xilinx shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in
* this Software without prior written authorization from Xilinx.
*
*
* Except as contained in this notice, the name of Allegro DVT2 shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in
* this Software without prior written authorization from Allegro DVT2.
*
******************************************************************************/

/****************************************************************************
   -----------------------------------------------------------------------------
 **************************************************************************//*!
   \addtogroup lib_decode_hls
   @{
   \file
 *****************************************************************************/

#pragma once

#include "I_PictMngr.h"
#include "lib_common/SliceHeader.h"
#include "lib_common_dec/DecPicParam.h"

/*************************************************************************//*!
   \brief Sets the POC of the current decoded frame
   \param[in] pCtx   Pointer to a Picture manager context object
   \param[in] pSlice slice header of the current decoded slice
*****************************************************************************/
bool AL_AVC_PictMngr_SetCurrentPOC(AL_TPictMngrCtx* pCtx, AL_TAvcSliceHdr* pSlice);

/*************************************************************************//*!
   \brief This function updates the reconstructed resolution information
   \param[in] pCtx Pointer to a Picture manager context object
   \param[in] pSPS Pointer to a ACV SPS structure
*****************************************************************************/
void AL_AVC_PictMngr_UpdateRecInfo(AL_TPictMngrCtx* pCtx, AL_TAvcSps const* pSPS);

/*************************************************************************//*!
   \brief This function updates the Picture Manager context each time a picture have been parsed.
   \param[in] pCtx            Pointer to a Picture manager context object
   \param[in] bClearRef       Specifies if the reference pool picture is cleared
   \param[in] eMarkingFlag    Reference status of the current picture
*****************************************************************************/
void AL_AVC_PictMngr_EndParsing(AL_TPictMngrCtx* pCtx, bool bClearRef, AL_EMarkingRef eMarkingFlag);

/*************************************************************************//*!
   \brief This function clean the DPB from unwanted pictures
   \param[in] pCtx            Pointer to a Picture manager context object
*****************************************************************************/
void AL_AVC_PictMngr_CleanDPB(AL_TPictMngrCtx* pCtx);

/*************************************************************************//*!
   \brief Retrieves all buffers (input and output) required to decode the current slice
   \param[in]  pCtx          Pointer to a Picture manager context object
   \param[in]  pPP           Pointer to the current picture parameters
   \param[in]  pSP           Pointer to the current slice parameters
   \param[in]  pSlice        Pointer to the slice header of the current slice
   \param[in]  pListRef      Pointer to the current picture reference lists
   \param[out] pListAddr     Pointer to the buffer that will receive the references, colocated POC and colocated motion vectors address list
   \param[out] ppPOC         Receives pointer to the POC buffer where
                          reference Pictures order count are stored.
   \param[out] ppMV          Receives pointer to the MV buffer where
                          Motion Vectors should be stored.
   \param[out] pWP           Receives slices Weighted Pred tables
   \param[out] ppRec         Receives pointer to the frame buffer where reconstructed picture should be stored.
   \param[in] eFBStorageMode the way frame buffer is stored
   \return If the function succeeds the return value is nonzero (true)
        If the function fails the return value is zero (false)
*****************************************************************************/
bool AL_AVC_PictMngr_GetBuffers(AL_TPictMngrCtx* pCtx, AL_TDecPicParam* pPP, AL_TDecSliceParam* pSP, AL_TAvcSliceHdr* pSlice, TBufferListRef* pListRef, TBuffer* pListAddr, TBufferPOC** ppPOC, TBufferMV** ppMV, TBuffer* pWP, AL_TBuffer** ppRec);

/*************************************************************************//*!
   \brief Initializes the reference picture list for the current slice
   \param[in]  pCtx     Pointer to a Picture manager context object
   \param[in]  pSlice   Current slice header
   \param[out] pListRef Receives the reference list of the current slice
*****************************************************************************/
void AL_AVC_PictMngr_InitPictList(AL_TPictMngrCtx* pCtx, AL_TAvcSliceHdr* pSlice, TBufferListRef* pListRef);

/*************************************************************************//*!
   \brief Initializes fill Gap in Frame num
   \param[in]  pCtx     Pointer to a Picture manager context object
   \param[in]  pSlice   Current slice header
*****************************************************************************/
void AL_AVC_PictMngr_Fill_Gap_In_FrameNum(AL_TPictMngrCtx* pCtx, AL_TAvcSliceHdr* pSlice);

/*************************************************************************//*!
   \brief Reorders the reference picture list of the current slice
   \param[in]     pCtx     Pointer to a Picture manager context object
   \param[in]     pSlice   Current slice header
   \param[in,out] pListRef Receives the modified reference list of the current slice
*****************************************************************************/
void AL_AVC_PictMngr_ReorderPictList(AL_TPictMngrCtx* pCtx, AL_TAvcSliceHdr* pSlice, TBufferListRef* pListRef);

/*@}*/

