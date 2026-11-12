// Outbound OLE drag source.  The data object owns immutable copies of the exported paths and text, so its lifetime
// is independent of the Kotuku source object, its widget and the preview surface.  OLE and any native recipient may
// retain the object after DoDragDrop() returns; it is released by reference count and never calls into the framework.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#define WINVER 0x0600
#endif

#ifdef _MSC_VER
#pragma warning (disable : 4244 4311 4312 4267 4068)
#endif

#include <kotuku/system/errors_c.h>

#include <windows.h>
#include <shlobj.h>
#include <objidl.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "clipboard.h"

#define EXPORT_MAX_FORMATS 4

typedef struct rkExportOffer {
   IDataObject Interface;
   LONG References;
   HGLOBAL Files;       // DROPFILES block, or NULL
   HGLOBAL Text;        // CF_UNICODETEXT with CRLF line endings, or NULL
   HGLOBAL Effect;      // CFSTR_PREFERREDDROPEFFECT: always DROPEFFECT_COPY
   HGLOBAL Marker;      // Process ID and session cookie, used to recognise our own offer
   FORMATETC Formats[EXPORT_MAX_FORMATS];
   int FormatCount;
} RK_EXPORT_OFFER;

typedef struct rkExportSource {
   IDropSource Interface;
   LONG References;
} RK_EXPORT_SOURCE;

static UINT fmtExportMarker = 0;
static UINT fmtExportEffect = 0;
static DWORD glExportCookie = 0;  // Non-zero only while our own DoDragDrop() is running
static DWORD glExportCounter = 0;

static void register_export_formats(void)
{
   if (!fmtExportMarker) fmtExportMarker = RegisterClipboardFormatA("Kotuku.DragExport");
   if (!fmtExportEffect) fmtExportEffect = RegisterClipboardFormatA(CFSTR_PREFERREDDROPEFFECT);
}

//********************************************************************************************************************

static HGLOBAL copy_global(HGLOBAL Source)
{
   SIZE_T size = GlobalSize(Source);
   HGLOBAL result = GlobalAlloc(GMEM_MOVEABLE, size);
   void *src, *dest;
   if (!result) return NULL;
   src = GlobalLock(Source);
   dest = GlobalLock(result);
   if ((src) && (dest)) memcpy(dest, src, size);
   if (src) GlobalUnlock(Source);
   if (dest) GlobalUnlock(result);
   if ((!src) || (!dest)) { GlobalFree(result); return NULL; }
   return result;
}

static HGLOBAL global_from(const void *Data, SIZE_T Size)
{
   HGLOBAL result = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, Size);
   void *dest;
   if (!result) return NULL;
   if (!(dest = GlobalLock(result))) { GlobalFree(result); return NULL; }
   memcpy(dest, Data, Size);
   GlobalUnlock(result);
   return result;
}

static HGLOBAL offer_global(RK_EXPORT_OFFER *Self, CLIPFORMAT Format)
{
   if (Format == CF_HDROP) return Self->Files;
   if (Format == CF_UNICODETEXT) return Self->Text;
   if (Format == fmtExportEffect) return Self->Effect;
   if (Format == fmtExportMarker) return Self->Marker;
   return NULL;
}

//********************************************************************************************************************
// IDataObject

static HRESULT STDMETHODCALLTYPE EXP_QueryInterface(IDataObject *Object, REFIID ID, void **Result)
{
   if (!Result) return E_POINTER;
   *Result = NULL;
   if (IsEqualGUID(ID, &IID_IUnknown) || IsEqualGUID(ID, &IID_IDataObject)) {
      *Result = Object;
      InterlockedIncrement(&((RK_EXPORT_OFFER *)Object)->References);
      return S_OK;
   }
   return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE EXP_AddRef(IDataObject *Object)
{
   return InterlockedIncrement(&((RK_EXPORT_OFFER *)Object)->References);
}

static ULONG STDMETHODCALLTYPE EXP_Release(IDataObject *Object)
{
   RK_EXPORT_OFFER *self = (RK_EXPORT_OFFER *)Object;
   LONG count = InterlockedDecrement(&self->References);
   if (count == 0) {
      if (self->Files) GlobalFree(self->Files);
      if (self->Text) GlobalFree(self->Text);
      if (self->Effect) GlobalFree(self->Effect);
      if (self->Marker) GlobalFree(self->Marker);
      free(self);
   }
   return count;
}

static HRESULT STDMETHODCALLTYPE EXP_QueryGetData(IDataObject *Object, FORMATETC *Format)
{
   if (!Format) return E_INVALIDARG;
   if (Format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
   if (!offer_global((RK_EXPORT_OFFER *)Object, Format->cfFormat)) return DV_E_FORMATETC;
   if (!(Format->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
   return S_OK;
}

static HRESULT STDMETHODCALLTYPE EXP_GetData(IDataObject *Object, FORMATETC *Format, STGMEDIUM *Medium)
{
   HRESULT result;
   if (!Medium) return E_INVALIDARG;
   memset(Medium, 0, sizeof(*Medium));
   if ((result = EXP_QueryGetData(Object, Format)) != S_OK) return result;
   // Each recipient receives its own copy, so the offer remains immutable.
   if (!(Medium->hGlobal = copy_global(offer_global((RK_EXPORT_OFFER *)Object, Format->cfFormat)))) {
      return E_OUTOFMEMORY;
   }
   Medium->tymed = TYMED_HGLOBAL;
   return S_OK;
}

static HRESULT STDMETHODCALLTYPE EXP_GetDataHere(IDataObject *Object, FORMATETC *Format, STGMEDIUM *Medium)
{
   return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE EXP_GetCanonicalFormatEtc(IDataObject *Object, FORMATETC *Input, FORMATETC *Output)
{
   if (!Output) return E_INVALIDARG;
   Output->ptd = NULL;
   return DATA_S_SAMEFORMATETC;
}

// Shell recipients may try to store drag-image or drop-description state.  The offer is immutable, so this is refused.

static HRESULT STDMETHODCALLTYPE EXP_SetData(IDataObject *Object, FORMATETC *Format, STGMEDIUM *Medium, BOOL Release)
{
   return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE EXP_EnumFormatEtc(IDataObject *Object, DWORD Direction, IEnumFORMATETC **Result)
{
   RK_EXPORT_OFFER *self = (RK_EXPORT_OFFER *)Object;
   if (!Result) return E_POINTER;
   *Result = NULL;
   if (Direction != DATADIR_GET) return E_NOTIMPL;
   return SHCreateStdEnumFmtEtc(self->FormatCount, self->Formats, Result);
}

static HRESULT STDMETHODCALLTYPE EXP_DAdvise(IDataObject *Object, FORMATETC *Format, DWORD Flags,
   IAdviseSink *Sink, DWORD *Connection)
{
   return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE EXP_DUnadvise(IDataObject *Object, DWORD Connection)
{
   return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE EXP_EnumDAdvise(IDataObject *Object, IEnumSTATDATA **Result)
{
   return OLE_E_ADVISENOTSUPPORTED;
}

static IDataObjectVtbl glExportOfferVTable = {
   EXP_QueryInterface, EXP_AddRef, EXP_Release, EXP_GetData, EXP_GetDataHere, EXP_QueryGetData,
   EXP_GetCanonicalFormatEtc, EXP_SetData, EXP_EnumFormatEtc, EXP_DAdvise, EXP_DUnadvise, EXP_EnumDAdvise
};

//********************************************************************************************************************
// IDropSource.  The drag continues only while the framework's session is alive, so destroying the source object or
// deactivating the pointer cancels the native session.

static HRESULT STDMETHODCALLTYPE SRC_QueryInterface(IDropSource *Object, REFIID ID, void **Result)
{
   if (!Result) return E_POINTER;
   *Result = NULL;
   if (IsEqualGUID(ID, &IID_IUnknown) || IsEqualGUID(ID, &IID_IDropSource)) {
      *Result = Object;
      InterlockedIncrement(&((RK_EXPORT_SOURCE *)Object)->References);
      return S_OK;
   }
   return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE SRC_AddRef(IDropSource *Object)
{
   return InterlockedIncrement(&((RK_EXPORT_SOURCE *)Object)->References);
}

static ULONG STDMETHODCALLTYPE SRC_Release(IDropSource *Object)
{
   RK_EXPORT_SOURCE *self = (RK_EXPORT_SOURCE *)Object;
   LONG count = InterlockedDecrement(&self->References);
   if (count == 0) free(self);
   return count;
}

static HRESULT STDMETHODCALLTYPE SRC_QueryContinueDrag(IDropSource *Object, BOOL Escape, DWORD Keys)
{
   if (Escape) return DRAGDROP_S_CANCEL;
   if (!winDragExport_Active()) return DRAGDROP_S_CANCEL;
   if (!(Keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
   return S_OK;
}

static HRESULT STDMETHODCALLTYPE SRC_GiveFeedback(IDropSource *Object, DWORD Effect)
{
   return DRAGDROP_S_USEDEFAULTCURSORS;
}

static IDropSourceVtbl glExportSourceVTable = {
   SRC_QueryInterface, SRC_AddRef, SRC_Release, SRC_QueryContinueDrag, SRC_GiveFeedback
};

static IDropSource * create_export_source(void)
{
   RK_EXPORT_SOURCE *source = (RK_EXPORT_SOURCE *)calloc(1, sizeof(RK_EXPORT_SOURCE));
   if (!source) return NULL;
   source->Interface.lpVtbl = &glExportSourceVTable;
   source->References = 1;
   return &source->Interface;
}

//********************************************************************************************************************
// Converts UTF-8 to a newly allocated, terminated UTF-16 string.  Returns NULL for invalid UTF-8.

static WCHAR * utf8_to_wide(const char *Text, int Length, int *OutLength)
{
   int size;
   WCHAR *result;
   *OutLength = 0;
   if (Length == 0) {
      if ((result = (WCHAR *)calloc(1, sizeof(WCHAR)))) return result;
      return NULL;
   }
   size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text, Length, NULL, 0);
   if (size <= 0) return NULL;
   if (!(result = (WCHAR *)malloc((size + 1) * sizeof(WCHAR)))) return NULL;
   MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text, Length, result, size);
   result[size] = 0;
   *OutLength = size;
   return result;
}

// Builds a wide DROPFILES block.  Separators are normalised and folder paths lose their trailing separator, except at
// a drive root, because shell recipients name folders without one.

static HGLOBAL build_hdrop(const char *const *Files, int Count)
{
   WCHAR **paths;
   int *lengths, i, total = 0, valid = 0;
   HGLOBAL result = NULL;

   if (Count <= 0) return NULL;
   paths = (WCHAR **)calloc(Count, sizeof(WCHAR *));
   lengths = (int *)calloc(Count, sizeof(int));
   if ((!paths) || (!lengths)) goto exit;

   for (i=0; i < Count; i++) {
      int length, j;
      WCHAR *path;
      if (!Files[i]) continue;
      if (!(path = utf8_to_wide(Files[i], (int)strlen(Files[i]), &length))) continue;
      for (j=0; j < length; j++) if (path[j] == L'/') path[j] = L'\\';
      while ((length > 1) && (path[length-1] == L'\\') && !((length == 3) && (path[1] == L':'))) {
         path[--length] = 0;
      }
      if (!length) { free(path); continue; }
      paths[i] = path;
      lengths[i] = length;
      total += length + 1;
      valid++;
   }

   if (valid) {
      SIZE_T size = sizeof(DROPFILES) + (total + 1) * sizeof(WCHAR);
      if ((result = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size))) {
         DROPFILES *drop = (DROPFILES *)GlobalLock(result);
         if (drop) {
            WCHAR *dest = (WCHAR *)((char *)drop + sizeof(DROPFILES));
            drop->pFiles = sizeof(DROPFILES);
            drop->fWide = TRUE;
            for (i=0; i < Count; i++) {
               if (!paths[i]) continue;
               memcpy(dest, paths[i], lengths[i] * sizeof(WCHAR));
               dest += lengths[i] + 1;
            }
            GlobalUnlock(result);
         }
         else { GlobalFree(result); result = NULL; }
      }
   }

exit:
   if (paths) {
      for (i=0; i < Count; i++) free(paths[i]);
      free(paths);
   }
   free(lengths);
   return result;
}

// Native text recipients expect CRLF line endings.  Existing CRLF pairs are preserved and lone CR or LF characters
// are expanded.

static HGLOBAL build_text(const char *Text, int Length)
{
   int wide_length, i, out = 0;
   WCHAR *wide, *crlf;
   HGLOBAL result = NULL;

   if ((!Text) || (Length < 0)) return NULL;
   if (!(wide = utf8_to_wide(Text, Length, &wide_length))) return NULL;
   if ((crlf = (WCHAR *)malloc((wide_length * 2 + 1) * sizeof(WCHAR)))) {
      for (i=0; i < wide_length; i++) {
         if (wide[i] == L'\r') {
            crlf[out++] = L'\r';
            crlf[out++] = L'\n';
            if ((i + 1 < wide_length) && (wide[i+1] == L'\n')) i++;
         }
         else if (wide[i] == L'\n') {
            crlf[out++] = L'\r';
            crlf[out++] = L'\n';
         }
         else crlf[out++] = wide[i];
      }
      crlf[out++] = 0;
      result = global_from(crlf, out * sizeof(WCHAR));
      free(crlf);
   }
   free(wide);
   return result;
}

static void add_format(RK_EXPORT_OFFER *Self, CLIPFORMAT Format)
{
   FORMATETC format = { Format, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
   if (Self->FormatCount < EXPORT_MAX_FORMATS) Self->Formats[Self->FormatCount++] = format;
}

//********************************************************************************************************************
// Creates an immutable offer with a reference count of one.  Returns NULL if neither files nor text are usable.

IDataObject * winCreateDragExport(const char *const *Files, int FileCount, const char *Text, int TextLength,
   DWORD Cookie)
{
   RK_EXPORT_OFFER *offer;
   DWORD effect = DROPEFFECT_COPY;
   DWORD marker[2] = { GetCurrentProcessId(), Cookie };

   register_export_formats();
   if (!(offer = (RK_EXPORT_OFFER *)calloc(1, sizeof(RK_EXPORT_OFFER)))) return NULL;
   offer->Interface.lpVtbl = &glExportOfferVTable;
   offer->References = 1;
   offer->Files = build_hdrop(Files, FileCount);
   if (Text) offer->Text = build_text(Text, TextLength);

   if ((!offer->Files) && (!offer->Text)) {
      EXP_Release(&offer->Interface);
      return NULL;
   }

   offer->Effect = global_from(&effect, sizeof(effect));
   offer->Marker = global_from(marker, sizeof(marker));
   if ((!offer->Effect) || (!offer->Marker)) {
      EXP_Release(&offer->Interface);
      return NULL;
   }

   if (offer->Files) add_format(offer, CF_HDROP);
   if (offer->Text) add_format(offer, CF_UNICODETEXT);
   add_format(offer, (CLIPFORMAT)fmtExportEffect);
   add_format(offer, (CLIPFORMAT)fmtExportMarker);
   return &offer->Interface;
}

//********************************************************************************************************************
// Returns 1 if the offer is the one exported by this process's active DoDragDrop().  Offers from other processes,
// including other Kotuku processes, are treated as ordinary native offers.

int winIsDragExport(IDataObject *Data)
{
   FORMATETC format = { 0, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
   STGMEDIUM medium;
   int result = 0;

   if ((!Data) || (!glExportCookie)) return 0;
   register_export_formats();
   format.cfFormat = (CLIPFORMAT)fmtExportMarker;
   if (Data->lpVtbl->GetData(Data, &format, &medium) != S_OK) return 0;
   if (GlobalSize(medium.hGlobal) >= 2 * sizeof(DWORD)) {
      DWORD *marker = (DWORD *)GlobalLock(medium.hGlobal);
      if (marker) {
         result = (marker[0] == GetCurrentProcessId()) && (marker[1] == glExportCookie);
         GlobalUnlock(medium.hGlobal);
      }
   }
   ReleaseStgMedium(&medium);
   return result;
}

//********************************************************************************************************************
// Runs a native copy-only drag.  This blocks in OLE's modal loop until the recipient accepts or refuses the drop, or
// the drag is cancelled.  Effect receives the DROPEFFECT reported by the recipient.

int winStartDragExport(const char *const *Files, int FileCount, const char *Text, int TextLength, int *Effect)
{
   IDataObject *offer;
   IDropSource *source;
   DWORD effect = DROPEFFECT_NONE, cookie;
   HRESULT result;

   if (Effect) *Effect = DROPEFFECT_NONE;
   if (glOleInit != 1) return ERR_NoSupport;
   if (glExportCookie) return ERR_InvalidState; // DoDragDrop() is not reentrant

   // OLE expects the button that started the gesture to be held.  It is the logical left button.
   if (!(GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000)) {
      return ERR_InvalidState;
   }

   if (!(cookie = ++glExportCounter)) cookie = ++glExportCounter;
   if (!(offer = winCreateDragExport(Files, FileCount, Text, TextLength, cookie))) return ERR_NoData;
   if (!(source = create_export_source())) {
      offer->lpVtbl->Release(offer);
      return ERR_AllocMemory;
   }

   glExportCookie = cookie;
   result = DoDragDrop(offer, source, DROPEFFECT_COPY, &effect);
   glExportCookie = 0;

   source->lpVtbl->Release(source);
   offer->lpVtbl->Release(offer);

   if (result == DRAGDROP_S_DROP) {
      if (Effect) *Effect = (int)effect;
      return ERR_Okay;
   }
   else if (result == DRAGDROP_S_CANCEL) return ERR_Cancelled;
   else return ERR_Failed;
}
