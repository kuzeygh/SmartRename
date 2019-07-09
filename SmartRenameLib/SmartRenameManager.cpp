#include "stdafx.h"
#include "SmartRenameManager.h"
#include "SmartRenameRegEx.h" // Default RegEx handler
#include <algorithm>
#include <shlobj.h>
#include "helpers.h"
#include <filesystem>
namespace fs = std::filesystem;

// The default FOF flags to use in the rename operations
#define FOF_DEFAULTFLAGS (FOF_ALLOWUNDO | FOFX_SHOWELEVATIONPROMPT | FOF_RENAMEONCOLLISION)

IFACEMETHODIMP_(ULONG) CSmartRenameManager::AddRef()
{
    return InterlockedIncrement(&m_refCount);
}

IFACEMETHODIMP_(ULONG) CSmartRenameManager::Release()
{
    long refCount = InterlockedDecrement(&m_refCount);

    if (refCount == 0)
    {
        delete this;
    }
    return refCount;
}

IFACEMETHODIMP CSmartRenameManager::QueryInterface(_In_ REFIID riid, _Outptr_ void** ppv)
{
    static const QITAB qit[] = {
        QITABENT(CSmartRenameManager, ISmartRenameManager),
        QITABENT(CSmartRenameManager, ISmartRenameRegExEvents),
        { 0 }
    };
    return QISearch(this, qit, riid, ppv);
}

IFACEMETHODIMP CSmartRenameManager::Advise(_In_ ISmartRenameManagerEvents* renameOpEvents, _Out_ DWORD* cookie)
{
    CSRWExclusiveAutoLock lock(&m_lockEvents);
    m_cookie++;
    SMART_RENAME_MGR_EVENT srme;
    srme.cookie = m_cookie;
    srme.pEvents = renameOpEvents;
    renameOpEvents->AddRef();
    m_SmartRenameManagerEvents.push_back(srme);

    *cookie = m_cookie;

    return S_OK;
}

IFACEMETHODIMP CSmartRenameManager::UnAdvise(_In_ DWORD cookie)
{
    HRESULT hr = E_FAIL;
    CSRWExclusiveAutoLock lock(&m_lockEvents);

    for (std::vector<SMART_RENAME_MGR_EVENT>::iterator it = m_SmartRenameManagerEvents.begin(); it != m_SmartRenameManagerEvents.end(); ++it)
    {
        if (it->cookie == cookie)
        {
            hr = S_OK;
            it->cookie = 0;
            if (it->pEvents)
            {
                it->pEvents->Release();
                it->pEvents = nullptr;
            }
            break;
        }
    }

    return hr;
}

IFACEMETHODIMP CSmartRenameManager::Start()
{
    return E_NOTIMPL;
}

IFACEMETHODIMP CSmartRenameManager::Stop()
{
    return E_NOTIMPL;
}

IFACEMETHODIMP CSmartRenameManager::Rename(_In_ HWND hwndParent)
{
    m_hwndParent = hwndParent;
    return _PerformFileOperation();
}

IFACEMETHODIMP CSmartRenameManager::Reset()
{
    // Stop all threads and wait
    // Reset all rename items
    return E_NOTIMPL;
}

IFACEMETHODIMP CSmartRenameManager::Shutdown()
{
    _ClearRegEx();
    _Cleanup();
    return S_OK;
}

IFACEMETHODIMP CSmartRenameManager::AddItem(_In_ ISmartRenameItem* pItem)
{
    // Scope lock
    {
        CSRWExclusiveAutoLock lock(&m_lockItems);
        m_smartRenameItems.push_back(pItem);
        pItem->AddRef();
    }

    _OnItemAdded(pItem);

    return S_OK;
}

IFACEMETHODIMP CSmartRenameManager::GetItemByIndex(_In_ UINT index, _COM_Outptr_ ISmartRenameItem** ppItem)
{
    *ppItem = nullptr;
    CSRWSharedAutoLock lock(&m_lockItems);
    HRESULT hr = E_FAIL;
    if (index < m_smartRenameItems.size())
    {
        *ppItem = m_smartRenameItems.at(index);
        (*ppItem)->AddRef();
        hr = S_OK;
    }

    return hr;
}

IFACEMETHODIMP CSmartRenameManager::GetItemById(_In_ int id, _COM_Outptr_ ISmartRenameItem** ppItem)
{
    *ppItem = nullptr;

    CSRWSharedAutoLock lock(&m_lockItems);
    HRESULT hr = E_FAIL;
    auto iterator = std::find_if(m_smartRenameItems.begin(), m_smartRenameItems.end(), [id](_In_ ISmartRenameItem* currentItem)
        {
            int idCurrent;
            currentItem->get_id(&idCurrent);
            return (idCurrent == id);
        });

    if (iterator != m_smartRenameItems.end())
    {
        *ppItem = (*iterator);
        (*ppItem)->AddRef();
        hr = S_OK;
    }

    return hr;
}

IFACEMETHODIMP CSmartRenameManager::GetItemCount(_Out_ UINT* count)
{
    CSRWSharedAutoLock lock(&m_lockItems);
    *count = static_cast<UINT>(m_smartRenameItems.size());
    return S_OK;
}

IFACEMETHODIMP CSmartRenameManager::GetSelectedItemCount(_Out_ UINT* count)
{
    *count = 0;
    CSRWSharedAutoLock lock(&m_lockItems);
    auto isSelected = [count](_In_ ISmartRenameItem* currentItem)
    {
        bool selected = false;
        if (SUCCEEDED(currentItem->get_selected(&selected)) && selected)
        {
            (*count)++;
        }
    };
    std::for_each(m_smartRenameItems.begin(), m_smartRenameItems.end(), isSelected);
    return S_OK;
}

IFACEMETHODIMP CSmartRenameManager::GetRenameItemCount(_Out_ UINT* count)
{
    *count = 0;
    CSRWSharedAutoLock lock(&m_lockItems);
    DWORD flags = m_flags;
    auto willRename = [count, flags](_In_ ISmartRenameItem* currentItem)
    {
        bool shouldRename = false;
        if (SUCCEEDED(currentItem->ShouldRenameItem(flags, &shouldRename)) && shouldRename)
        {
            (*count)++;
        }
    };
    std::for_each(m_smartRenameItems.begin(), m_smartRenameItems.end(), willRename);
    return S_OK;
}

IFACEMETHODIMP CSmartRenameManager::get_flags(_Out_ DWORD* flags)
{
    *flags = m_flags;
    return S_OK;
}

IFACEMETHODIMP CSmartRenameManager::put_flags(_In_ DWORD flags)
{
    if (flags != m_flags)
    {
        m_flags = flags;
        _EnsureRegEx();
        m_spRegEx->put_flags(flags);
    }
    return S_OK;
}

IFACEMETHODIMP CSmartRenameManager::get_smartRenameRegEx(_COM_Outptr_ ISmartRenameRegEx** ppRegEx)
{
    *ppRegEx = nullptr;
    HRESULT hr = _EnsureRegEx();
    if (SUCCEEDED(hr))
    {
        *ppRegEx = m_spRegEx;
        (*ppRegEx)->AddRef();
    }
    return hr;
}

IFACEMETHODIMP CSmartRenameManager::put_smartRenameRegEx(_In_ ISmartRenameRegEx* pRegEx)
{
    _ClearRegEx();
    m_spRegEx = pRegEx;
    return S_OK;
}

IFACEMETHODIMP CSmartRenameManager::get_smartRenameItemFactory(_COM_Outptr_ ISmartRenameItemFactory** ppItemFactory)
{
    *ppItemFactory = nullptr;
    HRESULT hr = E_FAIL;
    if (m_spItemFactory)
    {
        hr = S_OK;
        *ppItemFactory = m_spItemFactory;
        (*ppItemFactory)->AddRef();
    }
    return hr;
}

IFACEMETHODIMP CSmartRenameManager::put_smartRenameItemFactory(_In_ ISmartRenameItemFactory* pItemFactory)
{
    m_spItemFactory = pItemFactory;
    return S_OK;
}

IFACEMETHODIMP CSmartRenameManager::OnSearchTermChanged(_In_ PCWSTR /*searchTerm*/)
{
    _PerformRegExRename();
    return S_OK;
}

IFACEMETHODIMP CSmartRenameManager::OnReplaceTermChanged(_In_ PCWSTR /*replaceTerm*/)
{
    _PerformRegExRename();
    return S_OK;
}

IFACEMETHODIMP CSmartRenameManager::OnFlagsChanged(_In_ DWORD flags)
{
    // Flags were updated in the smart rename regex.  Update our preview.
    m_flags = flags;
    _PerformRegExRename();
    return S_OK;
}

HRESULT CSmartRenameManager::s_CreateInstance(_Outptr_ ISmartRenameManager** ppsrm)
{
    *ppsrm = nullptr;
    CSmartRenameManager *psrm = new CSmartRenameManager();
    HRESULT hr = psrm ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
    {
        hr = psrm->_Init();
        if (SUCCEEDED(hr))
        {
            hr = psrm->QueryInterface(IID_PPV_ARGS(ppsrm));
        }
        psrm->Release();
    }
    return hr;
}

CSmartRenameManager::CSmartRenameManager() :
    m_refCount(1)
{
    InitializeCriticalSection(&m_critsecReentrancy);
}

CSmartRenameManager::~CSmartRenameManager()
{
    DeleteCriticalSection(&m_critsecReentrancy);
}

HRESULT CSmartRenameManager::_Init()
{
    // Guaranteed to succeed
    m_startFileOpWorkerEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    m_startRegExWorkerEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    m_cancelRegExWorkerEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    m_hwndMessage = CreateMsgWindow(s_msgWndProc, this);

    return S_OK;
}

// Custom messages for worker threads
enum
{
    SRM_REGEX_ITEM_UPDATED = (WM_APP + 1),  // Single smart rename item processed by regex worker thread
    SRM_REGEX_CANCELED,                     // Regex operation was canceled
    SRM_REGEX_COMPLETE,                     // Regex worker thread completed
    SRM_FILEOP_COMPLETE                     // File Operation worker thread completed
};

struct WorkerThreadData
{
    HWND hwndManager = nullptr;
    HANDLE startEvent = nullptr;
    HANDLE cancelEvent = nullptr;
    HWND hwndParent = nullptr;
    CComPtr<ISmartRenameManager> spsrm;
};

// Msg-only worker window proc for communication from our worker threads
LRESULT CALLBACK CSmartRenameManager::s_msgWndProc(_In_ HWND hwnd, _In_ UINT uMsg, _In_ WPARAM wParam, _In_ LPARAM lParam)
{
    LRESULT lRes = 0;

    CSmartRenameManager* pThis = (CSmartRenameManager*)GetWindowLongPtr(hwnd, 0);
    if (pThis != nullptr)
    {
        lRes = pThis->_WndProc(hwnd, uMsg, wParam, lParam);
        if (uMsg == WM_NCDESTROY)
        {
            SetWindowLongPtr(hwnd, 0, NULL);
            pThis->m_hwndMessage = nullptr;
        }
    }
    else
    {
        lRes = DefWindowProc(hwnd, uMsg, wParam, lParam);
    }

    return lRes;
}

LRESULT CSmartRenameManager::_WndProc(_In_ HWND hwnd, _In_ UINT msg, _In_ WPARAM wParam, _In_ LPARAM lParam)
{
    LRESULT lRes = 0;

    AddRef();

    switch (msg)
    {
    case SRM_REGEX_ITEM_UPDATED:
    {
        int id = static_cast<int>(lParam);
        CComPtr<ISmartRenameItem> spItem;
        if (SUCCEEDED(GetItemById(id, &spItem)))
        {
            _OnUpdate(spItem);
        }
        break;
    }

    case SRM_REGEX_CANCELED:
        _OnRegExCanceled();
        break;

    case SRM_REGEX_COMPLETE:
        _OnRegExCompleted();
        break;

    default:
        lRes = DefWindowProc(hwnd, msg, wParam, lParam);
        break;
    }

    Release();

    return lRes;
}

HRESULT CSmartRenameManager::_PerformFileOperation()
{
    // Do we have items to rename?
    UINT renameItemCount = 0;
    if (FAILED(GetRenameItemCount(&renameItemCount)) || renameItemCount == 0)
    {
        return E_FAIL;
    }

    // Wait for any regex thread to finish
    _WaitForRegExWorkerThread();

    // Create worker thread which will perform the actual rename
    HRESULT hr = _CreateFileOpWorkerThread();
    if (SUCCEEDED(hr))
    {
        _OnRenameStarted();

        // Signal the worker thread that they can start working. We needed to wait until we
        // were ready to process thread messages.
        SetEvent(m_startFileOpWorkerEvent);

        while (true)
        {
            // Check if worker thread has exited
            if (WaitForSingleObject(m_fileOpWorkerThreadHandle, 0) == WAIT_OBJECT_0)
            {
                break;
            }

            MSG msg;
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == SRM_FILEOP_COMPLETE)
                {
                    // Worker thread completed
                    break;
                }
                else
                {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
            }
        }

        _OnRenameCompleted();
    }

    return 0;
}

HRESULT CSmartRenameManager::_CreateFileOpWorkerThread()
{
    WorkerThreadData* pwtd = new WorkerThreadData;
    HRESULT hr = pwtd ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
    {
        pwtd->hwndManager = m_hwndMessage;
        pwtd->startEvent = m_startRegExWorkerEvent;
        pwtd->cancelEvent = nullptr;
        pwtd->spsrm = this;
        m_fileOpWorkerThreadHandle = CreateThread(nullptr, 0, s_fileOpWorkerThread, pwtd, 0, nullptr);
        hr = (m_fileOpWorkerThreadHandle) ? S_OK : E_FAIL;
        if (FAILED(hr))
        {
            delete pwtd;
        }
    }

    return hr;
}

DWORD WINAPI CSmartRenameManager::s_fileOpWorkerThread(_In_ void* pv)
{
    if (SUCCEEDED(CoInitializeEx(NULL, 0)))
    {
        WorkerThreadData* pwtd = reinterpret_cast<WorkerThreadData*>(pv);
        if (pwtd)
        {
            // Wait to be told we can begin
            if (WaitForSingleObject(pwtd->startEvent, INFINITE) == WAIT_OBJECT_0)
            {
                CComPtr<ISmartRenameRegEx> spRenameRegEx;
                if (SUCCEEDED(pwtd->spsrm->get_smartRenameRegEx(&spRenameRegEx)))
                {
                    // Create IFileOperation interface
                    CComPtr<IFileOperation> spFileOp;
                    if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&spFileOp))))
                    {
                        DWORD flags = 0;
                        spRenameRegEx->get_flags(&flags);

                        UINT itemCount = 0;
                        pwtd->spsrm->GetItemCount(&itemCount);
                        // Add each rename operation
                        for (UINT u = 0; u <= itemCount; u++)
                        {
                            CComPtr<ISmartRenameItem> spItem;
                            if (SUCCEEDED(pwtd->spsrm->GetItemByIndex(u, &spItem)))
                            {
                                bool shouldRename = false;
                                if (SUCCEEDED(spItem->ShouldRenameItem(flags, &shouldRename)) && shouldRename)
                                {
                                    PWSTR newName = nullptr;
                                    if (SUCCEEDED(spItem->get_newName(&newName)))
                                    {
                                        CComPtr<IShellItem> spShellItem;
                                        if (SUCCEEDED(spItem->get_shellItem(&spShellItem)))
                                        {
                                            spFileOp->RenameItem(spShellItem, newName, nullptr);
                                        }
                                        CoTaskMemFree(newName);
                                    }
                                }
                            }
                        }

                        // Set the operation flags
                        if (SUCCEEDED(spFileOp->SetOperationFlags(FOF_DEFAULTFLAGS)))
                        {
                            // TODO: Update with hwnd of UI
                            // Set the parent window
                            if (pwtd->hwndParent)
                            {
                                spFileOp->SetOwnerWindow(pwtd->hwndParent);
                            }
                            
                            // Perform the operation
                            // We don't care about the return code here. We would rather
                            // return control back to explorer so the user can cleanly
                            // undo the operation if it failed halfway through.
                            spFileOp->PerformOperations();
                        }
                    }
                }
            }

            // Send the manager thread the completion message
            PostMessage(pwtd->hwndManager, SRM_FILEOP_COMPLETE, GetCurrentThreadId(), 0);

            delete pwtd;
        }
        CoUninitialize();
    }

    return 0;
}

HRESULT CSmartRenameManager::_PerformRegExRename()
{
    HRESULT hr = E_FAIL;

    if (!TryEnterCriticalSection(&m_critsecReentrancy))
    {
        // Ensure we do not renter since we pump messages here.
        // If we do, post a message back to ourselves
    }
    else
    {
        // Ensure previous thread is canceled
        _CancelRegExWorkerThread();

        // Create worker thread which will message us progress and completion.
        hr = _CreateRegExWorkerThread();
        if (SUCCEEDED(hr))
        {
            ResetEvent(m_cancelRegExWorkerEvent);

            _OnRegExStarted();

            // Signal the worker thread that they can start working. We needed to wait until we
            // were ready to process thread messages.
            SetEvent(m_startRegExWorkerEvent);
        }
    }

    return hr;
}

HRESULT CSmartRenameManager::_CreateRegExWorkerThread()
{
    WorkerThreadData* pwtd = new WorkerThreadData;
    HRESULT hr = pwtd ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
    {
        pwtd->hwndManager = m_hwndMessage;
        pwtd->startEvent = m_startRegExWorkerEvent;
        pwtd->cancelEvent = m_cancelRegExWorkerEvent;
        pwtd->hwndParent = m_hwndParent;
        pwtd->spsrm = this;
        m_regExWorkerThreadHandle = CreateThread(nullptr, 0, s_regexWorkerThread, pwtd, 0, nullptr);
        hr = (m_regExWorkerThreadHandle) ? S_OK : E_FAIL;
        if (FAILED(hr))
        {
            delete pwtd;
        }
    }

    return hr;
}

DWORD WINAPI CSmartRenameManager::s_regexWorkerThread(_In_ void* pv)
{
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
    {
        WorkerThreadData* pwtd = reinterpret_cast<WorkerThreadData*>(pv);
        if (pwtd)
        {
            // Wait to be told we can begin
            if (WaitForSingleObject(pwtd->startEvent, INFINITE) == WAIT_OBJECT_0)
            {
                CComPtr<ISmartRenameRegEx> spRenameRegEx;
                if (SUCCEEDED(pwtd->spsrm->get_smartRenameRegEx(&spRenameRegEx)))
                {
                    DWORD flags = 0;
                    spRenameRegEx->get_flags(&flags);

                    UINT itemCount = 0;
                    pwtd->spsrm->GetItemCount(&itemCount);
                    for (UINT u = 0; u <= itemCount; u++)
                    {
                        // Check if cancel event is signaled
                        if (WaitForSingleObject(pwtd->cancelEvent, 0) == WAIT_OBJECT_0)
                        {
                            // Canceled from manager
                            // Send the manager thread the canceled message
                            PostMessage(pwtd->hwndManager, SRM_REGEX_CANCELED, GetCurrentThreadId(), 0);
                            break;
                        }

                        CComPtr<ISmartRenameItem> spItem;
                        if (SUCCEEDED(pwtd->spsrm->GetItemByIndex(u, &spItem)))
                        {
                            PWSTR originalName = nullptr;
                            if (SUCCEEDED(spItem->get_originalName(&originalName)))
                            {
                                PWSTR currentNewName = nullptr;
                                spItem->get_newName(&currentNewName);

                                wchar_t sourceName[MAX_PATH] = { 0 };
                                if (flags & NameOnly)
                                {
                                    StringCchCopy(sourceName, ARRAYSIZE(sourceName), fs::path(originalName).stem().c_str());
                                }
                                else if (flags & ExtensionOnly)
                                {
                                    std::wstring extension = fs::path(originalName).extension().wstring();
                                    if (!extension.empty() && extension.front() == '.')
                                    {
                                        extension = extension.erase(0, 1);
                                    }
                                    StringCchCopy(sourceName, ARRAYSIZE(sourceName), extension.c_str());
                                }
                                else
                                {
                                    StringCchCopy(sourceName, ARRAYSIZE(sourceName), originalName);
                                }


                                PWSTR newName = nullptr;
                                // Failure here means we didn't match anything or had nothing to match
                                // Call put_newName with null in that case to reset it
                                spRenameRegEx->Replace(sourceName, &newName);

                                wchar_t resultName[MAX_PATH] = { 0 };


                                PWSTR newNameToUse = nullptr;

                                // newName == nullptr likely means we have an empty search string.  We should leave newNameToUse
                                // as nullptr so we clear the renamed column
                                if (newName != nullptr)
                                {
                                    newNameToUse = resultName;
                                    if (flags & NameOnly)
                                    {
                                        StringCchPrintf(resultName, ARRAYSIZE(resultName), L"%s%s", newName, fs::path(originalName).extension().c_str());
                                    }
                                    else if (flags & ExtensionOnly)
                                    {
                                        std::wstring extension = fs::path(originalName).extension().wstring();
                                        if (!extension.empty())
                                        {
                                            StringCchPrintf(resultName, ARRAYSIZE(resultName), L"%s.%s", fs::path(originalName).stem().c_str(), newName);
                                        }
                                        else
                                        {
                                            StringCchCopy(resultName, ARRAYSIZE(resultName), originalName);
                                        }
                                    }
                                    else
                                    {
                                        StringCchCopy(resultName, ARRAYSIZE(resultName), newName);
                                    }
                                }
                                
                                // No change from originalName so set newName to
                                // null so we clear it from our UI as well.
                                if (lstrcmp(originalName, newNameToUse) == 0)
                                {
                                    newNameToUse = nullptr;
                                }

                                // Only update newName if there was in fact a change from the original name
                                //if (newName == nullptr || lstrcmp(originalName, newName) != 0)
                                {
                                    wchar_t uniqueName[MAX_PATH] = { 0 };
                                    if (newNameToUse != nullptr && (flags & EnumerateItems))
                                    {
                                        PWSTR parentPath = nullptr;
                                        spItem->get_parentPath(&parentPath);

                                        // Make a unique name with a counter
                                        if (PathMakeUniqueName(uniqueName, ARRAYSIZE(uniqueName), newNameToUse, newNameToUse, parentPath))
                                        {
                                            // Trim to the last element in the path
                                            newNameToUse = PathFindFileNameW(uniqueName);
                                            if (newNameToUse == nullptr)
                                            {
                                                uniqueName[0] = L'\0';
                                                newNameToUse = uniqueName;
                                            }
                                        }

                                        CoTaskMemFree(parentPath);
                                    }

                                    spItem->put_newName(newNameToUse);

                                    // Was there a change?
                                    if (lstrcmp(currentNewName, newNameToUse) != 0)
                                    {
                                        int id = -1;
                                        if (SUCCEEDED(spItem->get_id(&id)))
                                        {
                                            // Send the manager thread the item processed message
                                            PostMessage(pwtd->hwndManager, SRM_REGEX_ITEM_UPDATED, GetCurrentThreadId(), id);
                                        }
                                    }
                                }

                                CoTaskMemFree(newName);
                                CoTaskMemFree(currentNewName);
                                CoTaskMemFree(originalName);
                            }
                        }
                    }
                }
            }

            // Send the manager thread the completion message
            PostMessage(pwtd->hwndManager, SRM_REGEX_COMPLETE, GetCurrentThreadId(), 0);

            delete pwtd;
        }
        CoUninitialize();
    }

    return 0;
}

void CSmartRenameManager::_CancelRegExWorkerThread()
{
    if (m_startRegExWorkerEvent)
    {
        SetEvent(m_startRegExWorkerEvent);
    }

    if (m_cancelRegExWorkerEvent)
    {
        SetEvent(m_cancelRegExWorkerEvent);
    }

    _WaitForRegExWorkerThread();
}

void CSmartRenameManager::_WaitForRegExWorkerThread()
{
    if (m_regExWorkerThreadHandle)
    {
        WaitForSingleObject(m_regExWorkerThreadHandle, INFINITE);
        CloseHandle(m_regExWorkerThreadHandle);
        m_regExWorkerThreadHandle = nullptr;
    }
}

void CSmartRenameManager::_Cancel()
{
    SetEvent(m_startFileOpWorkerEvent);
    _CancelRegExWorkerThread();
}

HRESULT CSmartRenameManager::_EnsureRegEx()
{
    HRESULT hr = S_OK;
    if (!m_spRegEx)
    {
        // Create the default regex handler
        hr = CSmartRenameRegEx::s_CreateInstance(&m_spRegEx);
        if (SUCCEEDED(hr))
        {
            hr = _InitRegEx();
        }
    }
    return hr;
}

HRESULT CSmartRenameManager::_InitRegEx()
{
    HRESULT hr = E_FAIL;
    if (m_spRegEx)
    {
        hr = m_spRegEx->Advise(this, &m_regExAdviseCookie);
    }

    return hr;
}

void CSmartRenameManager::_ClearRegEx()
{
    if (m_spRegEx)
    {
        m_spRegEx->UnAdvise(m_regExAdviseCookie);
        m_regExAdviseCookie = 0;
    }
}

void CSmartRenameManager::_OnItemAdded(_In_ ISmartRenameItem* renameItem)
{
    CSRWSharedAutoLock lock(&m_lockEvents);

    for (std::vector<SMART_RENAME_MGR_EVENT>::iterator it = m_SmartRenameManagerEvents.begin(); it != m_SmartRenameManagerEvents.end(); ++it)
    {
        if (it->pEvents)
        {
            it->pEvents->OnItemAdded(renameItem);
        }
    }
}

void CSmartRenameManager::_OnUpdate(_In_ ISmartRenameItem* renameItem)
{
    CSRWSharedAutoLock lock(&m_lockEvents);

    for (std::vector<SMART_RENAME_MGR_EVENT>::iterator it = m_SmartRenameManagerEvents.begin(); it != m_SmartRenameManagerEvents.end(); ++it)
    {
        if (it->pEvents)
        {
            it->pEvents->OnUpdate(renameItem);
        }
    }
}

void CSmartRenameManager::_OnError(_In_ ISmartRenameItem* renameItem)
{
    CSRWSharedAutoLock lock(&m_lockEvents);

    for (std::vector<SMART_RENAME_MGR_EVENT>::iterator it = m_SmartRenameManagerEvents.begin(); it != m_SmartRenameManagerEvents.end(); ++it)
    {
        if (it->pEvents)
        {
            it->pEvents->OnError(renameItem);
        }
    }
}

void CSmartRenameManager::_OnRegExStarted()
{
    CSRWSharedAutoLock lock(&m_lockEvents);

    for (std::vector<SMART_RENAME_MGR_EVENT>::iterator it = m_SmartRenameManagerEvents.begin(); it != m_SmartRenameManagerEvents.end(); ++it)
    {
        if (it->pEvents)
        {
            it->pEvents->OnRegExStarted();
        }
    }
}

void CSmartRenameManager::_OnRegExCanceled()
{
    CSRWSharedAutoLock lock(&m_lockEvents);

    for (std::vector<SMART_RENAME_MGR_EVENT>::iterator it = m_SmartRenameManagerEvents.begin(); it != m_SmartRenameManagerEvents.end(); ++it)
    {
        if (it->pEvents)
        {
            it->pEvents->OnRegExCanceled();
        }
    }
}

void CSmartRenameManager::_OnRegExCompleted()
{
    CSRWSharedAutoLock lock(&m_lockEvents);

    for (std::vector<SMART_RENAME_MGR_EVENT>::iterator it = m_SmartRenameManagerEvents.begin(); it != m_SmartRenameManagerEvents.end(); ++it)
    {
        if (it->pEvents)
        {
            it->pEvents->OnRegExCompleted();
        }
    }
}

void CSmartRenameManager::_OnRenameStarted()
{
    CSRWSharedAutoLock lock(&m_lockEvents);

    for (std::vector<SMART_RENAME_MGR_EVENT>::iterator it = m_SmartRenameManagerEvents.begin(); it != m_SmartRenameManagerEvents.end(); ++it)
    {
        if (it->pEvents)
        {
            it->pEvents->OnRenameStarted();
        }
    }
}

void CSmartRenameManager::_OnRenameCompleted()
{
    CSRWSharedAutoLock lock(&m_lockEvents);

    for (std::vector<SMART_RENAME_MGR_EVENT>::iterator it = m_SmartRenameManagerEvents.begin(); it != m_SmartRenameManagerEvents.end(); ++it)
    {
        if (it->pEvents)
        {
            it->pEvents->OnRenameCompleted();
        }
    }
}

void CSmartRenameManager::_ClearEventHandlers()
{
    CSRWExclusiveAutoLock lock(&m_lockEvents);

    // Cleanup event handlers
    for (std::vector<SMART_RENAME_MGR_EVENT>::iterator it = m_SmartRenameManagerEvents.begin(); it != m_SmartRenameManagerEvents.end(); ++it)
    {
        it->cookie = 0;
        if (it->pEvents)
        {
            it->pEvents->Release();
            it->pEvents = nullptr;
        }
    }

    m_SmartRenameManagerEvents.clear();
}

void CSmartRenameManager::_ClearSmartRenameItems()
{
    CSRWExclusiveAutoLock lock(&m_lockItems);

    // Cleanup smart rename items
    for (std::vector<ISmartRenameItem*>::iterator it = m_smartRenameItems.begin(); it != m_smartRenameItems.end(); ++it)
    {
        ISmartRenameItem* pItem = *it;
        pItem->Release();
    }

    m_smartRenameItems.clear();
}

void CSmartRenameManager::_Cleanup()
{
    if (m_hwndMessage)
    {
        DestroyWindow(m_hwndMessage);
        m_hwndMessage = nullptr;
    }

    CloseHandle(m_startFileOpWorkerEvent);
    m_startFileOpWorkerEvent = nullptr;

    CloseHandle(m_startRegExWorkerEvent);
    m_startRegExWorkerEvent = nullptr;

    CloseHandle(m_cancelRegExWorkerEvent);
    m_cancelRegExWorkerEvent = nullptr;

    _ClearRegEx();
    _ClearEventHandlers();
    _ClearSmartRenameItems();
}
