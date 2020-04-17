#include "renderer.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"
#include "atlstr.h"

void Renderer::OnInit()
{
	LoadPipeline();
	LoadAssets();
}

void Renderer::OnUpdate()
{
	angle += delta_rotation;
	eye_position += XMVECTOR{ sin(angle), 0.f, cos(angle) } * delta_forward;

	XMVECTOR focusPos = eye_position + XMVECTOR{ sin(angle), 0.f, cos(angle) };

	XMVECTOR upDirection = { 0.f, 1.f, 0.f };
	view = XMMatrixLookAtLH(eye_position, focusPos, upDirection);
	mwp = projection * view * world;

	memcpy(constant_buffer_data_begin, &mwp, sizeof(mwp));
}

void Renderer::OnRender()
{
	PopulateCommandList();
	ID3D12CommandList* commandList[] = { command_list.Get() };

	command_queue->ExecuteCommandLists(_countof(commandList), commandList);

	ThrowIfFailed(swap_chain->Present(0, 0));
	WaitForPreviousFrame();
}

void Renderer::OnDestroy()
{
	WaitForPreviousFrame();
	CloseHandle(fence_event);
}

void Renderer::OnKeyDown(CString key)
{
	if (key == _T("W")) {
		delta_forward = -0.001f;
	}
	else if (key == _T("S")) {
		delta_forward = 0.001f;
	}
	else if (key == _T("A")) {
		delta_rotation = -0.001f;
	}
	else if (key == _T("D")) {
		delta_rotation = 0.001f;
	}

}

void Renderer::OnKeyUp(CString key)
{
	if (key == _T("W")) {
		delta_forward = 0.f;
	}
	else if (key == _T("S")) {
		delta_forward = 0.f;
	}
	else if (key == _T("A")) {
		delta_rotation = 0.f;
	}
	else if (key == _T("D")) {
		delta_rotation = 0.f;
	}
}

void Renderer::LoadPipeline()
{
	// Create debug layer
	UINT dxgiFactoryFlag = 0;
#ifdef DEBUG
	ComPtr<ID3D12Debug> debugController;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
	{
		debugController->EnableDebugLayer();
		dxgiFactoryFlag |= DXGI_CREATE_FACTORY_DEBUG;
	}
#endif

	// Create device
	ComPtr<IDXGIFactory4> dxgiFactory;
	ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlag, IID_PPV_ARGS(&dxgiFactory)));

	ComPtr<IDXGIAdapter1> hardwareAdapter;
	ThrowIfFailed(dxgiFactory->EnumAdapters1(0, &hardwareAdapter));
	ThrowIfFailed(D3D12CreateDevice(hardwareAdapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));

	// Create a direct command queue 
	D3D12_COMMAND_QUEUE_DESC queueDescriptor = {};
	queueDescriptor.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDescriptor.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	ThrowIfFailed(device->CreateCommandQueue(&queueDescriptor, IID_PPV_ARGS(&command_queue)));

	// Create swap chain
	DXGI_SWAP_CHAIN_DESC1 swapChainDescriptor = {};
	swapChainDescriptor.BufferCount = frame_number;
	swapChainDescriptor.Width = GetWidth();
	swapChainDescriptor.Height = GetHeight();
	swapChainDescriptor.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDescriptor.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDescriptor.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDescriptor.SampleDesc.Count = 1;

	ComPtr<IDXGISwapChain1> tempSwapChain;
	ThrowIfFailed(dxgiFactory->CreateSwapChainForHwnd(
		command_queue.Get(),
		Win32Window::GetHwnd(),
		&swapChainDescriptor,
		nullptr,
		nullptr,
		&tempSwapChain
	));
	ThrowIfFailed(dxgiFactory->MakeWindowAssociation(Win32Window::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));
	ThrowIfFailed(tempSwapChain.As(&swap_chain));

	frame_index = swap_chain->GetCurrentBackBufferIndex();

	// Create descriptor heap for render target view
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDescriptor = {};
	rtvHeapDescriptor.NumDescriptors = frame_number;
	rtvHeapDescriptor.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDescriptor.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	ThrowIfFailed(device->CreateDescriptorHeap(&rtvHeapDescriptor, IID_PPV_ARGS(&rtv_heap)));
	rtv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDescriptor = {};
	cbvHeapDescriptor.NumDescriptors = 1;
	cbvHeapDescriptor.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	cbvHeapDescriptor.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	ThrowIfFailed(device->CreateDescriptorHeap(&cbvHeapDescriptor, IID_PPV_ARGS(&cbv_heap)));

	// Create render target view for each frame
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtv_heap->GetCPUDescriptorHandleForHeapStart());
	for (UINT i = 0; i < frame_number; i++)
	{
		ThrowIfFailed(swap_chain->GetBuffer(i, IID_PPV_ARGS(&render_targets[i])));
		device->CreateRenderTargetView(render_targets[i].Get(), nullptr, rtvHandle);
		rtvHandle.Offset(1, rtv_descriptor_size);
	}

	// Create command allocator
	ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocator)));
}

void Renderer::LoadAssets()
{
	// Create a root signature
	D3D12_FEATURE_DATA_ROOT_SIGNATURE rsFeatureData = {};
	rsFeatureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rsFeatureData, sizeof(rsFeatureData))))
	{
		rsFeatureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
	}

	CD3DX12_DESCRIPTOR_RANGE1 ranges[1];
	CD3DX12_ROOT_PARAMETER1 rootParameters[1];

	ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
	rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_VERTEX);

	D3D12_ROOT_SIGNATURE_FLAGS rsFlags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
		| D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
		| D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
		| D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS
		| D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDescriptor;
	rootSignatureDescriptor.Init_1_1(_countof(rootParameters), rootParameters, 0, nullptr, rsFlags);

	ComPtr<ID3D10Blob> signature;
	ComPtr<ID3D10Blob> error;
	ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&rootSignatureDescriptor, rsFeatureData.HighestVersion,
		&signature, &error));
	ThrowIfFailed(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root_signature)));

	// Create full PSO
	ComPtr<ID3D10Blob> vertexShader;
	ComPtr<ID3D10Blob> pixelShader;

	UINT compile_flags = 0;
#ifdef _DEBUG
	compile_flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

	std::wstring shaderPath = GetBinPath(std::wstring(L"shaders.hlsl"));
	ThrowIfFailed(D3DCompileFromFile(shaderPath.c_str(), nullptr, nullptr, "VSMain", "vs_5_0",
		compile_flags, 0, &vertexShader, &error));
	ThrowIfFailed(D3DCompileFromFile(shaderPath.c_str(), nullptr, nullptr, "PSMain", "ps_5_0",
		compile_flags, 0, &pixelShader, &error));

	D3D12_INPUT_ELEMENT_DESC inputElementDescriptor[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDescriptor = {};
	psoDescriptor.InputLayout = { inputElementDescriptor, _countof(inputElementDescriptor) };
	psoDescriptor.pRootSignature = root_signature.Get();
	psoDescriptor.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
	psoDescriptor.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
	psoDescriptor.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDescriptor.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	psoDescriptor.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
	psoDescriptor.RasterizerState.DepthClipEnable = FALSE;
	psoDescriptor.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDescriptor.DepthStencilState.DepthEnable = FALSE;
	psoDescriptor.DepthStencilState.StencilEnable = FALSE;
	psoDescriptor.SampleMask = UINT_MAX;
	psoDescriptor.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDescriptor.NumRenderTargets = 1;
	psoDescriptor.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDescriptor.SampleDesc.Count = 1;
	ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDescriptor, IID_PPV_ARGS(&pipeline_state)));

	// Create command list
	ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocator.Get(), \
		pipeline_state.Get(), IID_PPV_ARGS(&command_list)));
	ThrowIfFailed(command_list->Close());

	// Create and upload vertex buffer
	std::wstring objDirectory = GetBinPath(std::wstring());
	std::string objPath(objDirectory.begin(), objDirectory.end());
	std::string objFile = objPath + "CornellBox-Original.obj";
	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;

	std::string warn;
	std::string err;

	bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, objFile.c_str(), objPath.c_str());

	if (!warn.empty()) 
	{
		std::wstring w_warn(warn.begin(), warn.end());
		w_warn = L"Tiny OBJ reader warning: " + w_warn + L"\n";
		OutputDebugString(w_warn.c_str());
	}

	if (!err.empty()) 
	{
		std::wstring e_err(err.begin(), err.end());
		e_err = L"Tiny OBJ reader error: " + e_err + L"\n";
		OutputDebugString(e_err.c_str());
	}

	if (!ret) {
		ThrowIfFailed(-1);
	}

	// Loop over shapes
	for (size_t s = 0; s < shapes.size(); s++) 
	{
		// Loop over faces(polygon)
		size_t index_offset = 0;
		for (size_t f = 0; f < shapes[s].mesh.num_face_vertices.size(); f++) 
		{
			int fv = shapes[s].mesh.num_face_vertices[f];

			// Loop over vertices in the face.
			int materialIds = shapes[s].mesh.material_ids[f];
			for (size_t v = 0; v < fv; v++) {
				// access to vertex
				tinyobj::index_t idx = shapes[s].mesh.indices[index_offset + v];
				tinyobj::real_t vx = attrib.vertices[3 * idx.vertex_index + 0];
				tinyobj::real_t vy = attrib.vertices[3 * idx.vertex_index + 1];
				tinyobj::real_t vz = attrib.vertices[3 * idx.vertex_index + 2];
				materials[materialIds].diffuse;
				ColorVertex vertex = { 
					{vx, vy, vz} , 
					{materials[materialIds].diffuse[0], materials[materialIds].diffuse[1], materials[materialIds].diffuse[2], 1.f}
				};
				verteces.push_back(vertex);
			}
			index_offset += fv;
		}
	}

	const UINT vertexBufferSize = sizeof(ColorVertex) * verteces.size();
	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&vertex_buffer)
	));

	UINT8* vertexDataBegin;
	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(vertex_buffer->Map(0, &readRange, reinterpret_cast<void**>(&vertexDataBegin)));
	memcpy(vertexDataBegin, verteces.data(), sizeof(ColorVertex) * verteces.size());
	vertex_buffer->Unmap(0, nullptr);

	vertex_buffer_view.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
	vertex_buffer_view.StrideInBytes = sizeof(ColorVertex);
	vertex_buffer_view.SizeInBytes = vertexBufferSize;

	// Constant buffer init
	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(1024 * 64),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&constant_buffer)
	));

	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDescriptor = {};
	cbvDescriptor.BufferLocation = constant_buffer->GetGPUVirtualAddress();
	cbvDescriptor.SizeInBytes = (sizeof(mwp) + 255) & ~255;
	device->CreateConstantBufferView(&cbvDescriptor, cbv_heap->GetCPUDescriptorHandleForHeapStart());

	ThrowIfFailed(constant_buffer->Map(0, &readRange, reinterpret_cast<void**>(&constant_buffer_data_begin)));
	memcpy(constant_buffer_data_begin, &mwp, sizeof(mwp));

	// Create synchronization objects
	ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
	fence_value = 1;
	fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (fence_event == nullptr)
	{
		ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
	}
}

void Renderer::PopulateCommandList()
{
	// Reset allocators and lists
	ThrowIfFailed(command_allocator->Reset());

	ThrowIfFailed(command_list->Reset(command_allocator.Get(), pipeline_state.Get()));

	// Set initial state
	command_list->SetGraphicsRootSignature(root_signature.Get());
	ID3D12DescriptorHeap* heaps[] = {cbv_heap.Get()};
	command_list->SetDescriptorHeaps(_countof(heaps), heaps);
	command_list->SetGraphicsRootDescriptorTable(0, cbv_heap->GetGPUDescriptorHandleForHeapStart());
	command_list->RSSetViewports(1, &view_port);
	command_list->RSSetScissorRects(1, &scissor_rect);

	// Resource barrier from present to RT
	command_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
		render_targets[frame_index].Get(),
		D3D12_RESOURCE_STATE_PRESENT,
		D3D12_RESOURCE_STATE_RENDER_TARGET
	));

	// Record commands
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandler(rtv_heap->GetCPUDescriptorHandleForHeapStart(), frame_index, rtv_descriptor_size);
	command_list->OMSetRenderTargets(1, &rtvHandler, FALSE, nullptr);
	const float clearColor[] = { 0.f, 0.f, 0.f, 1.f };
	command_list->ClearRenderTargetView(rtvHandler, clearColor, 0, nullptr);
	command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	command_list->IASetVertexBuffers(0, 1, &vertex_buffer_view);
	command_list->DrawInstanced(verteces.size(), 1, 0, 0);

	// Resource barrier from RT to present
	command_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
		render_targets[frame_index].Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PRESENT
	));

	// Close command list
	ThrowIfFailed(command_list->Close());
}

void Renderer::WaitForPreviousFrame()
{
	// WAITING FOR THE FRAME TO COMPLETE BEFORE CONTINUING IS NOT BEST PRACTICE.
	// Signal and increment the fence value.
	const UINT64 prevFenceValue = fence_value;
	ThrowIfFailed(command_queue->Signal(fence.Get(), prevFenceValue));
	fence_value++;

	if (fence->GetCompletedValue() < prevFenceValue)
	{
		ThrowIfFailed(fence->SetEventOnCompletion(prevFenceValue, fence_event));
		WaitForSingleObject(fence_event, INFINITE);
	}

	frame_index = swap_chain->GetCurrentBackBufferIndex();
}

std::wstring Renderer::GetBinPath(std::wstring shader_file) const
{
	WCHAR buffer[MAX_PATH];
	GetModuleFileName(NULL, buffer, MAX_PATH);
	std::wstring modulePath = buffer;
	std::wstring::size_type pos = modulePath.find_last_of(L"\\/");
	return modulePath.substr(0, pos + 1) + shader_file;
}
