/* w42-scan.c - see w42-scan.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-scan.h"

#include <string.h>
#include <glib/gstdio.h>
#include <unistd.h>

#ifdef G_OS_WIN32

/* Windows Image Acquisition, through its Automation layer: the same
 * WIA.CommonDialog that Word XP's "From Scanner or Camera" used.  It is
 * driven by IDispatch names so that no WIA headers or import libraries
 * are needed beyond ole32 and oleaut32, which every Windows has. */

#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <oaidl.h>

/* Calls `name` on `obj` with `args` (in order) and puts what it returns
 * in `result`.  Automation wants the arguments backwards. */
static HRESULT
dispatch_call (IDispatch *obj, const wchar_t *name, VARIANT *args, int n_args,
               VARIANT *result)
{
  DISPID id;
  DISPPARAMS params;
  VARIANT *reversed = NULL;
  HRESULT hr;
  wchar_t *names[1] = { (wchar_t *) name };

  hr = IDispatch_GetIDsOfNames (obj, &IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id);
  if (FAILED (hr))
    return hr;
  if (n_args > 0)
    {
      reversed = g_new0 (VARIANT, n_args);
      for (int i = 0; i < n_args; i++)
        reversed[i] = args[n_args - 1 - i];
    }
  params.rgvarg = reversed;
  params.cArgs = (UINT) n_args;
  params.rgdispidNamedArgs = NULL;
  params.cNamedArgs = 0;
  if (result != NULL)
    VariantInit (result);
  hr = IDispatch_Invoke (obj, id, &IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
                         &params, result, NULL, NULL);
  g_free (reversed);
  return hr;
}

gboolean
w42_scan_available (void)
{
  return TRUE;
}

GBytes *
w42_scan_acquire (GtkWindow *parent, const char **format, GError **error)
{
  CLSID clsid;
  IDispatch *dialog = NULL;
  VARIANT args[7], result;
  HRESULT hr;
  GBytes *bytes = NULL;
  gboolean com_here;
  char *path = NULL;
  wchar_t *wpath;

  (void) parent;

  com_here = SUCCEEDED (CoInitializeEx (NULL, COINIT_APARTMENTTHREADED));
  hr = CLSIDFromProgID (L"WIA.CommonDialog", &clsid);
  if (SUCCEEDED (hr))
    hr = CoCreateInstance (&clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IDispatch, (void **) &dialog);
  if (FAILED (hr))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Windows Image Acquisition is not available on this computer.");
      goto out;
    }

  /* ShowAcquireImage (DeviceType, Intent, Bias, FormatID, AlwaysSelectDevice,
   * UseCommonUI, CancelError): any device, colour, a good picture, as a
   * PNG, choosing the device only when there is more than one, with the
   * device's own dialog, and a cancel that is not an error. */
  for (int i = 0; i < 7; i++)
    VariantInit (&args[i]);
  args[0].vt = VT_I4; args[0].lVal = 0;          /* UnspecifiedDeviceType */
  args[1].vt = VT_I4; args[1].lVal = 1;          /* ColorIntent */
  args[2].vt = VT_I4; args[2].lVal = 0x20000;    /* MaximizeQuality */
  args[3].vt = VT_BSTR;
  args[3].bstrVal = SysAllocString (L"{B96B3CAF-0728-11D3-9D7B-0000F81EF32E}");  /* wiaFormatPNG */
  args[4].vt = VT_BOOL; args[4].boolVal = VARIANT_FALSE;
  args[5].vt = VT_BOOL; args[5].boolVal = VARIANT_TRUE;
  args[6].vt = VT_BOOL; args[6].boolVal = VARIANT_FALSE;
  hr = dispatch_call (dialog, L"ShowAcquireImage", args, 7, &result);
  SysFreeString (args[3].bstrVal);
  if (FAILED (hr))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "The scanner could not be used (WIA error 0x%08lx).", (unsigned long) hr);
      goto out;
    }
  if (result.vt != VT_DISPATCH || result.pdispVal == NULL)
    goto out;                                  /* no device, or given up */

  /* The ImageFile: saved to a file of its own and read back, which is
   * simpler than unpicking its SAFEARRAY, and lets WIA write the PNG. */
  {
    IDispatch *image = result.pdispVal;
    VARIANT arg, dummy;
    int fd;

    fd = g_file_open_tmp ("word42-scan-XXXXXX", &path, NULL);
    if (fd >= 0)
      close (fd);
    wpath = path != NULL ? g_utf8_to_utf16 (path, -1, NULL, NULL, NULL) : NULL;
    if (path != NULL)
      g_unlink (path);                         /* SaveFile will not overwrite */
    VariantInit (&arg);
    arg.vt = VT_BSTR;
    arg.bstrVal = SysAllocString (wpath != NULL ? wpath : L"");
    hr = dispatch_call (image, L"SaveFile", &arg, 1, &dummy);
    SysFreeString (arg.bstrVal);
    g_free (wpath);
    IDispatch_Release (image);
    if (FAILED (hr) || path == NULL)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "The scanned picture could not be saved (WIA error 0x%08lx).", (unsigned long) hr);
        goto out;
      }
    {
      char *data = NULL;
      gsize len = 0;

      if (g_file_get_contents (path, &data, &len, error))
        bytes = g_bytes_new_take (data, len);
    }
    g_unlink (path);
    if (format != NULL)
      *format = g_intern_static_string ("png");
  }

out:
  g_free (path);
  if (dialog != NULL)
    IDispatch_Release (dialog);
  if (com_here)
    CoUninitialize ();
  return bytes;
}

#elif defined(__APPLE__)

gboolean
w42_scan_available (void)
{
  return FALSE;
}

GBytes *
w42_scan_acquire (GtkWindow *parent, const char **format, GError **error)
{
  (void) parent; (void) format;
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "Scanning is not available on macOS yet.  Scan with Image Capture "
               "and use Insert > Picture > From File.");
  return NULL;
}

#else

/* SANE, through scanimage: every distribution has it, and it drives
 * every scanner SANE knows without word42 having to. */

gboolean
w42_scan_available (void)
{
  char *found = g_find_program_in_path ("scanimage");
  gboolean ok = found != NULL;

  g_free (found);
  return ok;
}

GBytes *
w42_scan_acquire (GtkWindow *parent, const char **format, GError **error)
{
  char *path = NULL;
  int fd;
  char *argv[6] = { NULL };
  int status = 0;
  char *err_text = NULL;
  GBytes *bytes = NULL;

  (void) parent;
  if (!w42_scan_available ())
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "scanimage was not found.  Install SANE (sane-utils) to scan.");
      return NULL;
    }
  fd = g_file_open_tmp ("word42-scan-XXXXXX", &path, error);
  if (fd < 0)
    return NULL;
  close (fd);

  argv[0] = (char *) "scanimage";
  argv[1] = (char *) "--format=png";
  argv[2] = (char *) "--mode=Color";
  argv[3] = (char *) "-o";
  argv[4] = path;
  argv[5] = NULL;
  if (!g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
                     NULL, NULL, NULL, &err_text, &status, error))
    {
      g_unlink (path);
      g_free (path);
      return NULL;
    }
  if (!g_spawn_check_wait_status (status, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "The scanner said: %s",
                   err_text != NULL && *err_text != '\0' ? g_strstrip (err_text) : "nothing");
      g_free (err_text);
      g_unlink (path);
      g_free (path);
      return NULL;
    }
  g_free (err_text);
  {
    char *data = NULL;
    gsize len = 0;

    if (g_file_get_contents (path, &data, &len, error))
      bytes = g_bytes_new_take (data, len);
  }
  g_unlink (path);
  g_free (path);
  if (format != NULL)
    *format = g_intern_static_string ("png");
  return bytes;
}

#endif
