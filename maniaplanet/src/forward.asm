; Generated with the export list of Windows 11 d3d11.dll: every export the proxy doesn't hook
; jumps to the system d3d11.dll with the caller's arguments untouched (g_forward, filled in
; DllMain by proxy.cpp in this order).

EXTERN g_forward:QWORD

.code

CreateDirect3D11DeviceFromDXGIDevice PROC
    jmp qword ptr [g_forward + 0]
CreateDirect3D11DeviceFromDXGIDevice ENDP

CreateDirect3D11SurfaceFromDXGISurface PROC
    jmp qword ptr [g_forward + 8]
CreateDirect3D11SurfaceFromDXGISurface ENDP

D3D11CoreCreateDevice PROC
    jmp qword ptr [g_forward + 16]
D3D11CoreCreateDevice ENDP

D3D11CoreCreateLayeredDevice PROC
    jmp qword ptr [g_forward + 24]
D3D11CoreCreateLayeredDevice ENDP

D3D11CoreGetLayeredDeviceSize PROC
    jmp qword ptr [g_forward + 32]
D3D11CoreGetLayeredDeviceSize ENDP

D3D11CoreRegisterLayers PROC
    jmp qword ptr [g_forward + 40]
D3D11CoreRegisterLayers ENDP

D3D11CreateDeviceForD3D12 PROC
    jmp qword ptr [g_forward + 48]
D3D11CreateDeviceForD3D12 ENDP

D3D11On12CreateDevice PROC
    jmp qword ptr [g_forward + 56]
D3D11On12CreateDevice ENDP

D3DKMTCloseAdapter PROC
    jmp qword ptr [g_forward + 64]
D3DKMTCloseAdapter ENDP

D3DKMTCreateAllocation PROC
    jmp qword ptr [g_forward + 72]
D3DKMTCreateAllocation ENDP

D3DKMTCreateContext PROC
    jmp qword ptr [g_forward + 80]
D3DKMTCreateContext ENDP

D3DKMTCreateDevice PROC
    jmp qword ptr [g_forward + 88]
D3DKMTCreateDevice ENDP

D3DKMTCreateSynchronizationObject PROC
    jmp qword ptr [g_forward + 96]
D3DKMTCreateSynchronizationObject ENDP

D3DKMTDestroyAllocation PROC
    jmp qword ptr [g_forward + 104]
D3DKMTDestroyAllocation ENDP

D3DKMTDestroyContext PROC
    jmp qword ptr [g_forward + 112]
D3DKMTDestroyContext ENDP

D3DKMTDestroyDevice PROC
    jmp qword ptr [g_forward + 120]
D3DKMTDestroyDevice ENDP

D3DKMTDestroySynchronizationObject PROC
    jmp qword ptr [g_forward + 128]
D3DKMTDestroySynchronizationObject ENDP

D3DKMTEscape PROC
    jmp qword ptr [g_forward + 136]
D3DKMTEscape ENDP

D3DKMTGetContextSchedulingPriority PROC
    jmp qword ptr [g_forward + 144]
D3DKMTGetContextSchedulingPriority ENDP

D3DKMTGetDeviceState PROC
    jmp qword ptr [g_forward + 152]
D3DKMTGetDeviceState ENDP

D3DKMTGetDisplayModeList PROC
    jmp qword ptr [g_forward + 160]
D3DKMTGetDisplayModeList ENDP

D3DKMTGetMultisampleMethodList PROC
    jmp qword ptr [g_forward + 168]
D3DKMTGetMultisampleMethodList ENDP

D3DKMTGetRuntimeData PROC
    jmp qword ptr [g_forward + 176]
D3DKMTGetRuntimeData ENDP

D3DKMTGetSharedPrimaryHandle PROC
    jmp qword ptr [g_forward + 184]
D3DKMTGetSharedPrimaryHandle ENDP

D3DKMTLock PROC
    jmp qword ptr [g_forward + 192]
D3DKMTLock ENDP

D3DKMTOpenAdapterFromHdc PROC
    jmp qword ptr [g_forward + 200]
D3DKMTOpenAdapterFromHdc ENDP

D3DKMTOpenResource PROC
    jmp qword ptr [g_forward + 208]
D3DKMTOpenResource ENDP

D3DKMTPresent PROC
    jmp qword ptr [g_forward + 216]
D3DKMTPresent ENDP

D3DKMTQueryAdapterInfo PROC
    jmp qword ptr [g_forward + 224]
D3DKMTQueryAdapterInfo ENDP

D3DKMTQueryAllocationResidency PROC
    jmp qword ptr [g_forward + 232]
D3DKMTQueryAllocationResidency ENDP

D3DKMTQueryResourceInfo PROC
    jmp qword ptr [g_forward + 240]
D3DKMTQueryResourceInfo ENDP

D3DKMTRender PROC
    jmp qword ptr [g_forward + 248]
D3DKMTRender ENDP

D3DKMTSetAllocationPriority PROC
    jmp qword ptr [g_forward + 256]
D3DKMTSetAllocationPriority ENDP

D3DKMTSetContextSchedulingPriority PROC
    jmp qword ptr [g_forward + 264]
D3DKMTSetContextSchedulingPriority ENDP

D3DKMTSetDisplayMode PROC
    jmp qword ptr [g_forward + 272]
D3DKMTSetDisplayMode ENDP

D3DKMTSetDisplayPrivateDriverFormat PROC
    jmp qword ptr [g_forward + 280]
D3DKMTSetDisplayPrivateDriverFormat ENDP

D3DKMTSetGammaRamp PROC
    jmp qword ptr [g_forward + 288]
D3DKMTSetGammaRamp ENDP

D3DKMTSetVidPnSourceOwner PROC
    jmp qword ptr [g_forward + 296]
D3DKMTSetVidPnSourceOwner ENDP

D3DKMTSignalSynchronizationObject PROC
    jmp qword ptr [g_forward + 304]
D3DKMTSignalSynchronizationObject ENDP

D3DKMTUnlock PROC
    jmp qword ptr [g_forward + 312]
D3DKMTUnlock ENDP

D3DKMTWaitForSynchronizationObject PROC
    jmp qword ptr [g_forward + 320]
D3DKMTWaitForSynchronizationObject ENDP

D3DKMTWaitForVerticalBlankEvent PROC
    jmp qword ptr [g_forward + 328]
D3DKMTWaitForVerticalBlankEvent ENDP

D3DPerformance_BeginEvent PROC
    jmp qword ptr [g_forward + 336]
D3DPerformance_BeginEvent ENDP

D3DPerformance_EndEvent PROC
    jmp qword ptr [g_forward + 344]
D3DPerformance_EndEvent ENDP

D3DPerformance_GetStatus PROC
    jmp qword ptr [g_forward + 352]
D3DPerformance_GetStatus ENDP

D3DPerformance_SetMarker PROC
    jmp qword ptr [g_forward + 360]
D3DPerformance_SetMarker ENDP

EnableFeatureLevelUpgrade PROC
    jmp qword ptr [g_forward + 368]
EnableFeatureLevelUpgrade ENDP

OpenAdapter10 PROC
    jmp qword ptr [g_forward + 376]
OpenAdapter10 ENDP

OpenAdapter10_2 PROC
    jmp qword ptr [g_forward + 384]
OpenAdapter10_2 ENDP

END
