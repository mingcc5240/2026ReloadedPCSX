Attribute VB_Name = "modPSX"
Option Explicit

' --- pcsx_vb.dll 익스포트 함수 ---
Public Declare Function PSX_Init Lib "pcsx_vb.dll" ( _
    ByVal targetHwnd As Long, _
    ByVal pluginsDir As String, _
    ByVal biosPath As String, _
    ByVal gpuDll As String, _
    ByVal spuDll As String, _
    ByVal cdrDll As String, _
    ByVal padDll As String) As Long

Public Declare Function PSX_RunISO Lib "pcsx_vb.dll" ( _
    ByVal isoPath As String) As Long

Public Declare Sub PSX_Stop Lib "pcsx_vb.dll" ()
Public Declare Sub PSX_Shutdown Lib "pcsx_vb.dll" ()
Public Declare Sub PSX_ConfigSPU Lib "pcsx_vb.dll" ()
' --- Win32 API (파일 선택 대화상자 & 포커스) ---
Private Type OPENFILENAME
    lStructSize As Long
    hwndOwner As Long
    hInstance As Long
    lpstrFilter As String
    lpstrCustomFilter As String
    nMaxCustFilter As Long
    nFilterIndex As Long
    lpstrFile As String
    nMaxFile As Long
    lpstrFileTitle As String
    nMaxFileTitle As Long
    lpstrInitialDir As String
    lpstrTitle As String
    Flags As Long
    nFileOffset As Integer
    nFileExtension As Integer
    lpstrDefExt As String
    lCustData As Long
    lpfnHook As Long
    lpTemplateName As String
End Type

Private Declare Function GetOpenFileNameA Lib "comdlg32.dll" (pOpenfilename As OPENFILENAME) As Long
Public Declare Function SetFocusAPI Lib "user32" Alias "SetFocus" (ByVal hwnd As Long) As Long

Private Const OFN_FILEMUSTEXIST As Long = &H1000
Private Const OFN_PATHMUSTEXIST As Long = &H800
Private Const OFN_HIDEREADONLY As Long = &H4

Public Function SelectIsoFile(ByVal ownerHwnd As Long, ByVal initDir As String) As String
    Dim ofn As OPENFILENAME
    Dim buf As String * 260
    Dim nulPos As Long

    buf = String$(260, vbNullChar)

    With ofn
        .lStructSize = Len(ofn)
        .hwndOwner = ownerHwnd
        .hInstance = App.hInstance
        .lpstrFilter = "PSX Image (*.bin;*.iso;*.img;*.cue)" & vbNullChar & "*.bin;*.iso;*.img;*.cue" & vbNullChar & _
                       "All Files (*.*)" & vbNullChar & "*.*" & vbNullChar & vbNullChar
        .lpstrFile = buf
        .nMaxFile = Len(buf)
        .lpstrInitialDir = initDir
        .lpstrTitle = "PSX 게임 이미지 선택"
        .Flags = OFN_FILEMUSTEXIST Or OFN_PATHMUSTEXIST Or OFN_HIDEREADONLY
    End With

    If GetOpenFileNameA(ofn) <> 0 Then
        nulPos = InStr(ofn.lpstrFile, vbNullChar)
        If nulPos > 1 Then
            SelectIsoFile = Left$(ofn.lpstrFile, nulPos - 1)
        End If
    End If
End Function

