#pragma once

#define IDI_APP_ICON            101
#define IDR_MAIN_MENU           102
#define IDD_ADD_DOWNLOAD_DIALOG 103

// Raw payload: app icon bytes followed by the appended Tor LZX cabinet.
// Must be RCDATA, never ICON - the resource compiler strips trailing bytes from
// an ICON resource, which would discard the cabinet.
#define IDR_TOR_PAYLOAD         104

#define IDC_STATIC              -1

// Menu commands
#define IDM_FILE_ADD            40001
#define IDM_FILE_EXIT           40002
#define IDM_DOWNLOAD_START      40003
#define IDM_DOWNLOAD_PAUSE      40004
#define IDM_DOWNLOAD_REMOVE     40005
#define IDM_VIEW_DARK_MODE      40006
#define IDM_HELP_ABOUT          40007

// UI Control IDs
#define IDC_LIST_DOWNLOADS      50001
#define IDC_EDIT_URL            50002
#define IDC_BTN_START           50003
#define IDC_BTN_PAUSE           50004
#define IDC_BTN_DELETE          50005
#define IDC_STATUS_BAR          50006
#define IDC_PROGRESS_BAR        50007
#define IDC_STATIC_SPEED        50008
#define IDC_STATIC_STATUS       50009
#define IDC_BTN_PASTE           50010
