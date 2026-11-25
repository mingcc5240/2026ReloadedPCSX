VERSION 5.00
Begin VB.Form Form1 
   Caption         =   "Form1"
   ClientHeight    =   11145
   ClientLeft      =   60
   ClientTop       =   405
   ClientWidth     =   18120
   LinkTopic       =   "Form1"
   ScaleHeight     =   743
   ScaleMode       =   3  '픽셀
   ScaleWidth      =   1208
   StartUpPosition =   3  'Windows 기본값
   Begin VB.CommandButton Command1 
      Caption         =   "AudioSet"
      Height          =   495
      Left            =   16200
      TabIndex        =   6
      Top             =   6120
      Width           =   1695
   End
   Begin VB.CommandButton cmdStop 
      Caption         =   "Stop"
      Height          =   615
      Left            =   16200
      TabIndex        =   4
      Top             =   3720
      Width           =   1815
   End
   Begin VB.CommandButton cmdRun 
      Caption         =   "Run"
      Height          =   735
      Left            =   16200
      TabIndex        =   3
      Top             =   2760
      Width           =   1815
   End
   Begin VB.CommandButton cmdBrowse 
      Caption         =   "Select"
      Height          =   615
      Left            =   16200
      TabIndex        =   2
      Top             =   1800
      Width           =   1695
   End
   Begin VB.TextBox txtIsoPath 
      Height          =   855
      Left            =   15840
      TabIndex        =   1
      Text            =   "Text1"
      Top             =   600
      Width           =   2175
   End
   Begin VB.PictureBox picScreen 
      Height          =   10935
      Left            =   120
      ScaleHeight     =   725
      ScaleMode       =   3  '픽셀
      ScaleWidth      =   1037
      TabIndex        =   0
      Top             =   120
      Width           =   15615
   End
   Begin VB.Label lblStatus 
      Caption         =   "Label1"
      Height          =   975
      Left            =   16080
      TabIndex        =   5
      Top             =   4800
      Width           =   2055
      WordWrap        =   -1  'True
   End
End
Attribute VB_Name = "Form1"
Attribute VB_GlobalNameSpace = False
Attribute VB_Creatable = False
Attribute VB_PredeclaredId = True
Attribute VB_Exposed = False
Option Explicit

Private m_IsInit As Boolean
Private m_IsRunning As Boolean
Private m_BaseDir As String

Private Sub Command1_Click()
 
    If m_IsRunning Then
        MsgBox "게임을 중지(Stop)한 후 오디오 설정을 변경하세요.", vbInformation
        Exit Sub
    End If
    PSX_ConfigSPU

End Sub

Private Sub Form_Load()
    Dim ret As Long

    m_BaseDir = App.Path
    If Right$(m_BaseDir, 1) <> "\" Then m_BaseDir = m_BaseDir & "\"

    Me.Caption = "VB6 PSX Runner (PCSXR Core)"
    picScreen.BackColor = vbBlack
    cmdRun.Enabled = False
    cmdStop.Enabled = False

    ' 플러그인 파일명은 실제 plugins 폴더에 있는 DLL 이름에 맞게 수정하세요
    ret = PSX_Init( _
        picScreen.hwnd, _
        m_BaseDir & "plugins\", _
        m_BaseDir & "bios\", _
        "minigl_gpu.dll", _
        "spuPeopsDSound.dll", _
        "DFCdrom.dll", _
        "padSSSPSX.dll")

    If ret <> 0 Then
        lblStatus.Caption = "초기화 실패 (오류 코드: " & ret & ")"
        MsgBox "PSX Core Init Fail!! (code: " & ret & ")" & vbCrLf & _
               "plugins 폴더 내 DLL 파일명을 확인하세요.", vbCritical
        m_IsInit = False
    Else
        m_IsInit = True
        cmdRun.Enabled = True
        lblStatus.Caption = "준비 완료 - ISO/BIN 파일을 선택하세요."
    End If
End Sub

Private Sub cmdBrowse_Click()
    Dim picked As String
    picked = SelectIsoFile(Me.hwnd, m_BaseDir)
    If Len(picked) > 0 Then
        txtIsoPath.Text = picked
        lblStatus.Caption = "선택됨: " & picked
    End If
End Sub

Private Sub cmdRun_Click()
    Dim isoPath As String
    Dim ret As Long

    If Not m_IsInit Or m_IsRunning Then Exit Sub

    isoPath = Trim$(txtIsoPath.Text)
    If Len(isoPath) = 0 Or Dir$(isoPath) = "" Then
        MsgBox "유효한 ISO/BIN 파일을 먼저 선택하세요.", vbExclamation
        Exit Sub
    End If

    ret = PSX_RunISO(isoPath)
    If ret <> 0 Then
        lblStatus.Caption = "실행 실패 (코드: " & ret & ")"
        MsgBox "Game Error! (code: " & ret & ")", vbCritical
        Exit Sub
    End If

    m_IsRunning = True
    cmdRun.Enabled = False
    cmdBrowse.Enabled = False
    cmdStop.Enabled = True
    lblStatus.Caption = "실행 중..."

    ' 패드/키보드 입력이 전달되도록 화면 컨트롤에 포커스 이동
    SetFocusAPI picScreen.hwnd
End Sub

Private Sub cmdStop_Click()
    If Not m_IsRunning Then Exit Sub

    PSX_Stop
    m_IsRunning = False
    cmdRun.Enabled = True
    cmdBrowse.Enabled = True
    cmdStop.Enabled = False
    picScreen.Cls
    lblStatus.Caption = "중지됨"
End Sub

Private Sub picScreen_Click()
    ' 실행 중 화면을 클릭하면 다시 입력 포커스 확보
    If m_IsRunning Then SetFocusAPI picScreen.hwnd
End Sub

Private Sub Form_Unload(Cancel As Integer)
    If m_IsRunning Then
        PSX_Stop
        m_IsRunning = False
    End If
    If m_IsInit Then
        PSX_Shutdown
        m_IsInit = False
    End If
End Sub

