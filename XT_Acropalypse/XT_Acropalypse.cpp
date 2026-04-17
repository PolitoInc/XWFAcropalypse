#include "pch.h"

UINT64 g_totalImageCount = 0;
UINT64 g_pngImageCount = 0;
UINT64 g_jpgImageCount = 0;
UINT64 g_acropalypseImageCount = 0;
CRITICAL_SECTION counterCriticalSection;

LONG __stdcall XT_Init(DWORD info, DWORD nFlags, HANDLE hMainWnd, void* lpReserved) {
	if (XT_RetrieveFunctionPointers() != 0) {
		MessageBox((HWND)hMainWnd, 
			L"XT_Acropolypse Error: Missing function pointers.", 
			L"X-Tension Error", 
			MB_OK);

		return -1;
	}

	// Critical section used for updating counter values g_totalImageCount and g_acropalypseImageCount
	InitializeCriticalSection(&counterCriticalSection);

	return 2;
}

LONG __stdcall XT_About(HANDLE hParentWnd, void* lpReserved) {
	MessageBox((HWND)hParentWnd, L"XT_Acropolypse X-Tension, Copyright 2023, Polito, Inc.",
		L"XT_Acropolypse", MB_OK);
	return 0;
}

LONG __stdcall XT_Prepare(HANDLE hVolume, HANDLE hEvidence, DWORD nOpType, void* lpReserved) {
	// TODO: Check X-Ways Version. We need 18.8 or later to run
	XWF_OutputMessage(L"XT_Acropalypse preparing to process JPG/PNG images...", 0);
	return XT_PREPARE_CALLPILATE;
}

// Parse PNG chunks; we assume that the caller validates that this is an actual PNG file
// Based on code from https://gist.github.com/DavidBuchanan314/93de9d07f7fab494bcdf17c2bd6cef02
INT64 ParsePNGChunks(HANDLE hPNGFile, INT64 nCurrentFileSize) {
	INT64 nOffset = 8;
	PPNGCHUNKHEADER chunk_header = new PNGCHUNKHEADER;
	DWORD dwNumRead = 0;

	// Compute this one time since we need to use it many times for comparison
	// Rather than compute it each time - for performance reasons
	size_t PNGCHUNKHEADERSIZE = sizeof(PNGCHUNKHEADER);

	// Read the data
	do {
		dwNumRead = XWF_Read(hPNGFile, nOffset, (BYTE*)chunk_header, (DWORD)PNGCHUNKHEADERSIZE);
		if (dwNumRead == PNGCHUNKHEADERSIZE) {
			// Data is stored in big-endian on disk so we need to swap the byte order
			chunk_header->size = _byteswap_ulong((unsigned long)chunk_header->size);
			chunk_header->type = _byteswap_ulong((unsigned long)chunk_header->type);
		}
		nOffset += PNGCHUNKHEADERSIZE + chunk_header->size + PNG_CHECKSUM_LEN;
	} while (dwNumRead == PNGCHUNKHEADERSIZE && chunk_header->type != PNG_CHUNK_IEND
		&& nOffset < nCurrentFileSize);

	if (nOffset > nCurrentFileSize) {
		// Malformed PNG file; return nCurrentFileSize
		// This will result in the file NOT being flagged as acropalypse
		return nCurrentFileSize;
	}

	// Return the byte offset in the file where the IEND chunk was found
	return nOffset;
}

// Parse JPG segments, we assume that the caller validates that this is an actual JPG file
// Based on description of the JPEG format at https://github.com/corkami/formats/blob/master/image/jpeg.md
INT64 ParseJPGSegments(PBYTE JPG_File, INT64 nCurrentFileSize) {
	INT64 nOffset = 2;	// Start at byte offset 2 (just past the JPG header, 0xffd8)
	PJPG_SEGMENT_HEADER segment = NULL;

	// Read the data
	do {
		// Cast the memory buffer at the specified offset as a JPG_SEGMENT_HEADER object
		PJPG_SEGMENT_HEADER segment = (PJPG_SEGMENT_HEADER)(JPG_File + nOffset);

		// Check null pointer
		if (segment == NULL) {
			break;
		}

		// Confirm validity of the segment
		if (segment->segment_hibyte != 0xff) {
			// If we are in here we've reached an invalid segment; return the current offset
			break;
		}

		// Read the size of this segment and advance the pointer
		// NOTE: The segment header (0xffXX) does not seem to be included in the segment_size value
		// so we must add two bytes to account for that header length
		nOffset += 2 + _byteswap_ushort(segment->segment_size);

		// If this one was ffda, then we're done, nOffset should now point to
		// the start of the SCAN_DATA section
		if (segment->segment_lobyte == 0xda) {
			break;
		}
	} while (nOffset < nCurrentFileSize);

	// Return the offset into the file where the SCAN_DATA begins.
	return nOffset;
}

BOOL FlagImage(LONG nItemID) {
	//XWF_OutputMessage(L"Possible Acropalypse Image detected!", 0); Commented out 
	// b/c if lots of these, I/O slows down processing
	
	
	// Set the "notable" flag; first, get the current flags
	BOOL bSuccess = FALSE;
	INT64 iXwFlags = XWF_GetItemInformation(nItemID, XWF_ITEM_INFO_FLAGS, &bSuccess);

	// If successful, set the notable flag
	if (bSuccess) {
		XWF_SetItemInformation(nItemID, XWF_ITEM_INFO_FLAGS, iXwFlags | XWF_ITEM_INFO_FLAG_NOTABLE);
	}
	else {
		XWF_OutputMessage(L"Error setting notable flag on item!", 0);
		return FALSE;
	}

	// Add a metadata field to the file annotating the likelihood of acropalypse
	LPWSTR lpMetadata = XWF_GetExtractedMetadata(nItemID);
	if (lpMetadata == NULL || wcsstr(lpMetadata, L"Acropalypse") == NULL) {
		XWF_AddExtractedMetadata(nItemID, (LPWSTR)L"Possible Acropalypse image", 2);
	}
	else {
		XWF_OutputMessage(L"Error adding metadata to item!", 0);
		return FALSE;
	}

	// Upate the acropalypse image counter
	EnterCriticalSection(&counterCriticalSection);

	g_acropalypseImageCount += 1;

	LeaveCriticalSection(&counterCriticalSection);

	return TRUE;
}

/// <summary>
/// DLL export called by X-Ways to process the file when running RVS operations 
/// </summary>
/// <param name="nItemID">The ID of the item being processed</param>
/// <param name="hItem">The handle for I/O for the item being processed</param>
/// <param name="lpReserved">Unused</param>
/// <returns>In all cases returns 0. Will log error messages to the console</returns>
LONG __stdcall XT_ProcessItemEx(LONG nItemID, HANDLE hItem, void* lpReserved) {
	// Get the item's type
	wchar_t szType[8] = {};
	LONG lTypeResult = XWF_GetItemType(nItemID, szType, 8);

	// PNG
	if (LOBYTE(lTypeResult) == XT_ITEM_TYPE_CONFIRMED && _wcsnicmp(szType, L"png", 3) == 0) {
		 // Read the PNG header and make sure it's valid
		INT64 PNGHeader;
		DWORD dwNumRead = XWF_Read(hItem, 0, (PBYTE)&PNGHeader, sizeof(PNGHeader));

		// Get the filesize
		INT64 nFileSize = XWF_GetItemSize(nItemID);

		if (nFileSize <= 0) {
			XWF_OutputMessage(L"XT_Acropalypse error processing PNG file: Invalid File Size", 0);
			return 0;
		}

		// Parse the PNG chunks
		INT64 nOffset = ParsePNGChunks(hItem, nFileSize);

		// At this point the nOffset should be the same as the file size;
		// if not, it's possible that we've encountered the acropalypse bug
		if (nOffset < nFileSize) {
			FlagImage(nItemID);
		}

		// Update the counters
		EnterCriticalSection(&counterCriticalSection);

		g_totalImageCount += 1;
		g_pngImageCount += 1;

		LeaveCriticalSection(&counterCriticalSection);
	}

	// JPEG
	if (LOBYTE(lTypeResult) == XT_ITEM_TYPE_CONFIRMED && (_wcsnicmp(szType, L"jpg", 3) == 0 ||
		_wcsnicmp(szType, L"jpeg", 4) == 0)) {
		INT64 nItemSize = XWF_GetProp(hItem, XT_PROPERTY_LOGICAL_FILE_SIZE, NULL);

		// Check to make sure we don't encounter a situation where we would have an INT overflow
		if (nItemSize < 3) {
			XWF_OutputMessage(L"XT_Acropalypse error processing JPG file: invalid file size", 0);
			return 0;
		}

		LPBYTE lpImageBuffer = (LPBYTE)VirtualAlloc(NULL, (SIZE_T)nItemSize, MEM_COMMIT, PAGE_READWRITE);

		if (lpImageBuffer != NULL) {
			DWORD dwNumRead = XWF_Read(hItem, 0, lpImageBuffer, (DWORD)nItemSize);

			// Make sure we read all the bytes of the JPG file
			if (dwNumRead != nItemSize) {
				XWF_OutputMessage(L"Error: Incomplete file read when processing JPEG image for XT_Acropalypse", 0);
				VirtualFree(lpImageBuffer, 0, MEM_RELEASE);
				return 0;
			}

			INT64 nOffset = ParseJPGSegments((PBYTE)lpImageBuffer, nItemSize);
			for (INT64 i = nOffset; i < nItemSize - 3; i++) {	// File size - 3 bytes ensures we never encounter the actual footer
				if (lpImageBuffer[i] == 0xff && lpImageBuffer[i + 1] == 0xd9) {
					FlagImage(nItemID);
					break; // No need to continue looking
				}
			}

			// Free the buffer
			VirtualFree(lpImageBuffer, 0, MEM_RELEASE);
		}
		else {
			XWF_OutputMessage(L"Error: Unable to allocate buffer for JPEG image in XT_Acropalypse", 0);
			return 0;
		}

		// Update the counters
		EnterCriticalSection(&counterCriticalSection);

		g_totalImageCount += 1;
		g_jpgImageCount += 1;

		LeaveCriticalSection(&counterCriticalSection);
	}

	return 0;
}

LONG __stdcall XT_Finalize(HANDLE hVolume, HANDLE hEvidence, DWORD nOpType,
	void* lpReserved) {
	wchar_t summaryText[1024] = {};
	StringCchPrintf(summaryText,
		1024,
		L"XT_Acropalypse completed processing %d files: %d .JPG, %d .PNG. Detected %d acropalypse images.",
		g_totalImageCount,
		g_jpgImageCount,
		g_pngImageCount,
		g_acropalypseImageCount);

	XWF_OutputMessage(summaryText, 0);
	DeleteCriticalSection(&counterCriticalSection);
	return 0;
}