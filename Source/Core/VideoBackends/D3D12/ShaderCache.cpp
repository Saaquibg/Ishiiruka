// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.
#include <unordered_map>

#include "Common/LinearDiskCache.h"

#include "Core/ConfigManager.h"

#include "VideoBackends/D3D12/D3DCommandListManager.h"
#include "VideoBackends/D3D12/D3DShader.h"
#include "VideoBackends/D3D12/ShaderCache.h"

#include "VideoCommon/Debugger.h"
#include "VideoCommon/HLSLCompiler.h"
#include "VideoCommon/Statistics.h"

namespace DX12
{

// Primitive topology type is always triangle, unless the GS stage is used. This is consumed
// by the PSO created in Renderer::ApplyState.
static D3D12_PRIMITIVE_TOPOLOGY_TYPE s_current_primitive_topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
struct ByteCodeCacheEntry
{
	D3D12_SHADER_BYTECODE m_shader_bytecode;
	bool m_compiled;
	std::atomic_flag m_initialized;
	// Only used for shader debugging..
	std::string code;

	ByteCodeCacheEntry() : m_compiled(false), m_shader_bytecode({})
	{
		m_initialized.clear();
	}
	void Release() {
		SAFE_DELETE(m_shader_bytecode.pShaderBytecode);
	}
};

static ByteCodeCacheEntry s_pass_entry;

using GsBytecodeCache = std::unordered_map<GeometryShaderUid, ByteCodeCacheEntry, GeometryShaderUid::ShaderUidHasher>;
using PsBytecodeCache = std::unordered_map<PixelShaderUid, ByteCodeCacheEntry, PixelShaderUid::ShaderUidHasher>;
using VsBytecodeCache = std::unordered_map<VertexShaderUid, ByteCodeCacheEntry, VertexShaderUid::ShaderUidHasher>;
GsBytecodeCache gs_bytecode_cache;
PsBytecodeCache ps_bytecode_cache;
VsBytecodeCache vs_bytecode_cache;

static std::vector<D3DBlob*> s_shader_blob_list;

static LinearDiskCache<GeometryShaderUid, u8> s_gs_disk_cache;
static LinearDiskCache<PixelShaderUid, u8> s_ps_disk_cache;
static LinearDiskCache<VertexShaderUid, u8> s_vs_disk_cache;

static UidChecker<GeometryShaderUid, ShaderCode> s_geometry_uid_checker;
static UidChecker<PixelShaderUid, ShaderCode> s_pixel_uid_checker;
static UidChecker<VertexShaderUid, ShaderCode> s_vertex_uid_checker;

static ByteCodeCacheEntry* s_last_geometry_shader_bytecode;
static ByteCodeCacheEntry* s_last_pixel_shader_bytecode;
static ByteCodeCacheEntry* s_last_vertex_shader_bytecode;
static GeometryShaderUid s_last_geometry_shader_uid;
static PixelShaderUid s_last_pixel_shader_uid;
static VertexShaderUid s_last_vertex_shader_uid;
static GeometryShaderUid s_last_cpu_geometry_shader_uid;
static PixelShaderUid s_last_cpu_pixel_shader_uid;
static VertexShaderUid s_last_cpu_vertex_shader_uid;

static HLSLAsyncCompiler *s_compiler;
static Common::SpinLock<true> s_shaders_lock;

template<typename UidType, typename ShaderCacheType, ShaderCacheType* cache>
class ShaderCacheInserter final : public LinearDiskCacheReader<UidType, u8>
{
public:
	void Read(const UidType &key, const u8* value, u32 value_size)
	{
		D3DBlob* blob = new D3DBlob(value_size, value);
		ShaderCache::InsertByteCode<UidType, ShaderCacheType>(key, cache, blob);
	}
};

void ShaderCache::Init()
{
	s_compiler = &HLSLAsyncCompiler::getInstance();
	s_shaders_lock.unlock();
	s_pass_entry.m_compiled = true;
	s_pass_entry.m_initialized.test_and_set();
	// This class intentionally shares its shader cache files with DX11, as the shaders are (right now) identical.
	// Reduces unnecessary compilation when switching between APIs.

	s_last_geometry_shader_bytecode = nullptr;
	s_last_pixel_shader_bytecode = nullptr;
	s_last_vertex_shader_bytecode = nullptr;
	s_last_geometry_shader_uid = {};
	s_last_pixel_shader_uid = {};
	s_last_vertex_shader_uid = {};
	s_last_cpu_geometry_shader_uid = {};
	s_last_cpu_pixel_shader_uid = {};
	s_last_cpu_vertex_shader_uid = {};

	// Ensure shader cache directory exists..
	std::string shader_cache_path = File::GetUserPath(D_SHADERCACHE_IDX);

	if (!File::Exists(shader_cache_path))
		File::CreateDir(File::GetUserPath(D_SHADERCACHE_IDX));

	std::string title_unique_id = SConfig::GetInstance().m_strUniqueID.c_str();

	std::string gs_cache_filename = StringFromFormat("%sIDX11-%s-gs.cache", shader_cache_path.c_str(), title_unique_id.c_str());
	std::string ps_cache_filename = StringFromFormat("%sIDX11-%s-ps.cache", shader_cache_path.c_str(), title_unique_id.c_str());
	std::string vs_cache_filename = StringFromFormat("%sIDX11-%s-vs.cache", shader_cache_path.c_str(), title_unique_id.c_str());

	ShaderCacheInserter<GeometryShaderUid, GsBytecodeCache, &gs_bytecode_cache> gs_inserter;
	s_gs_disk_cache.OpenAndRead(gs_cache_filename, gs_inserter);

	ShaderCacheInserter<PixelShaderUid, PsBytecodeCache, &ps_bytecode_cache> ps_inserter;
	s_ps_disk_cache.OpenAndRead(ps_cache_filename, ps_inserter);

	ShaderCacheInserter<VertexShaderUid, VsBytecodeCache, &vs_bytecode_cache> vs_inserter;
	s_vs_disk_cache.OpenAndRead(vs_cache_filename, vs_inserter);

	// Clear out disk cache when debugging shaders to ensure stale ones don't stick around..
	if (g_Config.bEnableShaderDebugging)
		Clear();
	SETSTAT(stats.numGeometryShadersAlive, (int)gs_bytecode_cache.size());
	SETSTAT(stats.numGeometryShadersCreated, 0);
	SETSTAT(stats.numPixelShadersAlive, (int)ps_bytecode_cache.size());
	SETSTAT(stats.numPixelShadersCreated, 0);
	SETSTAT(stats.numVertexShadersAlive, (int)vs_bytecode_cache.size());
	SETSTAT(stats.numVertexShadersCreated, 0);
}

void ShaderCache::Clear()
{

}

void ShaderCache::Shutdown()
{
	if (s_compiler)
	{
		s_compiler->WaitForFinish();
	}

	for (auto& iter : s_shader_blob_list)
		SAFE_RELEASE(iter);

	s_shader_blob_list.clear();

	gs_bytecode_cache.clear();
	ps_bytecode_cache.clear();
	vs_bytecode_cache.clear();

	s_gs_disk_cache.Sync();
	s_gs_disk_cache.Close();
	s_ps_disk_cache.Sync();
	s_ps_disk_cache.Close();
	s_vs_disk_cache.Sync();
	s_vs_disk_cache.Close();

	s_geometry_uid_checker.Invalidate();
	s_pixel_uid_checker.Invalidate();
	s_vertex_uid_checker.Invalidate();
}

static void PushByteCode(ByteCodeCacheEntry* entry, D3DBlob* shaderBuffer)
{
	s_shader_blob_list.push_back(shaderBuffer);
	entry->m_shader_bytecode.pShaderBytecode = shaderBuffer->Data();
	entry->m_shader_bytecode.BytecodeLength = shaderBuffer->Size();
	entry->m_compiled = true;
}

void ShaderCache::SetCurrentPrimitiveTopology(u32 gs_primitive_type)
{
	switch (gs_primitive_type)
	{
		case PRIMITIVE_TRIANGLES:
			s_current_primitive_topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			break;
		case PRIMITIVE_LINES:
			s_current_primitive_topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
			break;
		case PRIMITIVE_POINTS:
			s_current_primitive_topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
			break;
		default:
			CHECK(0, "Invalid primitive type.");
			break;
}
}

void ShaderCache::HandleGSUIDChange(
	const GeometryShaderUid &gs_uid,
	u32 gs_primitive_type,
	u32 components,
	const XFMemory &xfr,
	bool on_gpu_thread)
{
	if (gs_uid.GetUidData().IsPassthrough())
	{
		s_last_geometry_shader_bytecode = &s_pass_entry;
		return;
	}
	
	s_shaders_lock.lock();
	ByteCodeCacheEntry* entry = &gs_bytecode_cache[gs_uid];
	s_shaders_lock.unlock();
	if (on_gpu_thread)
	{
		s_last_geometry_shader_bytecode = entry;
	}
	
	if (entry->m_initialized.test_and_set())
	{
		return;
	}

	// Need to compile a new shader
	ShaderCode code;
	ShaderCompilerWorkUnit *wunit = s_compiler->NewUnit(GEOMETRYSHADERGEN_BUFFERSIZE);
	code.SetBuffer(wunit->code.data());
	GenerateGeometryShaderCode(code, gs_primitive_type, API_D3D11, xfr, components);
	wunit->codesize = (u32)code.BufferSize();
	wunit->entrypoint = "main";
	wunit->flags = D3DCOMPILE_SKIP_VALIDATION | D3DCOMPILE_OPTIMIZATION_LEVEL3;
	wunit->target = D3D::GeometryShaderVersionString();

	wunit->ResultHandler = [gs_uid, entry](ShaderCompilerWorkUnit* wunit)
	{
		if (SUCCEEDED(wunit->cresult))
		{
			D3DBlob* shaderBuffer = new D3DBlob(wunit->shaderbytecode);
			s_gs_disk_cache.Append(gs_uid, shaderBuffer->Data(), shaderBuffer->Size());
			PushByteCode(entry, shaderBuffer);
			wunit->shaderbytecode->Release();
			wunit->shaderbytecode = nullptr;
			SETSTAT(stats.numGeometryShadersAlive, (int)ps_bytecode_cache.size());
			INCSTAT(stats.numGeometryShadersCreated);
#if defined(_DEBUG) || defined(DEBUGFAST)
			if (g_ActiveConfig.bEnableShaderDebugging)
			{
				entry->code = wunit->code.data();
			}
#endif
		}
		else
		{
			static int num_failures = 0;
			char szTemp[MAX_PATH];
			sprintf(szTemp, "%sbad_gs_%04i.txt", File::GetUserPath(D_DUMP_IDX).c_str(), num_failures++);
			std::ofstream file;
			OpenFStream(file, szTemp, std::ios_base::out);
			file << ((const char *)wunit->code.data());
			file << ((const char *)wunit->error->GetBufferPointer());
			file.close();

			PanicAlert("Failed to compile geometry shader!\nThis usually happens when trying to use Dolphin with an outdated GPU or integrated GPU like the Intel GMA series.\n\nIf you're sure this is Dolphin's error anyway, post the contents of %s along with this error message at the forums.\n\nDebug info (%s):\n%s",
				szTemp,
				D3D::GeometryShaderVersionString(),
				(char*)wunit->error->GetBufferPointer());
		}
	};
	s_compiler->CompileShaderAsync(wunit);
}
void ShaderCache::HandlePSUIDChange(
	const PixelShaderUid &ps_uid,
	DSTALPHA_MODE ps_dst_alpha_mode,
	u32 components,
	const XFMemory &xfr,
	const BPMemory &bpm, 
	bool on_gpu_thread)
{
	s_shaders_lock.lock();
	ByteCodeCacheEntry* entry = &ps_bytecode_cache[ps_uid];
	s_shaders_lock.unlock();
	if (on_gpu_thread)
	{
		s_last_pixel_shader_bytecode = entry;
	}
	if (entry->m_initialized.test_and_set())
	{
		return;
	}
	// Need to compile a new shader
	ShaderCode code;
	ShaderCompilerWorkUnit *wunit = s_compiler->NewUnit(PIXELSHADERGEN_BUFFERSIZE);
	code.SetBuffer(wunit->code.data());
	GeneratePixelShaderCodeD3D11(code, ps_dst_alpha_mode, components, xfr, bpm);
	wunit->codesize = (u32)code.BufferSize();
	wunit->entrypoint = "main";
	wunit->flags = D3DCOMPILE_SKIP_VALIDATION | D3DCOMPILE_OPTIMIZATION_LEVEL3;
	wunit->target = D3D::PixelShaderVersionString();
	wunit->ResultHandler = [ps_uid, entry](ShaderCompilerWorkUnit* wunit)
	{
		if (SUCCEEDED(wunit->cresult))
		{
			D3DBlob* shaderBuffer = new D3DBlob(wunit->shaderbytecode);
			s_ps_disk_cache.Append(ps_uid, shaderBuffer->Data(), shaderBuffer->Size());
			PushByteCode(entry, shaderBuffer);
			wunit->shaderbytecode->Release();
			wunit->shaderbytecode = nullptr;
#if defined(_DEBUG) || defined(DEBUGFAST)
			if (g_ActiveConfig.bEnableShaderDebugging)
			{
				entry->code = wunit->code.data();
			}
#endif
		}
		else
		{
			static int num_failures = 0;
			std::string filename = StringFromFormat("%sbad_ps_%04i.txt", File::GetUserPath(D_DUMP_IDX).c_str(), num_failures++);
			std::ofstream file;
			OpenFStream(file, filename, std::ios_base::out);
			file << ((const char *)wunit->code.data());
			file << ((const char *)wunit->error->GetBufferPointer());
			file.close();

			PanicAlert("Failed to compile pixel shader!\nThis usually happens when trying to use Dolphin with an outdated GPU or integrated GPU like the Intel GMA series.\n\nIf you're sure this is Dolphin's error anyway, post the contents of %s along with this error message at the forums.\n\nDebug info (%s):\n%s",
				filename,
				D3D::PixelShaderVersionString(),
				(char*)wunit->error->GetBufferPointer());
		}
	};
	s_compiler->CompileShaderAsync(wunit);
}

void ShaderCache::HandleVSUIDChange(
	const VertexShaderUid& vs_uid,
	u32 components,
	const XFMemory &xfr,
	const BPMemory &bpm,
	bool on_gpu_thread)
{
	s_shaders_lock.lock();
	ByteCodeCacheEntry* entry = &vs_bytecode_cache[vs_uid];
	s_shaders_lock.unlock();
	if (on_gpu_thread)
	{
		s_last_vertex_shader_bytecode = entry;
	}
	// Compile only when we have a new instance
	if (entry->m_initialized.test_and_set())
	{
		return;
	}
	ShaderCode code;
	ShaderCompilerWorkUnit *wunit = s_compiler->NewUnit(VERTEXSHADERGEN_BUFFERSIZE);
	code.SetBuffer(wunit->code.data());
	GenerateVertexShaderCodeD3D11(code, components, xfr, bpm);
	wunit->codesize = (u32)code.BufferSize();
	wunit->entrypoint = "main";
	wunit->flags = D3DCOMPILE_SKIP_VALIDATION | D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;
	wunit->target = D3D::VertexShaderVersionString();
	wunit->ResultHandler = [vs_uid, entry](ShaderCompilerWorkUnit* wunit)
	{
		if (SUCCEEDED(wunit->cresult))
		{
			D3DBlob* shaderBuffer = new D3DBlob(wunit->shaderbytecode);
			s_vs_disk_cache.Append(vs_uid, shaderBuffer->Data(), shaderBuffer->Size());
			PushByteCode(entry, shaderBuffer);
			wunit->shaderbytecode->Release();
			wunit->shaderbytecode = nullptr;
#if defined(_DEBUG) || defined(DEBUGFAST)
			if (g_ActiveConfig.bEnableShaderDebugging)
			{
				entry->code = wunit->code.data();
			}
#endif
		}
		else
		{
			static int num_failures = 0;
			std::string filename = StringFromFormat("%sbad_vs_%04i.txt", File::GetUserPath(D_DUMP_IDX).c_str(), num_failures++);
			std::ofstream file;
			OpenFStream(file, filename, std::ios_base::out);
			file << ((const char*)wunit->code.data());
			file.close();

			PanicAlert("Failed to compile vertex shader!\nThis usually happens when trying to use Dolphin with an outdated GPU or integrated GPU like the Intel GMA series.\n\nIf you're sure this is Dolphin's error anyway, post the contents of %s along with this error message at the forums.\n\nDebug info (%s):\n%s",
				filename,
				D3D::VertexShaderVersionString(),
				(char*)wunit->error->GetBufferPointer());
		}
	};
	s_compiler->CompileShaderAsync(wunit);
}

void ShaderCache::PrepareShaders(DSTALPHA_MODE ps_dst_alpha_mode,
	u32 gs_primitive_type,
	u32 components,
	const XFMemory &xfr,
	const BPMemory &bpm, bool on_gpu_thread)
{
	SetCurrentPrimitiveTopology(gs_primitive_type);
	GeometryShaderUid gs_uid;
	GetGeometryShaderUid(gs_uid, gs_primitive_type, API_D3D11, xfr, components);
	PixelShaderUid ps_uid;
	GetPixelShaderUidD3D11(ps_uid, ps_dst_alpha_mode, components, xfr, bpm);
	VertexShaderUid vs_uid;
	GetVertexShaderUidD3D11(vs_uid, components, xfr, bpm);
	bool gs_changed = false;
	bool ps_changed = false;
	bool vs_changed = false;
	if (on_gpu_thread)
	{
		s_compiler->ProcCompilationResults();
		gs_changed = gs_uid != s_last_geometry_shader_uid;
		ps_changed = ps_uid != s_last_pixel_shader_uid;
		vs_changed = vs_uid != s_last_vertex_shader_uid;
	}
	else
	{
		gs_changed = gs_uid != s_last_cpu_geometry_shader_uid;
		ps_changed = ps_uid != s_last_cpu_pixel_shader_uid;
		vs_changed = vs_uid != s_last_cpu_vertex_shader_uid;
	}

	if (!gs_changed && !ps_changed && !vs_changed)
	{
		return;
	}

	if (on_gpu_thread)
	{
		if (gs_changed)
		{
			s_last_geometry_shader_uid = gs_uid;
		}

		if (ps_changed)
		{
			s_last_pixel_shader_uid = ps_uid;
		}

		if (vs_changed)
		{
			s_last_vertex_shader_uid = vs_uid;
		}
		// A Uid has changed, so the PSO will need to be reset at next ApplyState.
		D3D::command_list_mgr->m_dirty_pso = true;
#if defined(_DEBUG) || defined(DEBUGFAST)
		if (g_ActiveConfig.bEnableShaderDebugging)
		{
			ShaderCode code;
			if (gs_changed)
			{
				GenerateGeometryShaderCode(code, gs_primitive_type, API_D3D11, xfr, components);
				s_geometry_uid_checker.AddToIndexAndCheck(code, gs_uid, "Geometry", "g");
			}
			if (ps_changed)
			{
				ShaderCode code;
				GeneratePixelShaderCodeD3D11(code, ps_dst_alpha_mode, components, xfr, bpm);
				s_pixel_uid_checker.AddToIndexAndCheck(code, ps_uid, "Pixel", "p");
			}
			if (vs_changed)
			{
				GenerateVertexShaderCodeD3D11(code, components, xfr, bpm);
				s_vertex_uid_checker.AddToIndexAndCheck(code, vs_uid, "Vertex", "v");
			}
		}
#endif
	}
	else
	{
		if (gs_changed)
		{
			s_last_cpu_geometry_shader_uid = gs_uid;
		}

		if (ps_changed)
		{
			s_last_cpu_pixel_shader_uid = ps_uid;
		}

		if (vs_changed)
		{
			s_last_cpu_vertex_shader_uid = vs_uid;
		}
	}

	if (gs_changed)
	{
		HandleGSUIDChange(gs_uid, gs_primitive_type, components, xfr, on_gpu_thread);
	}

	if (ps_changed)
	{
		HandlePSUIDChange(ps_uid, ps_dst_alpha_mode, components, xfr, bpm, on_gpu_thread);
	}

	if (vs_changed)
	{
		HandleVSUIDChange(vs_uid, components, xfr, bpm, on_gpu_thread);
	}
}

bool ShaderCache::TestShaders()
{
	if (s_last_geometry_shader_bytecode == nullptr
		|| s_last_pixel_shader_bytecode == nullptr
		|| s_last_vertex_shader_bytecode == nullptr)
	{
		return false;
	}
	int count = 0;
	while (!(s_last_geometry_shader_bytecode->m_compiled
		&& s_last_pixel_shader_bytecode->m_compiled
		&& s_last_vertex_shader_bytecode->m_compiled))
	{
		s_compiler->ProcCompilationResults();
		if (g_ActiveConfig.bFullAsyncShaderCompilation)
		{
			break;
		}
		Common::cYield(count++);
	}
	return s_last_geometry_shader_bytecode->m_compiled
		&& s_last_pixel_shader_bytecode->m_compiled
		&& s_last_vertex_shader_bytecode->m_compiled;
}

template<typename UidType, typename ShaderCacheType>
void ShaderCache::InsertByteCode(const UidType& uid, ShaderCacheType* shader_cache, D3DBlob* bytecode_blob)
{
	s_shader_blob_list.push_back(bytecode_blob);
	s_shaders_lock.lock();
	ByteCodeCacheEntry* entry = &(*shader_cache)[uid];
	s_shaders_lock.unlock();
	entry->m_shader_bytecode.pShaderBytecode = bytecode_blob->Data();
	entry->m_shader_bytecode.BytecodeLength = bytecode_blob->Size();
	entry->m_compiled = true;
	entry->m_initialized.test_and_set();
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE ShaderCache::GetCurrentPrimitiveTopology() { return s_current_primitive_topology;}

D3D12_SHADER_BYTECODE ShaderCache::GetActiveGeometryShaderBytecode() { return s_last_geometry_shader_bytecode->m_shader_bytecode; }
D3D12_SHADER_BYTECODE ShaderCache::GetActivePixelShaderBytecode() { return s_last_pixel_shader_bytecode->m_shader_bytecode; }
D3D12_SHADER_BYTECODE ShaderCache::GetActiveVertexShaderBytecode() { return s_last_vertex_shader_bytecode->m_shader_bytecode; }

const GeometryShaderUid* ShaderCache::GetActiveGeometryShaderUid() { return &s_last_geometry_shader_uid; }
const PixelShaderUid* ShaderCache::GetActivePixelShaderUid() { return &s_last_pixel_shader_uid; }
const VertexShaderUid* ShaderCache::GetActiveVertexShaderUid() { return &s_last_vertex_shader_uid; }

D3D12_SHADER_BYTECODE ShaderCache::GetGeometryShaderFromUid(const GeometryShaderUid* uid) { return gs_bytecode_cache[*uid].m_shader_bytecode; }
D3D12_SHADER_BYTECODE ShaderCache::GetPixelShaderFromUid(const PixelShaderUid* uid) { return ps_bytecode_cache[*uid].m_shader_bytecode; }
D3D12_SHADER_BYTECODE ShaderCache::GetVertexShaderFromUid(const VertexShaderUid* uid) { return vs_bytecode_cache[*uid].m_shader_bytecode; }

}