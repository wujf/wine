/*
 * Windows Device Context initialisation functions
 *
 * Copyright 1996 John Harvey
 */

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "windows.h"
#include "module.h"
#include "win16drv.h"
#include "gdi.h"
#include "bitmap.h"
#include "heap.h"
#include "color.h"
#include "font.h"
#include "callback.h"
#include "stddebug.h"
#include "debug.h"

#define SUPPORT_REALIZED_FONTS 1
#pragma pack(1)
typedef struct
{
  SHORT nSize;
  LPVOID lpindata;
  LPVOID lpFont;
  LPVOID lpXForm;
  LPVOID lpDrawMode;
} EXTTEXTDATA, *LPEXTTEXTDATA;
#pragma pack(4)

#if 0
static BOOL16 windrvExtTextOut16( DC *dc, INT16 x, INT16 y, UINT16 flags, const RECT16 * lprect,
                                 LPCSTR str, UINT16 count, const INT16 *lpDx);
#endif

DRAWMODE DrawMode;
static BOOL32 WIN16DRV_CreateDC( DC *dc, LPCSTR driver, LPCSTR device,
                                 LPCSTR output, const DEVMODE16* initData );
static INT32 WIN16DRV_Escape( DC *dc, INT32 nEscape, INT32 cbInput, 
                              SEGPTR lpInData, SEGPTR lpOutData );

static const DC_FUNCTIONS WIN16DRV_Funcs =
{
    NULL,                            /* pArc */
    NULL,                            /* pBitBlt */
    NULL,                            /* pChord */
    WIN16DRV_CreateDC,               /* pCreateDC */
    NULL,                            /* pDeleteDC */
    NULL,                            /* pDeleteObject */
    NULL,                            /* pEllipse */
    NULL,                            /* pEnumDeviceFonts */
    WIN16DRV_Escape,                 /* pEscape */
    NULL,                            /* pExcludeClipRect */
    NULL,                            /* pExcludeVisRect */
    NULL,                            /* pExtFloodFill */
    WIN16DRV_ExtTextOut,             /* pExtTextOut */
    WIN16DRV_GetCharWidth,           /* pGetCharWidth */
    NULL,                            /* pGetPixel */
    WIN16DRV_GetTextExtentPoint,     /* pGetTextExtentPoint */
    WIN16DRV_GetTextMetrics,         /* pGetTextMetrics */
    NULL,                            /* pIntersectClipRect */
    NULL,                            /* pIntersectVisRect */
    NULL,                            /* pLineTo */
    NULL,                            /* pMoveToEx */
    NULL,                            /* pOffsetClipRgn */
    NULL,                            /* pOffsetViewportOrgEx */
    NULL,                            /* pOffsetWindowOrgEx */
    NULL,                            /* pPaintRgn */
    NULL,                            /* pPatBlt */
    NULL,                            /* pPie */
    NULL,                            /* pPolyPolygon */
    NULL,                            /* pPolygon */
    NULL,                            /* pPolyline */
    NULL,                            /* pRealizePalette */
    NULL,                            /* pRectangle */
    NULL,                            /* pRestoreDC */
    NULL,                            /* pRoundRect */
    NULL,                            /* pSaveDC */
    NULL,                            /* pScaleViewportExtEx */
    NULL,                            /* pScaleWindowExtEx */
    NULL,                            /* pSelectClipRgn */
    WIN16DRV_SelectObject,           /* pSelectObject */
    NULL,                            /* pSelectPalette */
    NULL,                            /* pSetBkColor */
    NULL,                            /* pSetBkMode */
    NULL,                            /* pSetDeviceClipping */
    NULL,                            /* pSetDIBitsToDevice */
    NULL,                            /* pSetMapMode */
    NULL,                            /* pSetMapperFlags */
    NULL,                            /* pSetPixel */
    NULL,                            /* pSetPolyFillMode */
    NULL,                            /* pSetROP2 */
    NULL,                            /* pSetRelAbs */
    NULL,                            /* pSetStretchBltMode */
    NULL,                            /* pSetTextAlign */
    NULL,                            /* pSetTextCharacterExtra */
    NULL,                            /* pSetTextColor */
    NULL,                            /* pSetTextJustification */
    NULL,                            /* pSetViewportExtEx */
    NULL,                            /* pSetViewportOrgEx */
    NULL,                            /* pSetWindowExtEx */
    NULL,                            /* pSetWindowOrgEx */
    NULL,                            /* pStretchBlt */
    NULL                             /* pStretchDIBits */
};





/**********************************************************************
 *	     WIN16DRV_Init
 */
BOOL32 WIN16DRV_Init(void)
{
    return DRIVER_RegisterDriver( NULL /* generic driver */, &WIN16DRV_Funcs );
        
}

/* Tempory functions, for initialising structures */
/* These values should be calculated, not hardcoded */
void InitTextXForm(LPTEXTXFORM16 lpTextXForm)
{
    lpTextXForm->txfHeight 	= 0x0001;
    lpTextXForm->txfWidth 	= 0x000c;
    lpTextXForm->txfEscapement 	= 0x0000;
    lpTextXForm->txfOrientation = 0x0000;
    lpTextXForm->txfWeight 	= 0x0190;
    lpTextXForm->txfItalic 	= 0x00;
    lpTextXForm->txfUnderline 	= 0x00;
    lpTextXForm->txfStrikeOut 	= 0x00; 
    lpTextXForm->txfOutPrecision = 0x02;
    lpTextXForm->txfClipPrecision = 0x01;
    lpTextXForm->txfAccelerator = 0x0001;
    lpTextXForm->txfOverhang 	= 0x0000;
}


void InitDrawMode(LPDRAWMODE lpDrawMode)
{
    lpDrawMode->Rop2		= 0x000d;       
    lpDrawMode->bkMode		= 0x0001;     
    lpDrawMode->bkColor		= 0x3fffffff;    
    lpDrawMode->TextColor	= 0x20000000;  
    lpDrawMode->TBreakExtra	= 0x0000;
    lpDrawMode->BreakExtra	= 0x0000; 
    lpDrawMode->BreakErr	= 0x0000;   
    lpDrawMode->BreakRem	= 0x0000;   
    lpDrawMode->BreakCount	= 0x0000; 
    lpDrawMode->CharExtra	= 0x0000;  
    lpDrawMode->LbkColor	= 0x00ffffff;   
    lpDrawMode->LTextColor	= 0x00000000;     
}

/*
 * EnumCallback (GDI.158)
 * 
 * This is the callback function used when EnumDFonts is called. 
 * (The printer drivers uses it to pass info on available fonts).
 *
 * lpvClientData is the pointer passed to EnumDFonts, which points to a WEPFC
 * structure (WEPFC = WINE_ENUM_PRINTER_FONT_CALLBACK).  This structure 
 * contains infomation on how to store the data passed .
 *
 * There are two modes:
 * 	1) Just count the number of fonts available.
 * 	2) Store all font data passed.
 */
WORD WineEnumDFontCallback(LPLOGFONT16 lpLogFont, LPTEXTMETRIC16 lpTextMetrics, 
			   WORD wFontType, LONG lpvClientData) 
{
    int wRet = 0;
    WEPFC *pWEPFC = (WEPFC *)lpvClientData; 
    
    /* Make sure we have the right structure */
    if (pWEPFC != NULL )
    {
        dprintf_win16drv(stddeb, "mode is 0x%x\n",pWEPFC->nMode);
        
	switch (pWEPFC->nMode)
	{
	    /* Count how many fonts */
	  case 1:
	    pWEPFC->nCount++;
	    break;

	    /* Store the fonts in the printer driver structure */
	  case 2:
	  {
	      PRINTER_FONTS_INFO *pPFI;
                  
	      dprintf_win16drv(stddeb, "WineEnumDFontCallback: Found %s %x\n", 
		     lpLogFont->lfFaceName, wFontType);
	      
	      pPFI = &pWEPFC->pLPD->paPrinterFonts[pWEPFC->nCount];
	      memcpy(&(pPFI->lf), lpLogFont, sizeof(LOGFONT16));
	      memcpy(&(pPFI->tm), lpTextMetrics, sizeof(TEXTMETRIC16));	      
	      pWEPFC->nCount++;
              
	  }
	    break;
	}
	wRet = 1;
    }
    dprintf_win16drv(stddeb, "WineEnumDFontCallback: returnd %d\n", wRet);
    return wRet;
}

BOOL32 WIN16DRV_CreateDC( DC *dc, LPCSTR driver, LPCSTR device, LPCSTR output,
                          const DEVMODE16* initData )
{
    LOADED_PRINTER_DRIVER *pLPD;
    WORD wRet;
    DeviceCaps *printerDevCaps;
    FARPROC16 pfnCallback;
    int nPDEVICEsize;
    PDEVICE_HEADER *pPDH;
    WIN16DRV_PDEVICE *physDev;
    LPDRAWMODE lpDrawMode = &DrawMode;

    /* Realizing fonts */
    TEXTXFORM16 TextXForm;
    int nSize;
    char printerEnabled[20];
    PROFILE_GetWineIniString( "wine", "printer", "off",
                             printerEnabled, sizeof(printerEnabled) );
    if (strcmp(printerEnabled,"on"))
    {
        printf("WIN16DRV_CreateDC disabled in wine.conf file\n");
        return FALSE;
    }

    dprintf_win16drv(stddeb, "In creatdc for (%s,%s,%s) initData 0x%p\n",driver, device, output, initData);

    physDev = (WIN16DRV_PDEVICE *)HeapAlloc( SystemHeap, 0, sizeof(*physDev) );
    if (!physDev) return FALSE;
    dc->physDev = physDev;

    pLPD = LoadPrinterDriver(driver);
    if (pLPD == NULL)
    {
	dprintf_win16drv(stddeb, "LPGDI_CreateDC: Failed to find printer driver\n");
        HeapFree( SystemHeap, 0, physDev );
        return FALSE;
    }
    dprintf_win16drv(stddeb, "windevCreateDC pLPD 0x%p\n", pLPD);

    /* Now Get the device capabilities from the printer driver */
    
    printerDevCaps = (DeviceCaps *) malloc(sizeof(DeviceCaps));
    memset(printerDevCaps, 0, sizeof(DeviceCaps));

    /* Get GDIINFO which is the same as a DeviceCaps structure */
    wRet = PRTDRV_Enable(printerDevCaps, GETGDIINFO, device, driver, output,NULL); 

    /* Add this to the DC */
    dc->w.devCaps = printerDevCaps;
    dc->w.hVisRgn = CreateRectRgn32(0, 0, dc->w.devCaps->horzRes, dc->w.devCaps->vertRes);
    dc->w.bitsPerPixel = dc->w.devCaps->bitsPixel;
    
    printf("Got devcaps width %d height %d bits %d planes %d\n",
           dc->w.devCaps->horzRes,
           dc->w.devCaps->vertRes, 
           dc->w.devCaps->bitsPixel,
           dc->w.devCaps->planes);

    /* Now we allocate enough memory for the PDEVICE structure */
    /* The size of this varies between printer drivers */
    /* This PDEVICE is used by the printer DRIVER not by the GDI so must */
    /* be accessable from 16 bit code */
    nPDEVICEsize = dc->w.devCaps->pdeviceSize + sizeof(PDEVICE_HEADER);

    /* TTD Shouldn't really do pointer arithmetic on segment points */
    physDev->segptrPDEVICE = WIN16_GlobalLock16(GlobalAlloc16(GHND, nPDEVICEsize))+sizeof(PDEVICE_HEADER);
    *(BYTE *)(PTR_SEG_TO_LIN(physDev->segptrPDEVICE)+0) = 'N'; 
    *(BYTE *)(PTR_SEG_TO_LIN(physDev->segptrPDEVICE)+1) = 'B'; 

    /* Set up the header */
    pPDH = (PDEVICE_HEADER *)(PTR_SEG_TO_LIN(physDev->segptrPDEVICE) - sizeof(PDEVICE_HEADER)); 
    pPDH->pLPD = pLPD;
    
    dprintf_win16drv(stddeb, "PRTDRV_Enable: PDEVICE allocated %08lx\n",(DWORD)(physDev->segptrPDEVICE));
    
    /* Now get the printer driver to initialise this data */
    wRet = PRTDRV_Enable((LPVOID)physDev->segptrPDEVICE, INITPDEVICE, device, driver, output, NULL); 

    /* Now enumerate the fonts supported by the printer driver*/
    /* GDI.158 is EnumCallback, which is called by the 16bit printer driver */
    /* passing information on the available fonts */
    if (pLPD->paPrinterFonts == NULL)
    {
	pfnCallback = MODULE_GetEntryPoint( GetModuleHandle16("GDI"), 158 );
        
	if (pfnCallback != NULL)
	{
	    WEPFC wepfc;
	    
	    wepfc.nMode = 1;
	    wepfc.nCount = 0;
	    wepfc.pLPD = pLPD;
	    
	    /* First count the number of fonts */
            
	    PRTDRV_EnumDFonts(physDev->segptrPDEVICE, NULL, pfnCallback, 
			      (void *)&wepfc);
	    
	    /* Allocate a buffer to store all of the fonts */
	    pLPD->nPrinterFonts = wepfc.nCount;
            dprintf_win16drv(stddeb, "Got %d fonts\n",wepfc.nCount);
            
	    if (wepfc.nCount > 0)
	    {

		pLPD->paPrinterFonts = malloc(sizeof(PRINTER_FONTS_INFO) * wepfc.nCount);
		
		/* Now get all of the fonts */
		wepfc.nMode = 2;
		wepfc.nCount = 0;
		PRTDRV_EnumDFonts(physDev->segptrPDEVICE, NULL, pfnCallback, 
				  (void *)&wepfc);
	    }
	}
    }
		
    /* Select the first font into the DC */
    /* Set up the logfont */
    memcpy(&physDev->lf, 
	   &pLPD->paPrinterFonts[0].lf, 
	   sizeof(LOGFONT16));

    /* Set up the textmetrics */
    memcpy(&physDev->tm, 
	   &pLPD->paPrinterFonts[0].tm, 
	   sizeof(TEXTMETRIC16));

#ifdef SUPPORT_REALIZED_FONTS
    /* TTD should calculate this */
    InitTextXForm(&TextXForm);
    
    /* First get the size of the realized font */
    nSize = PRTDRV_RealizeObject(physDev->segptrPDEVICE, OBJ_FONT,
				 &pLPD->paPrinterFonts[0], NULL, 
				 NULL);
    
    physDev->segptrFontInfo = WIN16_GlobalLock16(GlobalAlloc16(GHND, nSize));
    /* Realize the font */
    PRTDRV_RealizeObject(physDev->segptrPDEVICE, OBJ_FONT,
			 &pLPD->paPrinterFonts[0], 
			 (LPVOID)physDev->segptrFontInfo, 
			 &TextXForm);
    /* Quick look at structure */
    if (physDev->segptrFontInfo)
    {  
	FONTINFO16 *p = (FONTINFO16 *)PTR_SEG_TO_LIN(physDev->segptrFontInfo);

	dprintf_win16drv(stddeb, "T:%d VR:%d HR:%d, F:%d L:%d\n",
	       p->dfType,
	       p->dfVertRes, p->dfHorizRes,
	       p->dfFirstCHAR, p->dfLastCHAR
	       );
    }

#endif
    /* TTD Lots more to do here */
    InitDrawMode(lpDrawMode);

    return TRUE;
}


/* 
 * Escape (GDI.38)
 */
static INT32 WIN16DRV_Escape( DC *dc, INT32 nEscape, INT32 cbInput, 
                              SEGPTR lpInData, SEGPTR lpOutData )
{
    WIN16DRV_PDEVICE *physDev = (WIN16DRV_PDEVICE *)dc->physDev;
    int nRet = 0;

    /* We should really process the nEscape parameter, but for now just
       pass it all to the driver */
    if (dc != NULL && physDev->segptrPDEVICE != 0)
    {
	switch(nEscape)
	{
	  case 0x9:
	    printf("Escape: SetAbortProc ignored\n");
	    break;
	case 0x100:
	  {
	    LPEXTTEXTDATA textData =  PTR_SEG_TO_LIN(lpInData);
	    printf("Got in data 0x%x textData 0x%x\n",lpInData, textData);
	    printf("size %d in 0x%x:0x%x font 0x%x:0x%x xform 0x%x:0x%x drawm 0x%x:0x%x\n",
		   textData->nSize,
		   textData->lpindata,PTR_SEG_TO_LIN(textData->lpindata),
		   textData->lpFont,PTR_SEG_TO_LIN(textData->lpFont),
		   textData->lpXForm,PTR_SEG_TO_LIN(textData->lpXForm),
		   textData->lpDrawMode,PTR_SEG_TO_LIN(textData->lpDrawMode));
	  }
	    break;
	  default:
	    nRet = PRTDRV_Control(physDev->segptrPDEVICE, nEscape, 
				  lpInData, lpOutData);
	}
    }
    else
	fprintf(stderr, "Escape(nEscape = %04x)\n", nEscape);      
    return nRet;
}




/****************** misc. printer releated functions */

/*
 * The following function should implement a queing system
 */
#ifndef HPQ 
#define HPQ WORD
#endif
struct hpq 
{
    struct hpq 	*next;
    int		 tag;
    int		 key;
};

static struct hpq *hpqueue;

HPQ 
CreatePQ(int size) 
{
    printf("CreatePQ: %d\n",size);
    return 1;
}
int 
DeletePQ(HPQ hPQ) 
{
    printf("DeletePQ: %x\n", hPQ);
    return 0;
}
int 
ExtractPQ(HPQ hPQ) 
{ 
    struct hpq *queue, *prev, *current, *currentPrev;
    int key, tag = -1;
    currentPrev = prev = NULL;
    queue = current = hpqueue;
    if (current)
        key = current->key;
    
    while (current)
    {
        currentPrev = current;
        current = current->next;
        if (current)
        {
            if (current->key < key)
            {
                queue = current;
                prev = currentPrev;
            }
        }
    }
    if (queue)
    {
        tag = queue->tag;
        
        if (prev)
            prev->next = queue->next;
        else
            hpqueue = queue->next;
        free(queue);
    }
    
    printf("ExtractPQ: %x got tag %d key %d\n", hPQ, tag, key); 

    return tag;
}

int 
InsertPQ(HPQ hPQ, int tag, int key) 
{
    struct hpq *queueItem = malloc(sizeof(struct hpq));
    queueItem->next = hpqueue;
    hpqueue = queueItem;
    queueItem->key = key;
    queueItem->tag = tag;
    
    printf("InsertPQ: %x %d %d\n", hPQ, tag, key);
    return TRUE;
}
int
MinPQ(HPQ hPQ) 
{
    printf("MinPQ: %x\n", hPQ); 
    return 0;
}
int
SizePQ(HPQ hPQ, int sizechange) 
{  
    printf("SizePQ: %x %d\n", hPQ, sizechange); 
    return -1; 
}

/* 
 * The following functions implement part of the spooling process to 
 * print manager.  I would like to see wine have a version of print managers
 * that used LPR/LPD.  For simplicity print jobs will be sent to a file for
 * now.
 */
typedef struct PRINTJOB
{
    char	*pszOutput;
    char 	*pszTitle;
    HDC16  	hDC;
    HANDLE16 	hHandle;
    int		nIndex;
    int		fd;
} PRINTJOB, *PPRINTJOB;

#define MAX_PRINT_JOBS 1
#define SP_OK 1

PPRINTJOB gPrintJobsTable[MAX_PRINT_JOBS];


static PPRINTJOB FindPrintJobFromHandle(HANDLE16 hHandle)
{
    return gPrintJobsTable[0];
}

/* TTD Need to do some DOS->UNIX file conversion here */
static int CreateSpoolFile(LPSTR pszOutput)
{
    int fd;
    char szSpoolFile[32];

    /* TTD convert the 'output device' into a spool file name */

    if (pszOutput == NULL || *pszOutput == '\0')
	strcpy(szSpoolFile,"lp.out");
    else
	strcpy(szSpoolFile, pszOutput);

    if ((fd = open(szSpoolFile, O_CREAT | O_TRUNC | O_WRONLY , 0600)) < 0)
    {
	printf("Failed to create spool file %s, errno = %d\n", szSpoolFile, errno);
    }
    return fd;
}

static int FreePrintJob(HANDLE16 hJob)
{
    int nRet = SP_ERROR;
    PPRINTJOB pPrintJob;

    pPrintJob = FindPrintJobFromHandle(hJob);
    if (pPrintJob != NULL)
    {
	gPrintJobsTable[pPrintJob->nIndex] = NULL;
	free(pPrintJob->pszOutput);
	free(pPrintJob->pszTitle);
	if (pPrintJob->fd >= 0) close(pPrintJob->fd);
	free(pPrintJob);
	nRet = SP_OK;
    }
    return nRet;
}

HANDLE16 OpenJob(LPSTR lpOutput, LPSTR lpTitle, HDC16 hDC)
{
    HANDLE16 hHandle = SP_ERROR;
    PPRINTJOB pPrintJob;

    dprintf_win16drv(stddeb, "OpenJob: \"%s\" \"%s\" %04x\n", lpOutput, lpTitle, hDC);

    pPrintJob = gPrintJobsTable[0];
    if (pPrintJob == NULL)
    {
	int fd;

	/* Try an create a spool file */
	fd = CreateSpoolFile(lpOutput);
	if (fd >= 0)
	{
	    hHandle = 1;

	    pPrintJob = malloc(sizeof(PRINTJOB));
	    memset(pPrintJob, 0, sizeof(PRINTJOB));

	    pPrintJob->pszOutput = strdup(lpOutput);
	    pPrintJob->pszTitle = strdup(lpTitle);
	    pPrintJob->hDC = hDC;
	    pPrintJob->fd = fd;
	    pPrintJob->nIndex = 0;
	    pPrintJob->hHandle = hHandle; 
	    gPrintJobsTable[pPrintJob->nIndex] = pPrintJob; 
	}
    }
    dprintf_win16drv(stddeb, "OpenJob: return %04x\n", hHandle);
    return hHandle;
}

int CloseJob(HANDLE16 hJob)
{
    int nRet = SP_ERROR;
    PPRINTJOB pPrintJob = NULL;

    dprintf_win16drv(stddeb, "CloseJob: %04x\n", hJob);

    pPrintJob = FindPrintJobFromHandle(hJob);
    if (pPrintJob != NULL)
    {
	/* Close the spool file */
	close(pPrintJob->fd);
	FreePrintJob(hJob);
	nRet  = 1;
    }
    return nRet;
}

int WriteSpool(HANDLE16 hJob, LPSTR lpData, WORD cch)
{
    int nRet = SP_ERROR;
    PPRINTJOB pPrintJob = NULL;

    dprintf_win16drv(stddeb, "WriteSpool: %04x %08lx %04x\n", hJob, (DWORD)lpData, cch);

    pPrintJob = FindPrintJobFromHandle(hJob);
    if (pPrintJob != NULL && pPrintJob->fd >= 0 && cch)
    {
	if (write(pPrintJob->fd, lpData, cch) != cch)
	  nRet = SP_OUTOFDISK;
	else
	  nRet = cch;
    }
    return nRet;
}

int WriteDialog(HANDLE16 hJob, LPSTR lpMsg, WORD cchMsg)
{
    int nRet = 0;

    dprintf_win16drv(stddeb, "WriteDialog: %04x %04x \"%s\"\n", hJob,  cchMsg, lpMsg);

    nRet = MessageBox16(NULL, lpMsg, "Printing Error", MB_OKCANCEL);
    return nRet;
}

int DeleteJob(HANDLE16 hJob, WORD wNotUsed)
{
    int nRet;

    dprintf_win16drv(stddeb, "DeleteJob: %04x\n", hJob);

    nRet = FreePrintJob(hJob);
    return nRet;
}

/* 
 * The following two function would allow a page to be sent to the printer
 * when it has been processed.  For simplicity they havn't been implemented.
 * This means a whole job has to be processed before it is sent to the printer.
 */
int StartSpoolPage(HANDLE16 hJob)
{
    dprintf_win16drv(stddeb, "StartSpoolPage GDI.246 unimplemented\n");
    return 1;

}
int EndSpoolPage(HANDLE16 hJob)
{
    dprintf_win16drv(stddeb, "EndSpoolPage GDI.247 unimplemented\n");
    return 1;
}


DWORD GetSpoolJob(int nOption, LONG param)
{
    DWORD retval = 0;
    dprintf_win16drv(stddeb, "In GetSpoolJob param 0x%lx noption %d\n",param, nOption);
    return retval;
}
