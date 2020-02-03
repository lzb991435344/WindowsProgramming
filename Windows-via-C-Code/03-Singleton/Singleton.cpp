/******************************************************************************
Module:  Singleton.cpp
Notices: Copyright (c) 2008 Jeffrey Richter & Christophe Nasarre
******************************************************************************/


#include "resource.h"

#include "..\CommonFiles\CmnHdr.h"     /* See Appendix A. */
#include <windowsx.h>
#include <Sddl.h>          // for SID management
#include <tchar.h>
#include <strsafe.h>

/**
跨进程边界共享内核对象
(1)使用对象句柄继承
(2)为对象命名
(3)复制对象句柄

*/


//创建专属的命名空间

/**
 创建内核对象的时候，可以传递一个指向SECURITY_ATTRIBUTE结构的指针
 
 (1)如何创建一个边界
 (2)如何将一个应用于本地管理员组的一个sid和它关联起来
 (3)如何创建或打开其名称被用作互斥量内核对象前缀的一个专有命名空间


 目的：边界描述符会获得一个名称，获得与特权用户组关联的sid
 window只有在该用户隶属于特权组时，在相同边界中创建命名空间，访问内核对象
*/
///////////////////////////////////////////////////////////////////////////////


// Main dialog
HWND     g_hDlg;

// Mutex, boundary and namespace used to detect previous running instance
HANDLE   g_hSingleton = NULL;
HANDLE   g_hBoundary = NULL;
HANDLE   g_hNamespace = NULL;

// Keep track whether or not the namespace was created or open for clean-up
BOOL     g_bNamespaceOpened = FALSE;

// Names of boundary and private namespace
//创建边界描述符，参数1：字符串标识符来定义范围
PCTSTR   g_szBoundary = TEXT("3-Boundary");
PCTSTR   g_szNamespace = TEXT("3-Namespace");


#define DETAILS_CTRL GetDlgItem(g_hDlg, IDC_EDIT_DETAILS)


///////////////////////////////////////////////////////////////////////////////


// Adds a string to the "Details" edit control
void AddText(PCTSTR pszFormat, ...) {

   va_list argList;
   va_start(argList, pszFormat);

   TCHAR sz[20 * 1024];

   Edit_GetText(DETAILS_CTRL, sz, _countof(sz));
   _vstprintf_s(
      _tcschr(sz, TEXT('\0')), _countof(sz) - _tcslen(sz), 
      pszFormat, argList);
   Edit_SetText(DETAILS_CTRL, sz);
   va_end(argList);
}


///////////////////////////////////////////////////////////////////////////////


void Dlg_OnCommand(HWND hwnd, int id, HWND hwndCtl, UINT codeNotify) {

   switch (id) {
      case IDOK:
      case IDCANCEL:
         // User has clicked on the Exit button
         // or dismissed the dialog with ESCAPE
         EndDialog(hwnd, id);
         break;
   }
}


///////////////////////////////////////////////////////////////////////////////


void CheckInstances() {

   // Create the boundary descriptor
   //HANDLE CreateBoundaryDescriptor(PCTSTR pszName, DWORD dwFlags)
   //功能：创建边界描述符
   //参数1：字符串标识符
   //参数2:0
   //返回值：指针，指向用户模式的结构
   //PS：不要将返回的句柄传入CloseHandle(),而是DeleteBoundaryDescriptor()
   g_hBoundary = CreateBoundaryDescriptor(g_szBoundary, 0);

   // Create a SID corresponding to the Local Administrator group
   BYTE localAdminSID[SECURITY_MAX_SID_SIZE];
   PSID pLocalAdminSID = &localAdminSID;
   DWORD cbSID = sizeof(localAdminSID);

   //创建一个安全描述符sid
   if (!CreateWellKnownSid(
      WinBuiltinAdministratorsSid, NULL, pLocalAdminSID, &cbSID)
      ) {
      AddText(TEXT("AddSIDToBoundaryDescriptor failed: %u\r\n"), 
         GetLastError());
      return;
   }
   
   // Associate the Local Admin SID to the boundary descriptor
   // --> only applications running under an administrator user
   //     will be able to access the kernel objects in the same namespace
   //BOOL AddSIDToBoundaryDescriptor(HANDLE* phBoundaryDescription, PSID pRequireSid)
   //功能：将一个特权用户的SID关联边界描述符
   //参数1：边界描述符  参数2：SID
   //返回值：BOOL
   if (!AddSIDToBoundaryDescriptor(&g_hBoundary, pLocalAdminSID)) {
      AddText(TEXT("AddSIDToBoundaryDescriptor failed: %u\r\n"), 
         GetLastError()); //获取错误码
      return;
   }

   // Create the namespace for Local Administrators only
   SECURITY_ATTRIBUTES sa;
   sa.nLength = sizeof(sa);//字节数
   sa.bInheritHandle = FALSE;//无法继承

   //参数1：安全描述字符串
   //ConvertStringSecurityDescriptorToSecurityDescriptor构造SECURITY_ATTRIBUTES
   if (!ConvertStringSecurityDescriptorToSecurityDescriptor(
      TEXT("D:(A;;GA;;;BA)"), 
      SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL)) {
      AddText(TEXT("Security Descriptor creation failed: %u\r\n"), GetLastError());
      return;
   }

   //创建专有命名空间，已有，返回NULL
   g_hNamespace = 
      CreatePrivateNamespace(&sa, g_hBoundary, g_szNamespace);

   // Don't forget to release memory for the security descriptor
   LocalFree(sa.lpSecurityDescriptor);


   // Check the private namespace creation result
   DWORD dwLastError = GetLastError();
   if (g_hNamespace == NULL) {
      // Nothing to do if access is denied
      // --> this code must run under a Local Administrator account
      if (dwLastError == ERROR_ACCESS_DENIED) {
         AddText(TEXT("Access denied when creating the namespace.\r\n"));
         AddText(TEXT("   You must be running as Administrator.\r\n\r\n"));
         return;
      } else { 
         if (dwLastError == ERROR_ALREADY_EXISTS) {
         // If another instance has already created the namespace, 
         // we need to open it instead. 
            AddText(TEXT("CreatePrivateNamespace failed: %u\r\n"), dwLastError);

            //功能：打开现有的专有命名空间
            g_hNamespace = OpenPrivateNamespace(g_hBoundary, g_szNamespace);
            if (g_hNamespace == NULL) {
               AddText(TEXT("   and OpenPrivateNamespace failed: %u\r\n"), 
               dwLastError);
               return;
            } else {
               g_bNamespaceOpened = TRUE;
               AddText(TEXT("   but OpenPrivateNamespace succeeded\r\n\r\n"));
            }
         } else {
            AddText(TEXT("Unexpected error occured: %u\r\n\r\n"),
               dwLastError);
            return;
         }
      }
   }
   
   // Try to create the mutex object with a name 
   // based on the private namespace 
   TCHAR szMutexName[64];
   StringCchPrintf(szMutexName, _countof(szMutexName), TEXT("%s\\%s"), 
      g_szNamespace, TEXT("Singleton"));

   g_hSingleton = CreateMutex(NULL, FALSE, szMutexName);
   if (GetLastError() == ERROR_ALREADY_EXISTS) {
      // There is already an instance of this Singleton object
      AddText(TEXT("Another instance of Singleton is running:\r\n"));
      AddText(TEXT("--> Impossible to access application features.\r\n"));
   } else  {
      // First time the Singleton object is created
      AddText(TEXT("First instance of Singleton:\r\n"));
      AddText(TEXT("--> Access application features now.\r\n"));
   }
}


///////////////////////////////////////////////////////////////////////////////


BOOL Dlg_OnInitDialog(HWND hwnd, HWND hwndFocus, LPARAM lParam) {

   chSETDLGICONS(hwnd, IDI_SINGLETON);

   // Keep track of the main dialog window handle
   g_hDlg = hwnd;

   // Check whether another instance is already running
   CheckInstances();
      
   return(TRUE);
}


///////////////////////////////////////////////////////////////////////////////


INT_PTR WINAPI Dlg_Proc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {

   switch (uMsg) {
      chHANDLE_DLGMSG(hwnd, WM_COMMAND,    Dlg_OnCommand);
      chHANDLE_DLGMSG(hwnd, WM_INITDIALOG, Dlg_OnInitDialog);
   }

   return(FALSE);
}


///////////////////////////////////////////////////////////////////////////////


int APIENTRY _tWinMain(HINSTANCE hInstance,
                     HINSTANCE hPrevInstance,
                     LPTSTR    lpCmdLine,
                     int       nCmdShow)
{
   UNREFERENCED_PARAMETER(hPrevInstance);
   UNREFERENCED_PARAMETER(lpCmdLine);

   // Show main window 
   DialogBox(hInstance, MAKEINTRESOURCE(IDD_SINGLETON), NULL, Dlg_Proc);

   // Don't forget to clean up and release kernel resources
   if (g_hSingleton != NULL) {
      CloseHandle(g_hSingleton);
   }

   if (g_hNamespace != NULL) {
      if (g_bNamespaceOpened) {  
         // Open namespace
         //0 关闭后仍然可见
         ClosePrivateNamespace(g_hNamespace, 0);
      } else { // Created namespace
         //PRIVATE_NAMESPACE_FLAG_DESTROY 关闭后不可见
         ClosePrivateNamespace(g_hNamespace, PRIVATE_NAMESPACE_FLAG_DESTROY);
      }
   }

   if (g_hBoundary != NULL) {
      DeleteBoundaryDescriptor(g_hBoundary);
   }

   return(0);
}


//////////////////////////////// End of File //////////////////////////////////
