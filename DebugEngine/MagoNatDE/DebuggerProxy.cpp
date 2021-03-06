/*
   Copyright (c) 2010 Aldo J. Nunez

   Licensed under the Apache License, Version 2.0.
   See the LICENSE text file for details.
*/

#include "Common.h"
#include "DebuggerProxy.h"
#include "CommandFunctor.h"


// these values can be tweaked, as long as we're responsive and don't spin
const DWORD EventTimeoutMillis = 50;
const DWORD CommandTimeoutMillis = 0;


namespace Mago
{
    DebuggerProxy::DebuggerProxy()
        :   mhThread( NULL ),
            mWorkerTid( 0 ),
            mMachine( NULL ),
            mCallback( NULL ),
            mhReadyEvent( NULL ),
            mhCommandEvent( NULL ),
            mhResultEvent( NULL ),
            mCurCommand( NULL ),
            mShutdown( false )
    {
    }

    DebuggerProxy::~DebuggerProxy()
    {
        // TODO: use smart pointers

        Shutdown();

        if ( mhThread != NULL )
            CloseHandle( mhThread );

        if ( mhReadyEvent != NULL )
            CloseHandle( mhReadyEvent );

        if ( mhCommandEvent != NULL )
            CloseHandle( mhCommandEvent );

        if ( mhResultEvent != NULL )
            CloseHandle( mhResultEvent );
    }

    HRESULT DebuggerProxy::Init( IMachine* machine, IEventCallback* callback )
    {
        _ASSERT( machine != NULL );
        _ASSERT( callback != NULL );
        if ( (machine == NULL) || (callback == NULL ) )
            return E_INVALIDARG;
        if ( (mMachine != NULL) || (mCallback != NULL ) )
            return E_ALREADY_INIT;

        HandlePtr   hReadyEvent;
        HandlePtr   hCommandEvent;
        HandlePtr   hResultEvent;

        hReadyEvent = CreateEvent( NULL, TRUE, FALSE, NULL );
        if ( hReadyEvent.IsEmpty() )
            return GetLastHr();

        hCommandEvent = CreateEvent( NULL, TRUE, FALSE, NULL );
        if ( hCommandEvent.IsEmpty() )
            return GetLastHr();

        hResultEvent = CreateEvent( NULL, TRUE, FALSE, NULL );
        if ( hResultEvent.IsEmpty() )
            return GetLastHr();

        mhReadyEvent = hReadyEvent.Detach();
        mhCommandEvent = hCommandEvent.Detach();
        mhResultEvent = hResultEvent.Detach();

        mMachine = machine;
        mMachine->AddRef();

        mCallback = callback;
        mCallback->AddRef();

        return S_OK;
    }

    HRESULT DebuggerProxy::Start()
    {
        _ASSERT( mMachine != NULL );
        _ASSERT( mCallback != NULL );
        if ( (mMachine == NULL) || (mCallback == NULL ) )
            return E_UNEXPECTED;
        if ( mhThread != NULL )
            return E_UNEXPECTED;

        HandlePtr   hThread;

        hThread = CreateThread(
            NULL,
            0,
            DebugPollProc,
            this,
            0,
            NULL );
        if ( hThread.IsEmpty() )
            return GetLastHr();

        HANDLE      waitObjs[2] = { mhReadyEvent, hThread.Get() };
        DWORD       waitRet = 0;

        // TODO: on error, thread will be shutdown from Shutdown method

        waitRet = WaitForMultipleObjects( _countof( waitObjs ), waitObjs, FALSE, INFINITE );
        if ( waitRet == WAIT_FAILED )
            return GetLastHr();

        if ( waitRet == WAIT_OBJECT_0 + 1 )
        {
            DWORD   exitCode = (DWORD) E_FAIL;

            // the thread ended because of an error, let's get the return exit code
            GetExitCodeThread( hThread.Get(), &exitCode );

            return (HRESULT) exitCode;
        }
        _ASSERT( waitRet == WAIT_OBJECT_0 );

        mhThread = hThread.Detach();

        return S_OK;
    }

    void DebuggerProxy::Shutdown()
    {
        // TODO: is this enough?

        mShutdown = true;

        if ( mhThread != NULL )
        {
            // there are no infinite waits on the poll thread, so it'll detect shutdown signal
            WaitForSingleObject( mhThread, INFINITE );
        }

        if ( mMachine != NULL )
        {
            mMachine->Release();
            mMachine = NULL;
        }

        if ( mCallback != NULL )
        {
            mCallback->Release();
            mCallback = NULL;
        }

        mExec.Shutdown();
    }


    HRESULT DebuggerProxy::InvokeCommand( CommandFunctor& cmd )
    {
        if ( mWorkerTid == GetCurrentThreadId() )
        {
            // since we're on the poll thread, we can run the command directly
            cmd.Run();
        }
        else
        {
            GuardedArea area( mCommandGuard );

            mCurCommand = &cmd;

            ResetEvent( mhResultEvent );
            SetEvent( mhCommandEvent );

            DWORD   waitRet = 0;
            HANDLE  handles[2] = { mhResultEvent, mhThread };

            waitRet = WaitForMultipleObjects( _countof( handles ), handles, FALSE, INFINITE );
            mCurCommand = NULL;
            if ( waitRet == WAIT_FAILED )
                return GetLastHr();

            if ( waitRet == WAIT_OBJECT_0 + 1 )
                return CO_E_REMOTE_COMMUNICATION_FAILURE;
        }

        return S_OK;
    }

//----------------------------------------------------------------------------
// Commands
//----------------------------------------------------------------------------

    HRESULT DebuggerProxy::Launch( LaunchInfo* launchInfo, IProcess*& process )
    {
        HRESULT         hr = S_OK;
        LaunchParams    params( mExec );

        params.Settings = launchInfo;

        hr = InvokeCommand( params );
        if ( FAILED( hr ) )
            return hr;

        if ( SUCCEEDED( params.OutHResult ) )
            process = params.OutProcess.Detach();

        return params.OutHResult;
    }

    HRESULT DebuggerProxy::Attach( uint32_t id, IProcess*& process )
    {
        HRESULT         hr = S_OK;
        AttachParams    params( mExec );

        params.ProcessId = id;

        hr = InvokeCommand( params );
        if ( FAILED( hr ) )
            return hr;

        if ( SUCCEEDED( params.OutHResult ) )
            process = params.OutProcess.Detach();

        return params.OutHResult;
    }

    HRESULT DebuggerProxy::Terminate( IProcess* process )
    {
        HRESULT         hr = S_OK;
        TerminateParams params( mExec );

        params.Process = process;

        hr = InvokeCommand( params );
        if ( FAILED( hr ) )
            return hr;

        return params.OutHResult;
    }

    HRESULT DebuggerProxy::Detach( IProcess* process )
    {
        HRESULT         hr = S_OK;
        DetachParams    params( mExec );

        params.Process = process;

        hr = InvokeCommand( params );
        if ( FAILED( hr ) )
            return hr;

        return params.OutHResult;
    }

    HRESULT DebuggerProxy::ResumeProcess( IProcess* process )
    {
        HRESULT             hr = S_OK;
        ResumeProcessParams params( mExec );

        params.Process = process;

        hr = InvokeCommand( params );
        if ( FAILED( hr ) )
            return hr;

        return params.OutHResult;
    }

    HRESULT DebuggerProxy::TerminateNewProcess( IProcess* process )
    {
        HRESULT                     hr = S_OK;
        TerminateNewProcessParams   params( mExec );

        params.Process = process;

        hr = InvokeCommand( params );
        if ( FAILED( hr ) )
            return hr;

        return params.OutHResult;
    }

    HRESULT DebuggerProxy::ReadMemory( 
        IProcess* process, 
        Address address,
        SIZE_T length, 
        SIZE_T& lengthRead, 
        SIZE_T& lengthUnreadable, 
        uint8_t* buffer )
    {
        // call it directly for performance (it gets called a lot for stack walking)
        // we're allowed to, since this function is now free threaded
        return mExec.ReadMemory( process, address, length, lengthRead, lengthUnreadable, buffer );
    }

    HRESULT DebuggerProxy::WriteMemory( 
        IProcess* process, 
        Address address,
        SIZE_T length, 
        SIZE_T& lengthWritten, 
        uint8_t* buffer )
    {
        HRESULT             hr = S_OK;
        WriteMemoryParams   params( mExec );

        params.Process = process;
        params.Address = address;
        params.Length = length;
        params.Buffer = buffer;

        hr = InvokeCommand( params );
        if ( FAILED( hr ) )
            return hr;

        if ( SUCCEEDED( params.OutHResult ) )
            lengthWritten = params.OutLengthWritten;

        return params.OutHResult;
    }

    HRESULT DebuggerProxy::SetBreakpoint( IProcess* process, Address address, BPCookie cookie )
    {
        HRESULT             hr = S_OK;
        SetBreakpointParams params( mExec );

        params.Process = process;
        params.Address = address;
        params.Cookie = cookie;

        hr = InvokeCommand( params );
        if ( FAILED( hr ) )
            return hr;

        return params.OutHResult;
    }

    HRESULT DebuggerProxy::RemoveBreakpoint( IProcess* process, Address address, BPCookie cookie )
    {
        HRESULT                 hr = S_OK;
        RemoveBreakpointParams  params( mExec );

        params.Process = process;
        params.Address = address;
        params.Cookie = cookie;

        hr = InvokeCommand( params );
        if ( FAILED( hr ) )
            return hr;

        return params.OutHResult;
    }

    HRESULT DebuggerProxy::StepOut( IProcess* process, Address targetAddr, bool handleException )
    {
        HRESULT                 hr = S_OK;
        StepOutParams           params( mExec );

        params.Process = process;
        params.TargetAddress = targetAddr;
        params.HandleException = handleException;

        hr = InvokeCommand( params );
        if ( FAILED( hr ) )
            return hr;

        return params.OutHResult;
    }

    HRESULT DebuggerProxy::StepInstruction( IProcess* process, bool stepIn, bool sourceMode, bool handleException )
    {
        HRESULT                 hr = S_OK;
        StepInstructionParams   params( mExec );

        params.Process = process;
        params.StepIn = stepIn;
        params.SourceMode = sourceMode;
        params.HandleException = handleException;

        hr = InvokeCommand( params );
        if ( FAILED( hr ) )
            return hr;

        return params.OutHResult;
    }

    HRESULT DebuggerProxy::StepRange( IProcess* process, bool stepIn, bool sourceMode, AddressRange* ranges, int rangeCount, bool handleException )
    {
        HRESULT         hr = S_OK;
        StepRangeParams params( mExec );

        params.Process = process;
        params.StepIn = stepIn;
        params.SourceMode = sourceMode;
        params.Ranges = ranges;
        params.RangeCount = rangeCount;
        params.HandleException = handleException;

        hr = InvokeCommand( params );
        if ( FAILED( hr ) )
            return hr;

        return params.OutHResult;
    }

    HRESULT DebuggerProxy::Continue( IProcess* process, bool handleException )
    {
        HRESULT         hr = S_OK;
        ContinueParams  params( mExec );

        params.Process = process;
        params.HandleException = handleException;

        hr = InvokeCommand( params );
        if ( FAILED( hr ) )
            return hr;

        return params.OutHResult;
    }

    HRESULT DebuggerProxy::Execute( IProcess* process, bool handleException )
    {
        HRESULT         hr = S_OK;
        ExecuteParams   params( mExec );

        params.Process = process;
        params.HandleException = handleException;

        hr = InvokeCommand( params );
        if ( FAILED( hr ) )
            return hr;

        return params.OutHResult;
    }

    HRESULT DebuggerProxy::AsyncBreak( IProcess* process )
    {
        HRESULT             hr = S_OK;
        AsyncBreakParams    params( mExec );

        params.Process = process;

        hr = InvokeCommand( params );
        if ( FAILED( hr ) )
            return hr;

        return params.OutHResult;
    }


//----------------------------------------------------------------------------
//  Poll thread
//----------------------------------------------------------------------------

    DWORD    DebuggerProxy::DebugPollProc( void* param )
    {
        _ASSERT( param != NULL );

        DebuggerProxy*    pThis = (DebuggerProxy*) param;

        CoInitializeEx( NULL, COINIT_MULTITHREADED );
        HRESULT hr = pThis->PollLoop();
        CoUninitialize();

        return hr;
    }

    HRESULT DebuggerProxy::PollLoop()
    {
        HRESULT hr = S_OK;

        hr = mExec.Init( mMachine, mCallback );
        if ( FAILED( hr ) )
            return hr;

        SetReadyThread();

        while ( !mShutdown )
        {
            hr = mExec.WaitForDebug( EventTimeoutMillis );
            if ( FAILED( hr ) )
            {
                if ( hr == E_HANDLE )
                {
                    // no debuggee has started yet
                    Sleep( EventTimeoutMillis );
                }
                else if ( hr != E_TIMEOUT )
                    break;
            }
            else
            {
                hr = mExec.DispatchEvent();
                if ( hr == S_OK )
                    // If there was an exception, then we generally want to pass it on.
                    // Exec will handle any special cases, if needed.
                    hr = mExec.ContinueDebug( false );
                if ( FAILED( hr ) )
                    break;
            }

            hr = CheckMessage();
            if ( FAILED( hr ) )
                break;
        }

        OutputDebugStringA( "Poll loop shutting down.\n" );
        return hr;
    }

    void    DebuggerProxy::SetReadyThread()
    {
        _ASSERT( mhReadyEvent != NULL );

        mWorkerTid = GetCurrentThreadId();
        SetEvent( mhReadyEvent );
    }

    HRESULT DebuggerProxy::CheckMessage()
    {
        HRESULT hr = S_OK;
        DWORD   waitRet = 0;

        waitRet = WaitForSingleObject( mhCommandEvent, CommandTimeoutMillis );
        if ( waitRet == WAIT_FAILED )
            return GetLastHr();

        if ( waitRet == WAIT_TIMEOUT )
            return S_OK;

        hr = ProcessCommand( mCurCommand );
        if ( FAILED( hr ) )
            return hr;

        ResetEvent( mhCommandEvent );
        SetEvent( mhResultEvent );

        return S_OK;
    }

    HRESULT DebuggerProxy::ProcessCommand( CommandFunctor* cmd )
    {
        _ASSERT( cmd != NULL );
        if ( cmd == NULL )
            return E_POINTER;

        cmd->Run();

        return S_OK;
    }
}
