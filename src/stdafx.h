#pragma once

/*

	insert next lines if you have linking issues:

	#pragma comment(lib, "d3d12.lib")
	#pragma comment(lib, "dxgi.lib")
	#pragma comment(lib, "d3dcompiler.lib")
	#pragma comment(lib, "dxguid.lib") // fixes the unresolved external symbol _IID_ID3D12Device error
								      // or add #include <initguid.h> before including d3d12.h
*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers.
#endif

#include <windows.h>

#include <dxgi1_6.h>
#include <d3d12.h>
#include <D3Dcompiler.h>
#include <DirectXMath.h>

#include "d3dx12.h"

#include <wrl.h>
#include <vector>
