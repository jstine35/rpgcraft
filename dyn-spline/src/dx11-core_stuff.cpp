
#include "msw-redtape.h"

#include "x-stl.h"
#include "x-string.h"
#include "x-thread.h"

#include "v-float.h"
#include "x-gpu-ifc.h"


#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <directxmath.h>
#include <DirectXColors.h>

#include <wincodec.h>

DECLARE_MODULE_NAME("dx11");

#define ptr_cast		reinterpret_cast

using namespace DirectX;

static const int BackBufferCount = 3;

extern HINSTANCE		g_hInst;
extern HWND				g_hWnd;

ID3D11RasterizerState*	g_RasterState[_GPU_Fill_Count_][_GPU_Cull_Count_][_GPU_Scissor_Count_];

D3D_DRIVER_TYPE         g_driverType			= D3D_DRIVER_TYPE_NULL;
D3D_FEATURE_LEVEL       g_featureLevel			= D3D_FEATURE_LEVEL_11_0;
ID3D11Device*           g_pd3dDevice			= nullptr;
ID3D11Device1*          g_pd3dDevice1			= nullptr;
ID3D11DeviceContext*    g_pImmediateContext		= nullptr;
ID3D11DeviceContext1*   g_pImmediateContext1	= nullptr;
IDXGISwapChain*         g_pSwapChain			= nullptr;
IDXGISwapChain1*        g_pSwapChain1			= nullptr;

ID3D11InputLayout*      g_pVertexLayout			= nullptr;
ID3D11Buffer*           g_pConstantBuffer		= nullptr;

ID3D11InputLayout*      g_pVertexLayouts[VertexBufferLayout_NUM_LAYOUTS];

XMMATRIX                g_World;
XMMATRIX                g_View;
XMMATRIX                g_Projection;

GPU_RenderTarget		g_gpu_BackBuffer;

int g_curBufferIdx = 0;


// * Vertex Buffers are Mostly Dynamic.
// * Use rotating buffers to avoid blocking on prev frame in order to setup new frame.
// * Index Buffers use series of "Default Layouts" which can be packed into a single buffer.

struct DynamicVertexBufferSet {

	// Vertex lists will need to be split according to their layout - namely their stride.
	// eg, if some have texture UV data while others do not.
	// It makes the most sense if the buffers are named after the vertex data types that they contain.
	// Or more generically they can be named according to the buffer stride, since the underlying data
	// is not important.

	// For now just have the Simple buffer...

	ID3D11Buffer*		Buffers[128]	= { nullptr };
};


int							g_DynVertBufferCount = 0;
DynamicVertexBufferSet		g_DynVertBuffers[BackBufferCount];

ID3D11SamplerState*			m_pTextureSampler = nullptr;


DXGI_FORMAT get_DXGI_Format(GPU_ResourceFmt bitmapFmt)
{
	switch(bitmapFmt)
	{
		case     GPU_ResourceFmt_R32G32B32A32_TYPELESS     :  return DXGI_FORMAT_R32G32B32A32_TYPELESS       ;
		case     GPU_ResourceFmt_R32G32B32A32_FLOAT        :  return DXGI_FORMAT_R32G32B32A32_FLOAT          ;
		case     GPU_ResourceFmt_R32G32B32A32_UINT         :  return DXGI_FORMAT_R32G32B32A32_UINT           ;
		case     GPU_ResourceFmt_R32G32B32A32_SINT         :  return DXGI_FORMAT_R32G32B32A32_SINT           ;
		case     GPU_ResourceFmt_R32G32B32_TYPELESS        :  return DXGI_FORMAT_R32G32B32_TYPELESS          ;
		case     GPU_ResourceFmt_R32G32B32_FLOAT           :  return DXGI_FORMAT_R32G32B32_FLOAT             ;
		case     GPU_ResourceFmt_R32G32B32_UINT            :  return DXGI_FORMAT_R32G32B32_UINT              ;
		case     GPU_ResourceFmt_R32G32B32_SINT            :  return DXGI_FORMAT_R32G32B32_SINT              ;
		case     GPU_ResourceFmt_R16G16B16A16_TYPELESS     :  return DXGI_FORMAT_R16G16B16A16_TYPELESS       ;
		case     GPU_ResourceFmt_R16G16B16A16_FLOAT        :  return DXGI_FORMAT_R16G16B16A16_FLOAT          ;
		case     GPU_ResourceFmt_R16G16B16A16_UNORM        :  return DXGI_FORMAT_R16G16B16A16_UNORM          ;
		case     GPU_ResourceFmt_R16G16B16A16_UINT         :  return DXGI_FORMAT_R16G16B16A16_UINT           ;
		case     GPU_ResourceFmt_R16G16B16A16_SNORM        :  return DXGI_FORMAT_R16G16B16A16_SNORM          ;
		case     GPU_ResourceFmt_R16G16B16A16_SINT         :  return DXGI_FORMAT_R16G16B16A16_SINT           ;
		case     GPU_ResourceFmt_R32G32_TYPELESS           :  return DXGI_FORMAT_R32G32_TYPELESS             ;
		case     GPU_ResourceFmt_R32G32_FLOAT              :  return DXGI_FORMAT_R32G32_FLOAT                ;
		case     GPU_ResourceFmt_R32G32_UINT               :  return DXGI_FORMAT_R32G32_UINT                 ;
		case     GPU_ResourceFmt_R32G32_SINT               :  return DXGI_FORMAT_R32G32_SINT                 ;
		case     GPU_ResourceFmt_R32G8X24_TYPELESS         :  return DXGI_FORMAT_R32G8X24_TYPELESS           ;
		case     GPU_ResourceFmt_D32_FLOAT_S8X24_UINT      :  return DXGI_FORMAT_D32_FLOAT_S8X24_UINT        ;
		case     GPU_ResourceFmt_R32_FLOAT_X8X24_TYPELESS  :  return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS    ;
		case     GPU_ResourceFmt_X32_TYPELESS_G8X24_UINT   :  return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT     ;
		case     GPU_ResourceFmt_R10G10B10A2_TYPELESS      :  return DXGI_FORMAT_R10G10B10A2_TYPELESS        ;
		case     GPU_ResourceFmt_R10G10B10A2_UNORM         :  return DXGI_FORMAT_R10G10B10A2_UNORM           ;
		case     GPU_ResourceFmt_R10G10B10A2_UINT          :  return DXGI_FORMAT_R10G10B10A2_UINT            ;
		case     GPU_ResourceFmt_R8G8B8A8_TYPELESS         :  return DXGI_FORMAT_R8G8B8A8_TYPELESS           ;
		case     GPU_ResourceFmt_R8G8B8A8_UNORM            :  return DXGI_FORMAT_R8G8B8A8_UNORM              ;
		case     GPU_ResourceFmt_R8G8B8A8_UINT             :  return DXGI_FORMAT_R8G8B8A8_UINT               ;
		case     GPU_ResourceFmt_R8G8B8A8_SNORM            :  return DXGI_FORMAT_R8G8B8A8_SNORM              ;
		case     GPU_ResourceFmt_R8G8B8A8_SINT             :  return DXGI_FORMAT_R8G8B8A8_SINT               ;
		case     GPU_ResourceFmt_R16G16_TYPELESS           :  return DXGI_FORMAT_R16G16_TYPELESS             ;
		case     GPU_ResourceFmt_R16G16_FLOAT              :  return DXGI_FORMAT_R16G16_FLOAT                ;
		case     GPU_ResourceFmt_R16G16_UNORM              :  return DXGI_FORMAT_R16G16_UNORM                ;
		case     GPU_ResourceFmt_R16G16_UINT               :  return DXGI_FORMAT_R16G16_UINT                 ;
		case     GPU_ResourceFmt_R16G16_SNORM              :  return DXGI_FORMAT_R16G16_SNORM                ;
		case     GPU_ResourceFmt_R16G16_SINT               :  return DXGI_FORMAT_R16G16_SINT                 ;
		case     GPU_ResourceFmt_R32_TYPELESS              :  return DXGI_FORMAT_R32_TYPELESS                ;
		case     GPU_ResourceFmt_D32_FLOAT                 :  return DXGI_FORMAT_D32_FLOAT                   ;
		case     GPU_ResourceFmt_R32_FLOAT                 :  return DXGI_FORMAT_R32_FLOAT                   ;
		case     GPU_ResourceFmt_R32_UINT                  :  return DXGI_FORMAT_R32_UINT                    ;
		case     GPU_ResourceFmt_R32_SINT                  :  return DXGI_FORMAT_R32_SINT                    ;
		case     GPU_ResourceFmt_R24G8_TYPELESS            :  return DXGI_FORMAT_R24G8_TYPELESS              ;
		case     GPU_ResourceFmt_D24_UNORM_S8_UINT         :  return DXGI_FORMAT_D24_UNORM_S8_UINT           ;
		case     GPU_ResourceFmt_R24_UNORM_X8_TYPELESS     :  return DXGI_FORMAT_R24_UNORM_X8_TYPELESS       ;
		case     GPU_ResourceFmt_X24_TYPELESS_G8_UINT      :  return DXGI_FORMAT_X24_TYPELESS_G8_UINT        ;
		case     GPU_ResourceFmt_R8G8_TYPELESS             :  return DXGI_FORMAT_R8G8_TYPELESS               ;
		case     GPU_ResourceFmt_R8G8_UNORM                :  return DXGI_FORMAT_R8G8_UNORM                  ;
		case     GPU_ResourceFmt_R8G8_UINT                 :  return DXGI_FORMAT_R8G8_UINT                   ;
		case     GPU_ResourceFmt_R8G8_SNORM                :  return DXGI_FORMAT_R8G8_SNORM                  ;
		case     GPU_ResourceFmt_R8G8_SINT                 :  return DXGI_FORMAT_R8G8_SINT                   ;
		case     GPU_ResourceFmt_R16_TYPELESS              :  return DXGI_FORMAT_R16_TYPELESS                ;
		case     GPU_ResourceFmt_R16_FLOAT                 :  return DXGI_FORMAT_R16_FLOAT                   ;
		case     GPU_ResourceFmt_D16_UNORM                 :  return DXGI_FORMAT_D16_UNORM                   ;
		case     GPU_ResourceFmt_R16_UNORM                 :  return DXGI_FORMAT_R16_UNORM                   ;
		case     GPU_ResourceFmt_R16_UINT                  :  return DXGI_FORMAT_R16_UINT                    ;
		case     GPU_ResourceFmt_R16_SNORM                 :  return DXGI_FORMAT_R16_SNORM                   ;
		case     GPU_ResourceFmt_R16_SINT                  :  return DXGI_FORMAT_R16_SINT                    ;
		case     GPU_ResourceFmt_R8_TYPELESS               :  return DXGI_FORMAT_R8_TYPELESS                 ;
		case     GPU_ResourceFmt_R8_UNORM                  :  return DXGI_FORMAT_R8_UNORM                    ;
		case     GPU_ResourceFmt_R8_UINT                   :  return DXGI_FORMAT_R8_UINT                     ;
		case     GPU_ResourceFmt_R8_SNORM                  :  return DXGI_FORMAT_R8_SNORM                    ;
		case     GPU_ResourceFmt_R8_SINT                   :  return DXGI_FORMAT_R8_SINT                     ;
		case     GPU_ResourceFmt_A8_UNORM                  :  return DXGI_FORMAT_A8_UNORM                    ;
	}

	unreachable();
	return DXGI_FORMAT_R8G8B8A8_UNORM;
}

//--------------------------------------------------------------------------------------
// Helper for compiling shaders with D3DCompile
//
// With VS 11, we could load up prebuilt .cso files instead...
//--------------------------------------------------------------------------------------
HRESULT TryCompileShaderFromFile(const WCHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut)
{
	HRESULT hr = S_OK;

	DWORD dwShaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
	// Set the D3DCOMPILE_DEBUG flag to embed debug information in the shaders.
	// Setting this flag improves the shader debugging experience, but still allows 
	// the shaders to be optimized and to run exactly the way they will run in 
	// the release configuration of this program.
	dwShaderFlags |= D3DCOMPILE_DEBUG;

	// Disable optimizations to further improve shader debugging
	dwShaderFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

	ID3DBlob* pErrorBlob = nullptr;
	Defer( { if (pErrorBlob) pErrorBlob->Release(); } );

	// Note on compiler macros: 
	//   Macros should match exactly what's being used to precompile shaders via the makefile.
	//   One possible way to do this is to have the project file write the active shader macro
	//   configuration into some larger macro that we process here and then pass to the runtime
	//   compiler.

#if defined(DX11_SHADER_COMPILER_MACROS)
	// DX11_SHADER_COMPILER_MACROS -
	//   a semicolon-delimited list of macros.  Translate it into a null-terminated macro list,
	//   because micorosoft is annoying and makes everything harder for us, the programmer.

#endif

	hr = D3DCompileFromFile(szFileName, nullptr, nullptr, szEntryPoint, szShaderModel,
		dwShaderFlags, 0, ppBlobOut, &pErrorBlob);

	if (FAILED(hr)) {
		if (pErrorBlob) {
			OutputDebugStringA(reinterpret_cast<const char*>(pErrorBlob->GetBufferPointer()));
		}
		else {
			log_and_abort("D3DCompileFromFile(%S) failed with no errorBlob, hr=0x%08x", szFileName, hr);
		}
	}

	return hr;
}

ID3DBlob* CompileShaderFromFile(const WCHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel)
{
	HRESULT hr = S_OK;
	ID3DBlob* ppBlobOut;

	hr = TryCompileShaderFromFile(szFileName, szEntryPoint, szShaderModel, &ppBlobOut);
	if (FAILED(hr)) {
		MessageBox(nullptr,
			L"The FX file cannot be compiled.  Please run this executable from the directory that contains the FX file.", L"Error", MB_OK
		);
	}

	return ppBlobOut;
}

void dx11_CleanupDevice()
{
	// ... one of these isn't like all the others... !!!

	if (g_pImmediateContext)	g_pImmediateContext		->ClearState();

	if (g_pSwapChain1)			g_pSwapChain1			->Release();
	if (g_pSwapChain)			g_pSwapChain			->Release();
	if (g_pImmediateContext1)	g_pImmediateContext1	->Release();
	if (g_pImmediateContext)	g_pImmediateContext		->Release();
	if (g_pd3dDevice1)			g_pd3dDevice1			->Release();
	if (g_pd3dDevice)			g_pd3dDevice			->Release();
}

// Our gpu interface provides a set of standard vertex buffer layouts to meet most of our common
// needs.  Uncommon needs can usually be met by using one of the more versatile vertex buffer
// layouts -- just treat unused fields as padding data (neither initialized or used by shader)
// Such layouts can be optimized later if seen fit, eg. there's actually enough geometry and meshes
// using the vertex type to justify baking a new layout into the GPU interface.

static const D3D11_INPUT_ELEMENT_DESC layout_color[] =
{
	{ "POSITION",	0, DXGI_FORMAT_R32G32B32_FLOAT,		0, 0,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "COLOR",		0, DXGI_FORMAT_R32G32B32A32_FLOAT,	0, 12,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
};

static const D3D11_INPUT_ELEMENT_DESC layout_tex1[] =
{
	{ "POSITION",	0, DXGI_FORMAT_R32G32B32_FLOAT,		0, 0,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD",	0, DXGI_FORMAT_R32G32_FLOAT,		0, 12,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
};

static const D3D11_INPUT_ELEMENT_DESC layout_colortex1[] =
{
	{ "POSITION",	0, DXGI_FORMAT_R32G32B32_FLOAT,		0, 0,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "COLOR",		0, DXGI_FORMAT_R32G32B32A32_FLOAT,	0, 12,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD",	0, DXGI_FORMAT_R32G32_FLOAT,		0, 28,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
};

static const D3D11_INPUT_ELEMENT_DESC layout_colortex4[] =
{
	{ "POSITION",	0, DXGI_FORMAT_R32G32B32_FLOAT,		0, 0,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "COLOR",		0, DXGI_FORMAT_R32G32B32A32_FLOAT,	0, 12,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD",	0, DXGI_FORMAT_R32G32_FLOAT,		0, 28,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD",	1, DXGI_FORMAT_R32G32_FLOAT,		0, 36,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD",	2, DXGI_FORMAT_R32G32_FLOAT,		0, 44,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD",	3, DXGI_FORMAT_R32G32_FLOAT,		0, 52,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
};

const D3D11_INPUT_ELEMENT_DESC* getVertexBufferLayout(GPU_VertexBufferLayoutType layoutType)
{
	switch (layoutType)
	{
		case VertexBufferLayout_Color:		return layout_color;
		case VertexBufferLayout_Tex1:		return layout_tex1;
		case VertexBufferLayout_ColorTex1:	return layout_colortex1;
		case VertexBufferLayout_ColorTex4:	return layout_colortex4;

		default: unreachable();
	}

	unreachable();
	return nullptr;
}

int getVertexBufferLayoutSize(GPU_VertexBufferLayoutType layoutType)
{
	switch (layoutType)
	{
		case VertexBufferLayout_Color:		return bulkof(layout_color		);
		case VertexBufferLayout_Tex1:		return bulkof(layout_tex1		);
		case VertexBufferLayout_ColorTex1:	return bulkof(layout_colortex1	);
		case VertexBufferLayout_ColorTex4:	return bulkof(layout_colortex4	);

		default: unreachable();
	}

	unreachable();
	return 0;
}

ID3DBlob* LoadShaderBlobVS(const xString& srcfile, const char* entryPointFn)
{
	bug_on( !entryPointFn || !entryPointFn[0] );
	ID3DBlob* blob = CompileShaderFromFile(toUTF16(srcfile).wc_str(), entryPointFn, "vs_4_0");
	return blob;
}

void dx11_InitDevice()
{
	HRESULT hr = S_OK;

	RECT rc;
	GetClientRect(g_hWnd, &rc);
	UINT width = rc.right - rc.left;
	UINT height = rc.bottom - rc.top;

	UINT createDeviceFlags = 0;
#ifdef _DEBUG
	createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	D3D_DRIVER_TYPE driverTypes[] =
	{
		D3D_DRIVER_TYPE_HARDWARE,
		D3D_DRIVER_TYPE_WARP,
		D3D_DRIVER_TYPE_REFERENCE,
	};
	UINT numDriverTypes = ARRAYSIZE(driverTypes);

	D3D_FEATURE_LEVEL featureLevels[] =
	{
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
	};
	UINT numFeatureLevels = ARRAYSIZE(featureLevels);

	for (UINT driverTypeIndex = 0; driverTypeIndex < numDriverTypes; driverTypeIndex++)
	{
		g_driverType = driverTypes[driverTypeIndex];
		hr = D3D11CreateDevice(nullptr, g_driverType, nullptr, createDeviceFlags, featureLevels, numFeatureLevels,
			D3D11_SDK_VERSION, &g_pd3dDevice, &g_featureLevel, &g_pImmediateContext);

		if (hr == E_INVALIDARG)
		{
			// DirectX 11.0 platforms will not recognize D3D_FEATURE_LEVEL_11_1 so we need to retry without it
			hr = D3D11CreateDevice(nullptr, g_driverType, nullptr, createDeviceFlags, &featureLevels[1], numFeatureLevels - 1,
				D3D11_SDK_VERSION, &g_pd3dDevice, &g_featureLevel, &g_pImmediateContext);
		}

		if (SUCCEEDED(hr))
			break;
	}

	log_and_abort_on (FAILED(hr));

	//  * DX Docs recommend always having MSAA enabled.
	//  * Depth Bias options are pretty much jsut for shadow implementations
	//  * FrontCounterClockwise is *probably* a system-wide setting.  Can't think of a good reason to have
	//    models with mixed clockwise/counter-clockwise vertex order.  Preprocess them bitches!
	//  * Wireframe of any raster state should be created, simply for debugging purposes.
	//  * That leaves just Cull Mode and Scissor Enable as the only other wildcards.

	D3D11_RASTERIZER_DESC drd = {
		D3D11_FILL_SOLID,	//D3D11_FILL_MODE FillMode;
		D3D11_CULL_NONE,	//D3D11_CULL_MODE CullMode;
		FALSE,				//BOOL FrontCounterClockwise;
		0,					//INT DepthBias;
		0.0f,				//FLOAT DepthBiasClamp;
		0.0f,				//FLOAT SlopeScaledDepthBias;
		TRUE,				//BOOL DepthClipEnable;
		FALSE,				//BOOL ScissorEnable;
		TRUE,				//BOOL MultisampleEnable;
		FALSE				//BOOL AntialiasedLineEnable;        
	};

	for (int fill=0; fill<_GPU_Fill_Count_; ++fill) {
		switch(fill) {
			case GPU_Fill_Solid:		drd.FillMode = D3D11_FILL_SOLID;		break;
			case GPU_Fill_Wireframe:	drd.FillMode = D3D11_FILL_WIREFRAME;	break;
			default: __unreachable();
		}

		for (int cull=0; cull<_GPU_Cull_Count_; ++cull) {
			switch(cull) {
				case GPU_Cull_None:		drd.CullMode = D3D11_CULL_NONE;		break;
				case GPU_Cull_Front:	drd.CullMode = D3D11_CULL_FRONT;	break;
				case GPU_Cull_Back:		drd.CullMode = D3D11_CULL_BACK;		break;
				default: __unreachable();
			}

			for (int scissor=0; scissor<_GPU_Scissor_Count_; ++scissor) {
				drd.ScissorEnable = (scissor == GPU_Scissor_Enable) ? 1 : 0;
				hr = g_pd3dDevice->CreateRasterizerState(&drd, &g_RasterState[fill][cull][scissor]);
				bug_on( FAILED( hr ));
			}
		}
	}

	// Obtain DXGI factory from device (since we used nullptr for pAdapter above)
	IDXGIFactory1* dxgiFactory = nullptr;
	{
		IDXGIDevice* dxgiDevice = nullptr;
		hr = g_pd3dDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
		if (SUCCEEDED(hr))
		{
			IDXGIAdapter* adapter = nullptr;
			hr = dxgiDevice->GetAdapter(&adapter);
			if (SUCCEEDED(hr))
			{
				hr = adapter->GetParent(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&dxgiFactory));
				adapter->Release();
			}
			dxgiDevice->Release();
		}
	}
	log_and_abort_on (hr);

	// Create swap chain
	IDXGIFactory2* dxgiFactory2 = nullptr;
	hr = dxgiFactory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&dxgiFactory2));
	if (dxgiFactory2)
	{
		// DirectX 11.1 or later
		hr = g_pd3dDevice->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&g_pd3dDevice1));
		if (SUCCEEDED(hr))
		{
			(void)g_pImmediateContext->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&g_pImmediateContext1));
		}

		DXGI_SWAP_CHAIN_DESC1 sd = {};
		sd.Width                = width;
		sd.Height               = height;
		sd.Format               = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.SampleDesc.Count     = 1;
		sd.SampleDesc.Quality   = 0;
		sd.BufferUsage          = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount          = 1;

		hr = dxgiFactory2->CreateSwapChainForHwnd(g_pd3dDevice, g_hWnd, &sd, nullptr, nullptr, &g_pSwapChain1);
		if (SUCCEEDED(hr))
		{
			hr = g_pSwapChain1->QueryInterface(__uuidof(IDXGISwapChain), reinterpret_cast<void**>(&g_pSwapChain));
		}

		dxgiFactory2->Release();
	} else
	{
		// DirectX 11.0 systems
		DXGI_SWAP_CHAIN_DESC sd = {};
		sd.BufferCount          = 1;
		sd.BufferDesc.Width     = width;
		sd.BufferDesc.Height    = height;
		sd.BufferDesc.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.BufferDesc.RefreshRate.Numerator = 60;
		sd.BufferDesc.RefreshRate.Denominator = 1;
		sd.BufferUsage          = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.OutputWindow         = g_hWnd;
		sd.SampleDesc.Count     = 1;
		sd.SampleDesc.Quality   = 0;
		sd.Windowed             = TRUE;

		hr = dxgiFactory->CreateSwapChain(g_pd3dDevice, &sd, &g_pSwapChain);
	}
	log_and_abort_on(FAILED(hr));

	// Note this tutorial doesn't handle full-screen swapchains so we block the ALT+ENTER shortcut
	dxgiFactory->MakeWindowAssociation(g_hWnd, DXGI_MWA_NO_ALT_ENTER);
	dxgiFactory->Release();

	ID3D11Texture2D* pBackBuffer = nullptr;
	hr = g_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pBackBuffer));
	log_and_abort_on(FAILED(hr));

	auto&	rtView	= ptr_cast<ID3D11RenderTargetView*&>(g_gpu_BackBuffer.m_driverData);

	g_gpu_BackBuffer.m_driverData;
	hr = g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &rtView);
	pBackBuffer->Release();
	log_and_abort_on(FAILED(hr));

	g_pImmediateContext->OMSetRenderTargets(1, &rtView, nullptr);

	// Setup the viewport
	D3D11_VIEWPORT vp;
	vp.Width	= (float)width;
	vp.Height	= (float)height;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	vp.TopLeftX = 0;
	vp.TopLeftY = 0;
	g_pImmediateContext->RSSetViewports(1, &vp);

	D3D11_SAMPLER_DESC samplerDesc = {};

//	samplerDesc.Filter			= D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.Filter			= D3D11_FILTER_MIN_MAG_MIP_POINT;
	samplerDesc.AddressU		= D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressV		= D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressW		= D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.ComparisonFunc	= D3D11_COMPARISON_NEVER;
	samplerDesc.MinLOD			= 0;
	samplerDesc.MaxLOD			= D3D11_FLOAT32_MAX;

	// Create the sampler
	hr = g_pd3dDevice->CreateSamplerState( &samplerDesc, &m_pTextureSampler);
	bug_on(FAILED(hr));

	XMVECTOR Eye	= XMVectorSet( 0.0f, 1.0f, -5.0f, 0.0f );
	XMVECTOR At		= XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f );
	XMVECTOR Up		= XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f );

	g_World			= XMMatrixIdentity();
	g_View			= XMMatrixLookAtLH( Eye, At, Up );
	g_Projection	= XMMatrixPerspectiveFovLH( XM_PIDIV2, width / (FLOAT)height, 0.01f, 100.0f );

	// TODO : generate these InputLayout shaders at runtime, according to the InputLayout
	//        specifications defined within and supported by our engine.

	for (int i=0; i<VertexBufferLayout_NUM_LAYOUTS; ++i) {
		auto vbl = (GPU_VertexBufferLayoutType)i;
		auto* IALayoutBlob = LoadShaderBlobVS("..\\ajek-framework\\shaders\\VertexInputLayouts.fx", toString(vbl));

		// Create the input layout
		hr = g_pd3dDevice->CreateInputLayout(
			getVertexBufferLayout		(vbl),
			getVertexBufferLayoutSize	(vbl),
			IALayoutBlob->GetBufferPointer(),
			IALayoutBlob->GetBufferSize(),
			&g_pVertexLayouts[vbl]
		);
		IALayoutBlob->Release();
	}
	log_and_abort_on(FAILED(hr));
}

void dx11_SetInputLayout()
{
	// Set the input layout
	g_pImmediateContext->IASetInputLayout(g_pVertexLayouts[VertexBufferLayout_Tex1]);
}

bool dx11_LoadShaderVS(GPU_ShaderVS& dest, const xString& srcfile, const char* entryPointFn)
{
	HRESULT hr;

	bug_on( !entryPointFn || !entryPointFn[0] );
	ID3DBlob* blob = CompileShaderFromFile(toUTF16(srcfile).wc_str(), entryPointFn, "vs_4_0");
	if (!blob) return false;

	auto& shader = ptr_cast<ID3D11VertexShader* &>(dest.m_driverData);
	if (shader) {
		log_host(
			"Unloading existing vertex shader resource:\n"
			" > %s",
			cPtrStr(shader)
		);
		shader->Release();
	}

	hr = g_pd3dDevice->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader);
	log_and_abort_on(FAILED(hr));

	blob->Release();
	return true;
}

bool dx11_LoadShaderFS(GPU_ShaderFS& dest, const xString& srcfile, const char* entryPointFn)
{
	HRESULT hr;

	bug_on( !entryPointFn || !entryPointFn[0] );
	ID3DBlob* blob = CompileShaderFromFile(toUTF16(srcfile).wc_str(), entryPointFn, "ps_4_0");
	if (!blob) return false;

	auto& shader = ptr_cast<ID3D11PixelShader* &>(dest.m_driverData);
	if (shader) {
		log_host(
			"Unloading existing fragment shader resource:\n"
			" > %s",
			cPtrStr(shader)
		);
		shader->Release();
	}

	hr = g_pd3dDevice->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader);
	log_and_abort_on(FAILED(hr));

	return true;
}

void dx11_BindShaderVS(GPU_ShaderVS& vs)
{
	auto&	shader	= ptr_cast<ID3D11VertexShader* const&>(vs.m_driverData);
	g_pImmediateContext->VSSetShader(shader, nullptr, 0);
}

void dx11_BindShaderFS(GPU_ShaderFS& fs)
{
	auto&	shader	= ptr_cast<ID3D11PixelShader* const&>(fs.m_driverData);
	g_pImmediateContext->PSSetShader(shader, nullptr, 0);
}

void dx11_SetVertexBuffer( int bufferId, int shaderSlot, int _stride, int _offset)
{
	uint stride = _stride;
	uint offset = _offset;

	g_pImmediateContext->IASetVertexBuffers(0, 1, &g_DynVertBuffers[g_curBufferIdx].Buffers[bufferId], &stride, &offset);
}

void dx11_SetVertexBuffer( const GPU_VertexBuffer& vbuffer, int shaderSlot, int _stride, int _offset)
{
	uint stride = _stride;
	uint offset = _offset;

	g_pImmediateContext->IASetVertexBuffers(0, 1, (ID3D11Buffer**)&vbuffer.m_driverData, &stride, &offset);
}

void dx11_SetIndexBuffer(GPU_IndexBuffer indexBuffer, int bitsPerIndex, int offset)
{
	DXGI_FORMAT format;
	switch (bitsPerIndex) {
		case 8:		format = DXGI_FORMAT_R8_UINT;		break;
		case 16:	format = DXGI_FORMAT_R16_UINT;		break;
		case 32:	format = DXGI_FORMAT_R32_UINT;		break;
		default:	unreachable("Invalid parameter 'bitsPerindex=%d'", bitsPerIndex);
	}
	g_pImmediateContext->IASetIndexBuffer( (ID3D11Buffer*)indexBuffer.m_driverData, format, offset);
}

void dx11_DrawIndexed(int indexCount, int startIndexLoc, int baseVertLoc)
{
	g_pImmediateContext->DrawIndexed(indexCount, startIndexLoc, baseVertLoc);
}

void dx11_UploadDynamicBufferData(int bufferIdx, void* srcData, int sizeInBytes)
{
	D3D11_MAPPED_SUBRESOURCE mappedResource = {};

	auto*   simple      = g_DynVertBuffers[g_curBufferIdx].Buffers[bufferIdx];

	g_pImmediateContext->Map(simple, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
	xMemCopy(mappedResource.pData, srcData, sizeInBytes);
	g_pImmediateContext->Unmap(simple, 0);
}

int dx11_CreateDynamicVertexBuffer(int bufferSizeInBytes)
{
	int bufferIdx = g_DynVertBufferCount;
	for (int i=0; i<BackBufferCount; ++i) {
		D3D11_BUFFER_DESC bd = {};
		bd.Usage			= D3D11_USAGE_DYNAMIC;
		bd.ByteWidth		= bufferSizeInBytes;
		bd.BindFlags		= D3D11_BIND_VERTEX_BUFFER;
		bd.CPUAccessFlags	= D3D11_CPU_ACCESS_WRITE;

		auto hr = g_pd3dDevice->CreateBuffer(&bd, nullptr, &g_DynVertBuffers[i].Buffers[bufferIdx]);
		bug_on (FAILED(hr));
	}
	g_DynVertBufferCount += 1;
	return bufferIdx;
}

GPU_VertexBuffer dx11_CreateStaticMesh(void* vertexData, int itemSizeInBytes, int vertexCount)
{
	D3D11_BUFFER_DESC bd = {};

	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.ByteWidth = itemSizeInBytes * vertexCount;
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	bd.CPUAccessFlags = 0;
	D3D11_SUBRESOURCE_DATA InitData = {};
	InitData.pSysMem = vertexData;

	GPU_VertexBuffer result;
	auto hr = g_pd3dDevice->CreateBuffer( &bd, &InitData, (ID3D11Buffer**)&result.m_driverData );

	bug_on(FAILED(hr));
	return result;
}

GPU_IndexBuffer dx11_CreateIndexBuffer(void* indexBuffer, int bufferSize)
{
	D3D11_SUBRESOURCE_DATA	InitData	= {};
	D3D11_BUFFER_DESC		bd			= {};

	bd.Usage			= D3D11_USAGE_DEFAULT;
	bd.ByteWidth		= bufferSize;
	bd.BindFlags		= D3D11_BIND_INDEX_BUFFER;
	bd.CPUAccessFlags	= 0;
	InitData.pSysMem	= indexBuffer;

	ID3D11Buffer* handle;
	auto hr = g_pd3dDevice->CreateBuffer( &bd, &InitData, &handle );
	bug_on (FAILED(hr));

	return (GPU_IndexBuffer)handle;
}

void dx11_BackbufferSwap()
{
	g_pSwapChain->Present(0, 0);
	g_curBufferIdx = (g_curBufferIdx+1) % BackBufferCount;
}

void dx11_SetPrimType(GpuPrimitiveType primType)
{
	D3D_PRIMITIVE_TOPOLOGY dxPrimTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

	switch(primType) {
		case GPU_PRIM_POINTLIST		: dxPrimTopology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST		;	break;
		case GPU_PRIM_LINELIST		: dxPrimTopology = D3D_PRIMITIVE_TOPOLOGY_LINELIST		;	break;
		case GPU_PRIM_LINESTRIP		: dxPrimTopology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP		;	break;
		case GPU_PRIM_TRIANGLELIST	: dxPrimTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST	;	break;
		case GPU_PRIM_TRIANGLESTRIP	: dxPrimTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP	;	break;

		default: unreachable("");
	}
	
	g_pImmediateContext->IASetPrimitiveTopology(dxPrimTopology);
}

void dx11_SetRasterState(GpuRasterFillMode fill, GpuRasterCullMode cull, GpuRasterScissorMode scissor)
{
	if (g_gpu_ForceWireframe) {
		fill = GPU_Fill_Wireframe;
	}
	g_pImmediateContext->RSSetState(g_RasterState[fill][cull][scissor]);
	g_pImmediateContext->PSSetSamplers( 0, 1, &m_pTextureSampler );
}

void dx11_BindShaderResource(const GPU_ShaderResource& res, int startSlot)
{
	auto&	resourceView	= ptr_cast<ID3D11ShaderResourceView* const&>(res.m_driverData_view);
	g_pImmediateContext->PSSetShaderResources( startSlot, 1, &resourceView );
}

void dx11_CreateTexture2D(GPU_TextureResource2D& dest, const void* src_bitmap_data, int width, int height, GPU_ResourceFmt format)
{
	HRESULT hr;

	auto&	texture		= ptr_cast<ID3D11Texture2D*&>			(dest.m_driverData_tex );
	auto&	textureView	= ptr_cast<ID3D11ShaderResourceView*&>	(dest.m_driverData_view);

	// See if format is supported for auto-gen mipmaps (varies by feature level)
	// (Must have context and shader-view to auto generate mipmaps)

	DXGI_FORMAT dxfmt = get_DXGI_Format(format);

	bool autogen_mipmaps = false;
	UINT fmtSupport = 0;
	hr = g_pd3dDevice->CheckFormatSupport(dxfmt, &fmtSupport);
	if (SUCCEEDED(hr) && (fmtSupport & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN))
	{
		autogen_mipmaps = true;
	}

	// Create texture
	D3D11_TEXTURE2D_DESC desc = {};

	desc.Width				= width;
	desc.Height				= height;
	desc.MipLevels			= (autogen_mipmaps) ? 0 : 1;
	desc.ArraySize			= 1;
	desc.Format				= dxfmt;
	desc.SampleDesc.Count	= 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage				= D3D11_USAGE_DEFAULT;
	desc.CPUAccessFlags		= 0;

	desc.BindFlags			= D3D11_BIND_SHADER_RESOURCE; 
	desc.MiscFlags			= 0;
	
	if (autogen_mipmaps)
	{
		desc.BindFlags	   |= D3D11_BIND_RENDER_TARGET;
		desc.MiscFlags	   |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
	}

	int bytespp = 4;
	if (format == GPU_ResourceFmt_R32G32_UINT) {
		bytespp = 8;
	}

	if (format == GPU_ResourceFmt_R32G32_FLOAT) {
		bytespp = 8;
	}

	int rowPitch = ((width * bytespp));

	D3D11_SUBRESOURCE_DATA initData;
	initData.pSysMem			= src_bitmap_data;
	initData.SysMemPitch		= rowPitch;
	initData.SysMemSlicePitch	= rowPitch * height;

	hr = g_pd3dDevice->CreateTexture2D(&desc, (autogen_mipmaps) ? nullptr : &initData, &texture);
	log_and_abort_on(FAILED(hr) || !texture);

	// textureView bits

	D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
	SRVDesc.Format				= desc.Format;
	SRVDesc.ViewDimension		= D3D11_SRV_DIMENSION_TEXTURE2D;
	SRVDesc.Texture2D.MipLevels = autogen_mipmaps ? -1 : 1;

	hr = g_pd3dDevice->CreateShaderResourceView(texture, &SRVDesc, &textureView);
	log_and_abort_on (FAILED(hr));

	if (autogen_mipmaps)
	{

#if defined(_XBOX_ONE) && defined(_TITLE)
		// reference code left in from when I copied this mess from DirectXTK.
		// Has not been updated or tested.  --jstine

		ID3D11Texture2D *pStaging = nullptr;
		CD3D11_TEXTURE2D_DESC stagingDesc(format, twidth, theight, 1, 1, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, 1, 0, 0);
		initData.pSysMem = temp.get();
		initData.SysMemPitch = static_cast<UINT>(rowPitch);
		initData.SysMemSlicePitch = static_cast<UINT>(imageSize);

		hr = d3dDevice->CreateTexture2D(&stagingDesc, &initData, &pStaging);
		if (SUCCEEDED(hr))
		{
			d3dContext->CopySubresourceRegion(tex, 0, 0, 0, 0, pStaging, 0, nullptr);

			UINT64 copyFence = d3dContextX->InsertFence(0);
			while (d3dDeviceX->IsFencePending(copyFence)) { SwitchToThread(); }
			pStaging->Release();
		}
#else
		g_pImmediateContext->UpdateSubresource(texture, 0, nullptr, src_bitmap_data, rowPitch, rowPitch * height);
#endif
		g_pImmediateContext->GenerateMips(textureView);
	}
}

void dx11_ClearRenderTarget(const GPU_RenderTarget& target, const float4& color)
{
	g_pImmediateContext->ClearRenderTargetView((ID3D11RenderTargetView*)target.m_driverData, color.f);
}

#if 0

IWICImagingFactory* factory = nullptr;

void WIC_Initialize()
{
	IWICImagingFactory* factory = nullptr;

	HRESULT hr = CoCreateInstance(
		CLSID_WICImagingFactory2,
		nullptr,
		CLSCTX_INPROC_SERVER,
		__uuidof(IWICImagingFactory2),
		(LPVOID*)&factory
	);
}

void WIC_TextureFromFile(const xString& filename)
{
	IWICBitmapDecoder decoder;
	HRESULT hr = pWIC->CreateDecoderFromFilename(fileName, 0, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
	bug_on(FAILED(hr));

	IWICBitmapFrameDecode frame;
	hr = decoder->GetFrame(0, frame.GetAddressOf());
}

#endif
