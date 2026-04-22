
// MessageControlDlg.cpp: 实现文件
//

#include "pch.h"
#include "framework.h"
#include "MessageControl.h"
#include "MessageControlDlg.h"
#include "afxdialogex.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif


#define SATACOPY_CLASSNAME _T("SataCopyApp")

#define WM_COPY_ORDER_NUMBER	WM_USER + 1001
#define WM_COPY_START			WM_USER + 1002
#define WM_COPY_STOP			WM_USER + 1003
#define WM_COPY_WORK_MODE		WM_USER + 1004

typedef enum WORK_MODE
{
	QUICK_COPY = 0,			// 快速拷贝
	QUICK_COPY_COMPARE = 1,	// 快速拷贝+比对
	FULL_COPY = 2,			// 全盘拷贝
	FULLL_COPY_COMPARE = 3, // 全盘拷贝+比对
	MAKE_MAP = 6,			// 制作镜像
	MAP_COPY = 7,			// 镜像拷贝
};


// 用于应用程序“关于”菜单项的 CAboutDlg 对话框

class CAboutDlg : public CDialogEx
{
public:
	CAboutDlg();

// 对话框数据
#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_ABOUTBOX };
#endif

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV 支持

// 实现
protected:
	DECLARE_MESSAGE_MAP()
};

CAboutDlg::CAboutDlg() : CDialogEx(IDD_ABOUTBOX)
{
}

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialogEx)
END_MESSAGE_MAP()


// CMessageControlDlg 对话框



CMessageControlDlg::CMessageControlDlg(CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_MESSAGECONTROL_DIALOG, pParent)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CMessageControlDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_COMBO_WORK_MODE, m_comboWorkMode);
	DDX_Control(pDX, IDC_EDIT1, m_editWO);
}

BEGIN_MESSAGE_MAP(CMessageControlDlg, CDialogEx)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_BN_CLICKED(IDOK, &CMessageControlDlg::OnBnClickedOk)
	ON_BN_CLICKED(IDC_BUTTON_START, &CMessageControlDlg::OnBnClickedButtonStart)
	ON_BN_CLICKED(IDC_BUTTON_STOP, &CMessageControlDlg::OnBnClickedButtonStop)
	ON_BN_CLICKED(IDC_BUTTON_SET_WORK_MODE, &CMessageControlDlg::OnBnClickedButtonSetWorkMode)
	ON_BN_CLICKED(IDC_BUTTON_SET_WO, &CMessageControlDlg::OnBnClickedButtonSetWo)
END_MESSAGE_MAP()


// CMessageControlDlg 消息处理程序

BOOL CMessageControlDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	// 将“关于...”菜单项添加到系统菜单中。

	// IDM_ABOUTBOX 必须在系统命令范围内。
	ASSERT((IDM_ABOUTBOX & 0xFFF0) == IDM_ABOUTBOX);
	ASSERT(IDM_ABOUTBOX < 0xF000);

	CMenu* pSysMenu = GetSystemMenu(FALSE);
	if (pSysMenu != nullptr)
	{
		BOOL bNameValid;
		CString strAboutMenu;
		bNameValid = strAboutMenu.LoadString(IDS_ABOUTBOX);
		ASSERT(bNameValid);
		if (!strAboutMenu.IsEmpty())
		{
			pSysMenu->AppendMenu(MF_SEPARATOR);
			pSysMenu->AppendMenu(MF_STRING, IDM_ABOUTBOX, strAboutMenu);
		}
	}

	// 设置此对话框的图标。  当应用程序主窗口不是对话框时，框架将自动
	//  执行此操作
	SetIcon(m_hIcon, TRUE);			// 设置大图标
	SetIcon(m_hIcon, FALSE);		// 设置小图标

	// TODO: 在此添加额外的初始化代码
	m_comboWorkMode.AddString(_T("快速拷贝"));
	m_comboWorkMode.AddString(_T("快速拷贝+比对"));
	m_comboWorkMode.AddString(_T("全盘拷贝"));
	m_comboWorkMode.AddString(_T("全盘拷贝+比对"));
	m_comboWorkMode.AddString(_T("制作镜像"));
	m_comboWorkMode.AddString(_T("镜像拷贝"));
	m_comboWorkMode.SetCurSel(5);

	return TRUE;  // 除非将焦点设置到控件，否则返回 TRUE
}

void CMessageControlDlg::OnSysCommand(UINT nID, LPARAM lParam)
{
	if ((nID & 0xFFF0) == IDM_ABOUTBOX)
	{
		CAboutDlg dlgAbout;
		dlgAbout.DoModal();
	}
	else
	{
		CDialogEx::OnSysCommand(nID, lParam);
	}
}

// 如果向对话框添加最小化按钮，则需要下面的代码
//  来绘制该图标。  对于使用文档/视图模型的 MFC 应用程序，
//  这将由框架自动完成。

void CMessageControlDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this); // 用于绘制的设备上下文

		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);

		// 使图标在工作区矩形中居中
		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;

		// 绘制图标
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialogEx::OnPaint();
	}
}

//当用户拖动最小化窗口时系统调用此函数取得光标
//显示。
HCURSOR CMessageControlDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}



void CMessageControlDlg::OnBnClickedOk()
{
	// TODO: 在此添加控件通知处理程序代码
	CDialogEx::OnOK();
}


void CMessageControlDlg::OnBnClickedButtonStart()
{
	HWND hWndTarget = ::FindWindow(SATACOPY_CLASSNAME, NULL);
	if (hWndTarget)
	{
		::PostMessage(hWndTarget, WM_COPY_START, 0, 0);
	}
	else
	{
		MessageBox(_T("TAMonitor Window not found!"));
	}
}


void CMessageControlDlg::OnBnClickedButtonStop()
{
	HWND hWndTarget = ::FindWindow(SATACOPY_CLASSNAME, NULL);
	if (hWndTarget)
	{
		::PostMessage(hWndTarget, WM_COPY_STOP, 0, 0);
	}
	else
	{
		MessageBox(_T("TAMonitor Window not found!"));
	}
}


void CMessageControlDlg::OnBnClickedButtonSetWorkMode()
{
	HWND hWndTarget = ::FindWindow(SATACOPY_CLASSNAME, NULL);
	WORK_MODE mode = MAP_COPY;
	switch (m_comboWorkMode.GetCurSel())
	{
	case 0: // 快速拷贝
		mode = QUICK_COPY;
		break;
	case 1: // 快速拷贝+比对
		mode = QUICK_COPY_COMPARE;
		break;
	case 2: // 全盘拷贝
		mode = FULL_COPY;
		break;
	case 3: // 全盘拷贝+比对
		mode = FULLL_COPY_COMPARE;
		break;
	case 4: // 制作镜像
		mode = MAKE_MAP;
		break;
	case 5: // 镜像拷贝
		mode = MAP_COPY;
		break;
	}
	if (hWndTarget)
	{
		::PostMessage(hWndTarget, WM_COPY_WORK_MODE, 0, (LPARAM)mode);
	}
	else
	{
		MessageBox(_T("TAMonitor Window not found!"));
	}
}


void CMessageControlDlg::OnBnClickedButtonSetWo()
{
	CString strWo;
	m_editWO.GetWindowText(strWo);

	// 管理員權限通信, 共享內存方式, 接收端GlobalLock出來的字串地址是NULL, 改用文字檔方式傳送字串
	//HGLOBAL hMem = GlobalAlloc(GHND | GMEM_SHARE, (strWo.GetLength() + 1) * sizeof(wchar_t));
	//if (!hMem)
	//{
	//	MessageBox(_T("Memory allocation failed!"));
	//	return;
	//}
	//wchar_t* pGlobal = (wchar_t*)GlobalLock(hMem);
	//if (pGlobal)
	//{
	//	lstrcpyW(pGlobal, strWo.GetBuffer());
	//	GlobalUnlock(hMem);
	//}

	// 將工單號寫入文字檔, 通知拷貝機讀取
	CString strFilePath = _T("E:\\OrderNumber.txt");
	CStdioFile fileTxt;
	CFileException fileException;
	if (fileTxt.Open(strFilePath, CFile::modeCreate | CFile::modeWrite | CFile::typeText, &fileException))
	{
		fileTxt.WriteString(strWo);
		fileTxt.Close();
	}

	HWND hWndTarget = ::FindWindow(SATACOPY_CLASSNAME, NULL);
	if (!hWndTarget)
	{
		MessageBox(_T("Target window not found!"));
		//GlobalFree(hMem);
		return;
	}

	BOOL bRet = ::PostMessage(hWndTarget, WM_USER + 1001, 0, 0);
	if (!bRet)
	{
		MessageBox(_T("Message Fail !!!"));
		//GlobalFree(hMem);  // 必須釋放！
	}
	// 接收方需負責呼叫 GlobalFree(hMem)
}