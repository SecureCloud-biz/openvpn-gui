/*
 *  OpenVPN-GUI -- A Windows GUI for OpenVPN.
 *
 *  Copyright (C) 2004 Mathias Sundman <mathias@nilings.se>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program (see the file COPYING included with this
 *  distribution); if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#if !defined (UNICODE)
#error UNICODE and _UNICODE must be defined. This version only supports unicode builds.
#endif

#include <windows.h>
#include <shlwapi.h>
#include <wtsapi32.h>
#include <prsht.h>
#include <pbt.h>
#include <commdlg.h>

#include "tray.h"
#include "openvpn.h"
#include "openvpn_config.h"
#include "viewlog.h"
#include "service.h"
#include "main.h"
#include "options.h"
#include "passphrase.h"
#include "proxy.h"
#include "registry.h"
#include "openvpn-gui-res.h"
#include "localization.h"
#include "manage.h"
#include "misc.h"
#include "save_pass.h"

#ifndef DISABLE_CHANGE_PASSWORD
#include <openssl/evp.h>
#include <openssl/err.h>
#endif

/*  Declare Windows procedure  */
LRESULT CALLBACK WindowProcedure (HWND, UINT, WPARAM, LPARAM);
static void ShowSettingsDialog();
void CloseApplication(HWND hwnd);
void ImportConfigFile();

/*  Class name and window title  */
TCHAR szClassName[ ] = _T("OpenVPN-GUI");
TCHAR szTitleText[ ] = _T("OpenVPN");

/* Options structure */
options_t o;

/* Workaround for ASLR on Windows */
__declspec(dllexport) char aslr_workaround;

static int
VerifyAutoConnections()
{
    int i;
    BOOL match;

    for (i = 0; i < MAX_CONFIGS && o.auto_connect[i] != 0; i++)
    {
        int j;
        match = FALSE;
        for (j = 0; j < MAX_CONFIGS; j++)
        {
            if (_tcsicmp(o.conn[j].config_file, o.auto_connect[i]) == 0)
            {
                match = TRUE;
                break;
            }
        }
        if (match == FALSE)
        {
            /* autostart config not found */
            ShowLocalizedMsg(IDS_ERR_AUTOSTART_CONF, o.auto_connect[i]);
            return FALSE;
        }
    }

    return TRUE;
}


int WINAPI _tWinMain (HINSTANCE hThisInstance,
                    UNUSED HINSTANCE hPrevInstance,
                    UNUSED LPTSTR lpszArgument,
                    UNUSED int nCmdShow)
{
  MSG messages;            /* Here messages to the application are saved */
  WNDCLASSEX wincl;        /* Data structure for the windowclass */
  DWORD shell32_version;

  /* Initialize handlers for manangement interface notifications */
  mgmt_rtmsg_handler handler[] = {
      { ready,    OnReady },
      { hold,     OnHold },
      { log,      OnLogLine },
      { state,    OnStateChange },
      { password, OnPassword },
      { proxy,    OnProxy },
      { stop,     OnStop },
      { needok,   OnNeedOk },
      { needstr,  OnNeedStr },
      { echo,     OnEcho },
      { 0,        NULL }
  };
  InitManagement(handler);

  /* initialize options to default state */
  InitOptions(&o);

#ifdef DEBUG
  /* Open debug file for output */
  if (!(o.debug_fp = _wfopen(DEBUG_FILE, L"a+,ccs=UTF-8")))
    {
      /* can't open debug file */
      ShowLocalizedMsg(IDS_ERR_OPEN_DEBUG_FILE, DEBUG_FILE);
      exit(1);
    }
  PrintDebug(_T("Starting OpenVPN GUI v%S"), PACKAGE_VERSION);
#endif


  o.hInstance = hThisInstance;

  if(!GetModuleHandle(_T("RICHED20.DLL")))
    {
      LoadLibrary(_T("RICHED20.DLL"));
    }
  else
    {
      /* can't load riched20.dll */
      ShowLocalizedMsg(IDS_ERR_LOAD_RICHED20);
      exit(1);
    }

  /* Check version of shell32.dll */
  shell32_version=GetDllVersion(_T("shell32.dll"));
  if (shell32_version < PACKVERSION(5,0))
    {
      /* shell32.dll version to low */
      ShowLocalizedMsg(IDS_ERR_SHELL_DLL_VERSION, shell32_version);
      exit(1);
    }
#ifdef DEBUG
  PrintDebug(_T("Shell32.dll version: 0x%lx"), shell32_version);
#endif


  /* Check if a previous instance is already running. */
  if ((FindWindow (szClassName, NULL)) != NULL)
    {
        /* GUI already running */
        ShowLocalizedMsg(IDS_ERR_GUI_ALREADY_RUNNING);
        exit(1);
    }

  UpdateRegistry(); /* Checks version change and update keys/values */

  GetRegistryKeys();
  /* Parse command-line options */
  ProcessCommandLine(&o, GetCommandLine());

  EnsureDirExists(o.config_dir);

  if (!CheckVersion()) {
    exit(1);
  }

  if (!EnsureDirExists(o.log_dir))
  {
    ShowLocalizedMsg(IDS_ERR_CREATE_PATH, _T("log_dir"), o.log_dir);
    exit(1);
  }

  if (!IsUserAdmin() && strtod(o.ovpn_version, NULL) > 2.3 && !o.silent_connection)
    CheckIServiceStatus(TRUE);

  BuildFileList();
  if (!VerifyAutoConnections()) {
    exit(1);
  }
  GetProxyRegistrySettings();

#ifndef DISABLE_CHANGE_PASSWORD
  /* Initialize OpenSSL */
  OpenSSL_add_all_algorithms();
  ERR_load_crypto_strings();
#endif

  /* The Window structure */
  wincl.hInstance = hThisInstance;
  wincl.lpszClassName = szClassName;
  wincl.lpfnWndProc = WindowProcedure;      /* This function is called by windows */
  wincl.style = CS_DBLCLKS;                 /* Catch double-clicks */
  wincl.cbSize = sizeof (WNDCLASSEX);

  /* Use default icon and mouse-pointer */
  wincl.hIcon = LoadLocalizedIcon(ID_ICO_APP);
  wincl.hIconSm = LoadLocalizedIcon(ID_ICO_APP);
  wincl.hCursor = LoadCursor (NULL, IDC_ARROW);
  wincl.lpszMenuName = NULL;                 /* No menu */
  wincl.cbClsExtra = 0;                      /* No extra bytes after the window class */
  wincl.cbWndExtra = 0;                      /* structure or the window instance */
  /* Use Windows's default color as the background of the window */
  wincl.hbrBackground = (HBRUSH) COLOR_3DSHADOW; //COLOR_BACKGROUND;

  /* Register the window class, and if it fails quit the program */
  if (!RegisterClassEx (&wincl))
    return 1;

  /* The class is registered, let's create the program*/
  CreateWindowEx (
           0,                   /* Extended possibilites for variation */
           szClassName,         /* Classname */
           szTitleText,         /* Title Text */
           WS_OVERLAPPEDWINDOW, /* default window */
           (int)CW_USEDEFAULT,  /* Windows decides the position */
           (int)CW_USEDEFAULT,  /* where the window ends up on the screen */
           230,                 /* The programs width */
           200,                 /* and height in pixels */
           HWND_DESKTOP,        /* The window is a child-window to desktop */
           NULL,                /* No menu */
           hThisInstance,       /* Program Instance handler */
           NULL                 /* No Window Creation data */
           );


  /* Run the message loop. It will run until GetMessage() returns 0 */
  while (GetMessage (&messages, NULL, 0, 0))
  {
    TranslateMessage(&messages);
    DispatchMessage(&messages);
  }

  /* The program return-value is 0 - The value that PostQuitMessage() gave */
  return messages.wParam;
}


static void
StopAllOpenVPN()
{
    int i;

    for (i = 0; i < o.num_configs; i++)
    {
        if (o.conn[i].state != disconnected)
            StopOpenVPN(&o.conn[i]);
    }

    /* Wait for all connections to terminate (Max 5 sec) */
    for (i = 0; i < 20; i++, Sleep(250))
    {
        if (CountConnState(disconnected) == o.num_configs)
            break;
    }
}


static int
AutoStartConnections()
{
    int i;

    for (i = 0; i < o.num_configs; i++)
    {
        if (o.conn[i].auto_connect)
            StartOpenVPN(&o.conn[i]);
    }

    return TRUE;
}


static void
ResumeConnections()
{
    int i;
    for (i = 0; i < o.num_configs; i++) {
        /* Restart suspend connections */
        if (o.conn[i].state == suspended)
            StartOpenVPN(&o.conn[i]);

        /* If some connection never reached SUSPENDED state */
        if (o.conn[i].state == suspending)
            StopOpenVPN(&o.conn[i]);
    }
}

/*
 * Set scale factor of windows in pixels. Scale = 100% for dpi = 96
 */
static void
dpi_setscale(UINT dpix)
{
    /* scale factor in percentage compared to the reference dpi of 96 */
    if (dpix != 0)
        o.dpi_scale = MulDiv(dpix, 100, 96);
    else
        o.dpi_scale = 100;
    PrintDebug(L"DPI scale set to %u", o.dpi_scale);
}

/*
 * Get dpi of the system and set the scale factor.
 * The system dpi may be different from the per monitor dpi on
 * Win 8.1 later. We set dpi awareness to system-dpi level in the
 * manifest, and let Windows automatically re-scale windows
 * if/when dpi changes dynamically.
 */
static void
dpi_initialize(void)
{
    UINT dpix = 0;
    HDC hdc = GetDC(NULL);

    if (hdc)
    {
        dpix = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(NULL, hdc);
        PrintDebug(L"System DPI: dpix = %u", dpix);
    }
    else
    {
        PrintDebug(L"GetDC failed, using default dpi = 96 (error = %lu)", GetLastError());
        dpix = 96;
    }

    dpi_setscale(dpix);
}

/*  This function is called by the Windows function DispatchMessage()  */
LRESULT CALLBACK WindowProcedure (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  static UINT s_uTaskbarRestart;

  switch (message) {
    case WM_CREATE:       

      /* Save Window Handle */
      o.hWnd = hwnd;
      dpi_initialize();

      s_uTaskbarRestart = RegisterWindowMessage(TEXT("TaskbarCreated"));

      WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);

      /* Load application icon */
      HICON hIcon = LoadLocalizedIcon(ID_ICO_APP);
      if (hIcon) {
        SendMessage(hwnd, WM_SETICON, (WPARAM) (ICON_SMALL), (LPARAM) (hIcon));
        SendMessage(hwnd, WM_SETICON, (WPARAM) (ICON_BIG), (LPARAM) (hIcon));
      }

      CreatePopupMenus();	/* Create popup menus */  
      ShowTrayIcon();
      if (o.service_only)
        CheckServiceStatus();	// Check if service is running or not
      if (!AutoStartConnections()) {
        SendMessage(hwnd, WM_CLOSE, 0, 0);
        break;
      }
      break;
    	
    case WM_NOTIFYICONTRAY:
      OnNotifyTray(lParam); 	// Manages message from tray
      break;

    case WM_COMMAND:
      if ( (LOWORD(wParam) >= IDM_CONNECTMENU) && (LOWORD(wParam) < IDM_CONNECTMENU + MAX_CONFIGS) ) {
        StartOpenVPN(&o.conn[LOWORD(wParam) - IDM_CONNECTMENU]);
      }
      if ( (LOWORD(wParam) >= IDM_DISCONNECTMENU) && (LOWORD(wParam) < IDM_DISCONNECTMENU + MAX_CONFIGS) ) {
        StopOpenVPN(&o.conn[LOWORD(wParam) - IDM_DISCONNECTMENU]);
      }
      if ( (LOWORD(wParam) >= IDM_STATUSMENU) && (LOWORD(wParam) < IDM_STATUSMENU + MAX_CONFIGS) ) {
        ShowWindow(o.conn[LOWORD(wParam) - IDM_STATUSMENU].hwndStatus, SW_SHOW);
      }
      if ( (LOWORD(wParam) >= IDM_VIEWLOGMENU) && (LOWORD(wParam) < IDM_VIEWLOGMENU + MAX_CONFIGS) ) {
        ViewLog(LOWORD(wParam) - IDM_VIEWLOGMENU);
      }
      if ( (LOWORD(wParam) >= IDM_EDITMENU) && (LOWORD(wParam) < IDM_EDITMENU + MAX_CONFIGS) ) {
        EditConfig(LOWORD(wParam) - IDM_EDITMENU);
      }
      if ( (LOWORD(wParam) >= IDM_CLEARPASSMENU) && (LOWORD(wParam) < IDM_CLEARPASSMENU + MAX_CONFIGS) ) {
        DisablePasswordSave(&o.conn[LOWORD(wParam) - IDM_CLEARPASSMENU]);
      }
#ifndef DISABLE_CHANGE_PASSWORD
      if ( (LOWORD(wParam) >= IDM_PASSPHRASEMENU) && (LOWORD(wParam) < IDM_PASSPHRASEMENU + MAX_CONFIGS) ) {
        ShowChangePassphraseDialog(&o.conn[LOWORD(wParam) - IDM_PASSPHRASEMENU]);
      }
#endif
      if (LOWORD(wParam) == IDM_IMPORT) {
        ImportConfigFile();
      }
      if (LOWORD(wParam) == IDM_SETTINGS) {
        ShowSettingsDialog();
      }
      if (LOWORD(wParam) == IDM_CLOSE) {
        CloseApplication(hwnd);
      }
      if (LOWORD(wParam) == IDM_SERVICE_START) {
        MyStartService();
      }
      if (LOWORD(wParam) == IDM_SERVICE_STOP) {
        MyStopService();
      }     
      if (LOWORD(wParam) == IDM_SERVICE_RESTART) MyReStartService();
      break;
	    
    case WM_CLOSE:
      CloseApplication(hwnd);
      break;

    case WM_DESTROY:
      WTSUnRegisterSessionNotification(hwnd);
      StopAllOpenVPN();	
      OnDestroyTray();          /* Remove Tray Icon and destroy menus */
      PostQuitMessage (0);	/* Send a WM_QUIT to the message queue */
      break;

    case WM_QUERYENDSESSION:
      return(TRUE);

    case WM_ENDSESSION:
      StopAllOpenVPN();
      OnDestroyTray();
      break;

    case WM_WTSSESSION_CHANGE:
      switch (wParam) {
        case WTS_SESSION_LOCK:
          o.session_locked = TRUE;
          break;
        case WTS_SESSION_UNLOCK:
          o.session_locked = FALSE;
          if (CountConnState(suspended) != 0)
            ResumeConnections();
          break;
      }
      break;

    default:			/* for messages that we don't deal with */
      if (message == s_uTaskbarRestart)
        {
          /* Explorer has restarted, re-register the tray icon. */
          ShowTrayIcon();
          CheckAndSetTrayIcon();
          break;
        }      
      return DefWindowProc (hwnd, message, wParam, lParam);
  }

  return 0;
}


static INT_PTR CALLBACK
AboutDialogFunc(UNUSED HWND hDlg, UINT msg, UNUSED WPARAM wParam, LPARAM lParam)
{
  LPPSHNOTIFY psn;
  if (msg == WM_NOTIFY) {
    psn = (LPPSHNOTIFY) lParam;
    if (psn->hdr.code == (UINT) PSN_APPLY)
        return TRUE;
  }
  return FALSE;
}


static void
ShowSettingsDialog()
{
  PROPSHEETPAGE psp[4];
  int page_number = 0;

  /* General tab */
  psp[page_number].dwSize = sizeof(PROPSHEETPAGE);
  psp[page_number].dwFlags = PSP_DLGINDIRECT;
  psp[page_number].hInstance = o.hInstance;
  psp[page_number].pResource = LocalizedDialogResource(ID_DLG_GENERAL);
  psp[page_number].pfnDlgProc = GeneralSettingsDlgProc;
  psp[page_number].lParam = 0;
  psp[page_number].pfnCallback = NULL;
  ++page_number;

  /* Proxy tab */
  if (o.service_only == 0) {
    psp[page_number].dwSize = sizeof(PROPSHEETPAGE);
    psp[page_number].dwFlags = PSP_DLGINDIRECT;
    psp[page_number].hInstance = o.hInstance;
    psp[page_number].pResource = LocalizedDialogResource(ID_DLG_PROXY);
    psp[page_number].pfnDlgProc = ProxySettingsDialogFunc;
    psp[page_number].lParam = 0;
    psp[page_number].pfnCallback = NULL;
    ++page_number;
  }

  /* Advanced tab */
  psp[page_number].dwSize = sizeof(PROPSHEETPAGE);
  psp[page_number].dwFlags = PSP_DLGINDIRECT;
  psp[page_number].hInstance = o.hInstance;
  psp[page_number].pResource = LocalizedDialogResource(ID_DLG_ADVANCED);
  psp[page_number].pfnDlgProc = AdvancedSettingsDlgProc;
  psp[page_number].lParam = 0;
  psp[page_number].pfnCallback = NULL;
  ++page_number;

  /* About tab */
  psp[page_number].dwSize = sizeof(PROPSHEETPAGE);
  psp[page_number].dwFlags = PSP_DLGINDIRECT;
  psp[page_number].hInstance = o.hInstance;
  psp[page_number].pResource = LocalizedDialogResource(ID_DLG_ABOUT);
  psp[page_number].pfnDlgProc = AboutDialogFunc;
  psp[page_number].lParam = 0;
  psp[page_number].pfnCallback = NULL;
  ++page_number;

  PROPSHEETHEADER psh;
  psh.dwSize = sizeof(PROPSHEETHEADER);
  psh.dwFlags = PSH_USEHICON | PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW | PSH_NOCONTEXTHELP;
  psh.hwndParent = o.hWnd;
  psh.hInstance = o.hInstance;
  psh.hIcon = LoadLocalizedIcon(ID_ICO_APP);
  psh.pszCaption = LoadLocalizedString(IDS_SETTINGS_CAPTION);
  psh.nPages = page_number;
  psh.nStartPage = 0;
  psh.ppsp = (LPCPROPSHEETPAGE) &psp;
  psh.pfnCallback = NULL;

  PropertySheet(&psh);
}


void
CloseApplication(HWND hwnd)
{
    int i;

    if (o.service_state == service_connected
    && ShowLocalizedMsgEx(MB_YESNO, _T("Exit OpenVPN"), IDS_NFO_SERVICE_ACTIVE_EXIT) == IDNO)
            return;

    for (i = 0; i < o.num_configs; i++)
    {
        if (o.conn[i].state == disconnected)
            continue;

        /* Ask for confirmation if still connected */
        if (ShowLocalizedMsgEx(MB_YESNO, _T("Exit OpenVPN"), IDS_NFO_ACTIVE_CONN_EXIT) == IDNO)
            return;
    }

    DestroyWindow(hwnd);
}

void
ImportConfigFile()
{
    
    TCHAR filter[2*_countof(o.ext_string)+5];

    _sntprintf_0(filter, _T("*.%s%c*.%s%c"), o.ext_string, _T('\0'), o.ext_string, _T('\0'));
    
    OPENFILENAME fn;
    TCHAR source[MAX_PATH] = _T("");

    fn.lStructSize = sizeof(OPENFILENAME);
    fn.hwndOwner = NULL;
    fn.lpstrFilter = filter;
    fn.lpstrCustomFilter = NULL;
    fn.nFilterIndex = 1;
    fn.lpstrFile = source;
    fn.nMaxFile = MAX_PATH;
    fn.lpstrFileTitle = NULL;
    fn.lpstrInitialDir = NULL;
    fn.lpstrTitle = NULL;
    fn.Flags = OFN_DONTADDTORECENT | OFN_FILEMUSTEXIST;
    fn.lpstrDefExt = NULL;

    if (GetOpenFileName(&fn)) 
    {

        TCHAR destination[MAX_PATH];
        PTCHAR fileName = source + fn.nFileOffset;

        _sntprintf_0(destination, _T("%s\\%s"), o.config_dir, fileName);

        destination[_tcslen(destination) - _tcslen(o.ext_string) - 1] = _T('\0');
        
        if (EnsureDirExists(destination))
        {

            _sntprintf_0(destination, _T("%s\\%s"), destination, fileName);
            
            if (!CopyFile(source, destination, TRUE))
            {
                if (GetLastError() == ERROR_FILE_EXISTS)
                {
                    fileName[_tcslen(fileName) - _tcslen(o.ext_string) - 1] = _T('\0');
                    ShowLocalizedMsg(IDS_ERR_IMPORT_EXISTS, fileName);
                    return;
                }
            }
            else
            {
                ShowLocalizedMsg(IDS_NFO_IMPORT_SUCCESS);
                return;
            }

        }

        ShowLocalizedMsg(IDS_ERR_IMPORT_FAILED, destination);

    }
    
}

#ifdef DEBUG
void PrintDebugMsg(TCHAR *msg)
{
  time_t log_time;
  struct tm *time_struct;
  TCHAR date[30];

  log_time = time(NULL);
  time_struct = localtime(&log_time);
  _sntprintf(date, _countof(date), _T("%d-%.2d-%.2d %.2d:%.2d:%.2d"),
                 time_struct->tm_year + 1900,
                 time_struct->tm_mon + 1,
                 time_struct->tm_mday,
                 time_struct->tm_hour,
                 time_struct->tm_min,
                 time_struct->tm_sec);

  _ftprintf(o.debug_fp, _T("%s %s\n"), date, msg);
  fflush(o.debug_fp);
}
#endif

#define PACKVERSION(major,minor) MAKELONG(minor,major)
DWORD GetDllVersion(LPCTSTR lpszDllName)
{
    HINSTANCE hinstDll;
    DWORD dwVersion = 0;

    /* For security purposes, LoadLibrary should be provided with a 
       fully-qualified path to the DLL. The lpszDllName variable should be
       tested to ensure that it is a fully qualified path before it is used. */
    hinstDll = LoadLibrary(lpszDllName);
	
    if(hinstDll)
    {
        DLLGETVERSIONPROC pDllGetVersion;
        pDllGetVersion = (DLLGETVERSIONPROC)GetProcAddress(hinstDll, 
                          "DllGetVersion");

        /* Because some DLLs might not implement this function, you
        must test for it explicitly. Depending on the particular 
        DLL, the lack of a DllGetVersion function can be a useful
        indicator of the version. */

        if(pDllGetVersion)
        {
            DLLVERSIONINFO dvi;
            HRESULT hr;

            ZeroMemory(&dvi, sizeof(dvi));
            dvi.cbSize = sizeof(dvi);

            hr = (*pDllGetVersion)(&dvi);

            if(SUCCEEDED(hr))
            {
               dwVersion = PACKVERSION(dvi.dwMajorVersion, dvi.dwMinorVersion);
            }
        }

        FreeLibrary(hinstDll);
    }
    return dwVersion;
}
