// Copyright 1998-2017 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	SceneRenderTargets.cpp: Scene render target implementation.
=============================================================================*/

#include "PostProcess/SceneRenderTargets.h"
#include "Shader.h"
#include "StaticBoundShaderState.h"
#include "SceneUtils.h"
#include "SceneRenderTargetParameters.h"
#include "VelocityRendering.h"
#include "RendererModule.h"
#include "LightPropagationVolume.h"
#include "ScenePrivate.h"
#include "ClearQuad.h"
#include "RenderUtils.h"
#include "PipelineStateCache.h"

IMPLEMENT_UNIFORM_BUFFER_STRUCT(FGBufferResourceStruct,TEXT("GBuffers"));

static TAutoConsoleVariable<int32> CVarRSMResolution(
	TEXT("r.LPV.RSMResolution"),
	360,
	TEXT("Reflective Shadow Map resolution (used for LPV) - higher values result in less aliasing artifacts, at the cost of performance"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

/*-----------------------------------------------------------------------------
FSceneRenderTargets
-----------------------------------------------------------------------------*/

int32 GDownsampledOcclusionQueries = 0;
static FAutoConsoleVariableRef CVarDownsampledOcclusionQueries(
	TEXT("r.DownsampledOcclusionQueries"),
	GDownsampledOcclusionQueries,
	TEXT("Whether to issue occlusion queries to a downsampled depth buffer"),
	ECVF_RenderThreadSafe
	);

static TAutoConsoleVariable<int32> CVarSceneTargetsResizingMethod(
	TEXT("r.SceneRenderTargetResizeMethod"),
	0,
	TEXT("Control the scene render target resize method:\n")
	TEXT("(This value is only used in game mode and on windowing platforms.)\n")
	TEXT("0: Resize to match requested render size (Default) (Least memory use, can cause stalls when size changes e.g. ScreenPercentage)\n")
	TEXT("1: Fixed to screen resolution.\n")
	TEXT("2: Expands to encompass the largest requested render dimension. (Most memory use, least prone to allocation stalls.)"),	
	ECVF_RenderThreadSafe
	);

static TAutoConsoleVariable<int32> CVarCustomDepth(
	TEXT("r.CustomDepth"),
	1,
	TEXT("0: feature is disabled\n")
	TEXT("1: feature is enabled, texture is created on demand\n")
	TEXT("2: feature is enabled, texture is not released until required (should be the project setting if the feature should not stall)\n")
	TEXT("3: feature is enabled, stencil writes are enabled, texture is not released until required (should be the project setting if the feature should not stall)"),
	ECVF_RenderThreadSafe
	);

static TAutoConsoleVariable<int32> CVarMSAACount(
	TEXT("r.MSAACount"),
	4,
	TEXT("Number of MSAA samples to use with the forward renderer.  Only used when MSAA is enabled in the rendering project settings.\n")
	TEXT("0: MSAA disabled (Temporal AA enabled)\n")
	TEXT("1: MSAA disabled\n")
	TEXT("2: Use 2x MSAA\n")
	TEXT("4: Use 4x MSAA"),
	ECVF_RenderThreadSafe | ECVF_Scalability
	);

static TAutoConsoleVariable<int32> CVarMobileMSAA(
	TEXT("r.MobileMSAA"),
	1,
	TEXT("Use MSAA instead of Temporal AA on mobile:\n")
	TEXT("1: Use Temporal AA (MSAA disabled)\n")
	TEXT("2: Use 2x MSAA (Temporal AA disabled)\n")
	TEXT("4: Use 4x MSAA (Temporal AA disabled)\n")
	TEXT("8: Use 8x MSAA (Temporal AA disabled)"),
	ECVF_RenderThreadSafe | ECVF_ReadOnly
	);

static TAutoConsoleVariable<int32> CVarGBufferFormat(
	TEXT("r.GBufferFormat"),
	1,
	TEXT("Defines the memory layout used for the GBuffer.\n")
	TEXT("(affects performance, mostly through bandwidth, quality of normals and material attributes).\n")
	TEXT(" 0: lower precision (8bit per component, for profiling)\n")
	TEXT(" 1: low precision (default)\n")
	TEXT(" 3: high precision normals encoding\n")
	TEXT(" 5: high precision"),
	ECVF_RenderThreadSafe);

// Fast VRam Settings. These should be tuned based on title requirements and resolution
// Note: Currently, changing these settings at runtime does not cause buffers to be reallocated 
static TAutoConsoleVariable<int32> CVarFastVRamGBufferCount(
	TEXT("r.FastVRamGBufferCount"),
	4,
	TEXT("Number of fast VRAM GBuffers, in order A-E. May not be optimal on platforms with limited fast VRAM. Adjust based on resolution"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarFastVRamSceneColor(
	TEXT("r.FastVRamSceneColor"),
	1,
	TEXT("Whether to store scene color in fast VRAM"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarFastVRamSceneDepth(
	TEXT("r.FastVRamSceneDepth"),
	1,
	TEXT("Whether to store scene depth in fast VRAM"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarFastVRamLightAttenuation(
	TEXT("r.FastVRamLightAttenuation"),
	1,
	TEXT("Whether to store light attenuation in fast VRAM"),
	ECVF_Scalability | ECVF_RenderThreadSafe);


/** The global render targets used for scene rendering. */
static TGlobalResource<FSceneRenderTargets> SceneRenderTargetsSingleton;

FSceneRenderTargets& FSceneRenderTargets::Get(FRHICommandList& RHICmdList)
{
	FSceneRenderTargets* SceneContext = (FSceneRenderTargets*)RHICmdList.GetRenderThreadContext(FRHICommandListBase::ERenderThreadContext::SceneRenderTargets);
	if (!SceneContext)
	{
		return SceneRenderTargetsSingleton;
	}
	check(!RHICmdList.IsImmediate());
	return *SceneContext;
}

FSceneRenderTargets& FSceneRenderTargets::Get(FRHICommandListImmediate& RHICmdList)
{
	check(IsInRenderingThread() && !RHICmdList.GetRenderThreadContext(FRHICommandListBase::ERenderThreadContext::SceneRenderTargets)
		&& !FTaskGraphInterface::Get().IsThreadProcessingTasks(ENamedThreads::RenderThread_Local)); // if we are processing tasks on the local queue, it is assumed this are in support of async tasks, which cannot use the current state of the render targets. This can be relaxed if needed.
	return SceneRenderTargetsSingleton;
}

FSceneRenderTargets& FSceneRenderTargets::Get(FRHIAsyncComputeCommandListImmediate& RHICmdList)
{
	check(IsInRenderingThread() && !RHICmdList.GetRenderThreadContext(FRHICommandListBase::ERenderThreadContext::SceneRenderTargets)
		&& !FTaskGraphInterface::Get().IsThreadProcessingTasks(ENamedThreads::RenderThread_Local)); // if we are processing tasks on the local queue, it is assumed this are in support of async tasks, which cannot use the current state of the render targets. This can be relaxed if needed.
	return SceneRenderTargetsSingleton;
}

FSceneRenderTargets& FSceneRenderTargets::GetGlobalUnsafe()
{
	check(IsInRenderingThread()
		&& !FTaskGraphInterface::Get().IsThreadProcessingTasks(ENamedThreads::RenderThread_Local)); // if we are processing tasks on the local queue, it is assumed this are in support of async tasks, which cannot use the current state of the render targets. This can be relaxed if needed.
	return SceneRenderTargetsSingleton;
}

FSceneRenderTargets& FSceneRenderTargets::Get_FrameConstantsOnly()
{
	return SceneRenderTargetsSingleton;
}

 FSceneRenderTargets* FSceneRenderTargets::CreateSnapshot(const FViewInfo& InView)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FSceneRenderTargets_CreateSnapshot);
	check(IsInRenderingThread() && FMemStack::Get().GetNumMarks() == 1); // we do not want this popped before the end of the scene and it better be the scene allocator
	FSceneRenderTargets* NewSnapshot = new (FMemStack::Get()) FSceneRenderTargets(InView, *this);
	check(NewSnapshot->bSnapshot);
	Snapshots.Add(NewSnapshot);
	return NewSnapshot;
}

void FSceneRenderTargets::SetSnapshotOnCmdList(FRHICommandList& TargetCmdList)
{
	check(bSnapshot);
	TargetCmdList.SetRenderThreadContext(this, FRHICommandListBase::ERenderThreadContext::SceneRenderTargets);
}

void FSceneRenderTargets::DestroyAllSnapshots()
{
	if (Snapshots.Num())
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_FSceneRenderTargets_DestroyAllSnapshots);
		check(IsInRenderingThread());
		for (auto Snapshot : Snapshots)
		{
			Snapshot->~FSceneRenderTargets();
		}
		Snapshots.Reset();
		GRenderTargetPool.DestructSnapshots();
	}
}



template <size_t N>
static void SnapshotArray(TRefCountPtr<IPooledRenderTarget> (&Dest)[N], const TRefCountPtr<IPooledRenderTarget> (&Src)[N])
{
	for (int32 Index = 0; Index < N; Index++)
	{
		Dest[Index] = GRenderTargetPool.MakeSnapshot(Src[Index]);
	}
}

FSceneRenderTargets::FSceneRenderTargets(const FViewInfo& View, const FSceneRenderTargets& SnapshotSource)
	: LightAttenuation(GRenderTargetPool.MakeSnapshot(SnapshotSource.LightAttenuation))
	, LightAccumulation(GRenderTargetPool.MakeSnapshot(SnapshotSource.LightAccumulation))
	, DirectionalOcclusion(GRenderTargetPool.MakeSnapshot(SnapshotSource.DirectionalOcclusion))
	, SceneDepthZ(GRenderTargetPool.MakeSnapshot(SnapshotSource.SceneDepthZ))
	, LightingChannels(GRenderTargetPool.MakeSnapshot(SnapshotSource.LightingChannels))
	, SceneAlphaCopy(GRenderTargetPool.MakeSnapshot(SnapshotSource.SceneAlphaCopy))
	, AuxiliarySceneDepthZ(GRenderTargetPool.MakeSnapshot(SnapshotSource.AuxiliarySceneDepthZ))
	, SmallDepthZ(GRenderTargetPool.MakeSnapshot(SnapshotSource.SmallDepthZ))
	, GBufferA(GRenderTargetPool.MakeSnapshot(SnapshotSource.GBufferA))
	, GBufferB(GRenderTargetPool.MakeSnapshot(SnapshotSource.GBufferB))
	, GBufferC(GRenderTargetPool.MakeSnapshot(SnapshotSource.GBufferC))
	, GBufferD(GRenderTargetPool.MakeSnapshot(SnapshotSource.GBufferD))
	, GBufferE(GRenderTargetPool.MakeSnapshot(SnapshotSource.GBufferE))
	, GBufferVelocity(GRenderTargetPool.MakeSnapshot(SnapshotSource.GBufferVelocity))
	, DBufferA(GRenderTargetPool.MakeSnapshot(SnapshotSource.DBufferA))
	, DBufferB(GRenderTargetPool.MakeSnapshot(SnapshotSource.DBufferB))
	, DBufferC(GRenderTargetPool.MakeSnapshot(SnapshotSource.DBufferC))
	, DBufferMask(GRenderTargetPool.MakeSnapshot(SnapshotSource.DBufferMask))
	, ScreenSpaceAO(GRenderTargetPool.MakeSnapshot(SnapshotSource.ScreenSpaceAO))
	, QuadOverdrawBuffer(GRenderTargetPool.MakeSnapshot(SnapshotSource.QuadOverdrawBuffer))
	, CustomDepth(GRenderTargetPool.MakeSnapshot(SnapshotSource.CustomDepth))
	, MobileCustomStencil(GRenderTargetPool.MakeSnapshot(SnapshotSource.MobileCustomStencil))
	, CustomStencilSRV(SnapshotSource.CustomStencilSRV)
	, SkySHIrradianceMap(GRenderTargetPool.MakeSnapshot(SnapshotSource.SkySHIrradianceMap))
	, MobileMultiViewSceneColor(GRenderTargetPool.MakeSnapshot(SnapshotSource.MobileMultiViewSceneColor))
	, MobileMultiViewSceneDepthZ(GRenderTargetPool.MakeSnapshot(SnapshotSource.MobileMultiViewSceneDepthZ))
	, EditorPrimitivesColor(GRenderTargetPool.MakeSnapshot(SnapshotSource.EditorPrimitivesColor))
	, EditorPrimitivesDepth(GRenderTargetPool.MakeSnapshot(SnapshotSource.EditorPrimitivesDepth))
	, SeparateTranslucencyRT(SnapshotSource.SeparateTranslucencyRT)
	, SeparateTranslucencyDepthRT(SnapshotSource.SeparateTranslucencyDepthRT)
	, bScreenSpaceAOIsValid(SnapshotSource.bScreenSpaceAOIsValid)
	, bCustomDepthIsValid(SnapshotSource.bCustomDepthIsValid)
	, GBufferRefCount(SnapshotSource.GBufferRefCount)
	, LargestDesiredSizeThisFrame(SnapshotSource.LargestDesiredSizeThisFrame)
	, LargestDesiredSizeLastFrame(SnapshotSource.LargestDesiredSizeLastFrame)
	, ThisFrameNumber(SnapshotSource.ThisFrameNumber)
	, bVelocityPass(SnapshotSource.bVelocityPass)
	, bSeparateTranslucencyPass(SnapshotSource.bSeparateTranslucencyPass)
	, GBufferResourcesUniformBuffer(SnapshotSource.GBufferResourcesUniformBuffer)
	, GBufferDummyResourcesUniformBuffer(SnapshotSource.GBufferDummyResourcesUniformBuffer)
	, BufferSize(SnapshotSource.BufferSize)
	, SeparateTranslucencyBufferSize(SnapshotSource.SeparateTranslucencyBufferSize)
	, SeparateTranslucencyScale(SnapshotSource.SeparateTranslucencyScale)
	, SmallColorDepthDownsampleFactor(SnapshotSource.SmallColorDepthDownsampleFactor)
	, bLightAttenuationEnabled(SnapshotSource.bLightAttenuationEnabled)
	, bUseDownsizedOcclusionQueries(SnapshotSource.bUseDownsizedOcclusionQueries)
	, CurrentGBufferFormat(SnapshotSource.CurrentGBufferFormat)
	, CurrentSceneColorFormat(SnapshotSource.CurrentSceneColorFormat)
	, bAllowStaticLighting(SnapshotSource.bAllowStaticLighting)
	, CurrentMaxShadowResolution(SnapshotSource.CurrentMaxShadowResolution)
	, CurrentRSMResolution(SnapshotSource.CurrentRSMResolution)
	, CurrentTranslucencyLightingVolumeDim(SnapshotSource.CurrentTranslucencyLightingVolumeDim)
	, CurrentMobile32bpp(SnapshotSource.CurrentMobile32bpp)
	, CurrentMSAACount(SnapshotSource.CurrentMSAACount)
	, CurrentMinShadowResolution(SnapshotSource.CurrentMinShadowResolution)
	, bCurrentLightPropagationVolume(SnapshotSource.bCurrentLightPropagationVolume)
	, CurrentFeatureLevel(SnapshotSource.CurrentFeatureLevel)
	, CurrentShadingPath(SnapshotSource.CurrentShadingPath)
	, bRequireSceneColorAlpha(SnapshotSource.bRequireSceneColorAlpha)
	, bAllocateVelocityGBuffer(SnapshotSource.bAllocateVelocityGBuffer)
	, bGBuffersFastCleared(SnapshotSource.bGBuffersFastCleared)	
	, bSceneDepthCleared(SnapshotSource.bSceneDepthCleared)	
	, bSnapshot(true)
	, DefaultColorClear(SnapshotSource.DefaultColorClear)
	, DefaultDepthClear(SnapshotSource.DefaultDepthClear)
	, QuadOverdrawIndex(SnapshotSource.QuadOverdrawIndex)
{
	SnapshotArray(SceneColor, SnapshotSource.SceneColor);
	SnapshotArray(ReflectionColorScratchCubemap, SnapshotSource.ReflectionColorScratchCubemap);
	SnapshotArray(DiffuseIrradianceScratchCubemap, SnapshotSource.DiffuseIrradianceScratchCubemap);
	SnapshotArray(TranslucencyLightingVolumeAmbient, SnapshotSource.TranslucencyLightingVolumeAmbient);
	SnapshotArray(TranslucencyLightingVolumeDirectional, SnapshotSource.TranslucencyLightingVolumeDirectional);
	SnapshotArray(OptionalShadowDepthColor, SnapshotSource.OptionalShadowDepthColor);
}

inline const TCHAR* GetSceneColorTargetName(EShadingPath ShadingPath)
{
	const TCHAR* SceneColorNames[(uint32)EShadingPath::Num] =
	{ 
		TEXT("SceneColorMobile"), 
		TEXT("SceneColorDeferred")
	};
	check((uint32)ShadingPath < ARRAY_COUNT(SceneColorNames));
	return SceneColorNames[(uint32)ShadingPath];
}

FIntPoint FSceneRenderTargets::ComputeDesiredSize(const FSceneViewFamily& ViewFamily)
{
	enum ESizingMethods { RequestedSize, ScreenRes, Grow, VisibleSizingMethodsCount };
	ESizingMethods SceneTargetsSizingMethod = Grow;

	bool bIsSceneCapture = false;
	bool bIsReflectionCapture = false;

	for (int32 ViewIndex = 0, ViewCount = ViewFamily.Views.Num(); ViewIndex < ViewCount; ++ViewIndex)
	{
		const FSceneView* View = ViewFamily.Views[ViewIndex];

		bIsSceneCapture |= View->bIsSceneCapture;
		bIsReflectionCapture |= View->bIsReflectionCapture;
	}

	if(!FPlatformProperties::SupportsWindowedMode())
	{
		// Force ScreenRes on non windowed platforms.
		SceneTargetsSizingMethod = RequestedSize;
	}
	else if (GIsEditor)
	{
		// Always grow scene render targets in the editor.
		SceneTargetsSizingMethod = Grow;
	}	
	else
	{
		// Otherwise use the setting specified by the console variable.
		SceneTargetsSizingMethod = (ESizingMethods) FMath::Clamp(CVarSceneTargetsResizingMethod.GetValueOnRenderThread(), 0, (int32)VisibleSizingMethodsCount);
	}

	FIntPoint DesiredBufferSize = FIntPoint::ZeroValue;

	switch (SceneTargetsSizingMethod)
	{
		case RequestedSize:
			DesiredBufferSize = FIntPoint(ViewFamily.FamilySizeX, ViewFamily.FamilySizeY);
			break;

		case ScreenRes:
			DesiredBufferSize = FIntPoint(GSystemResolution.ResX, GSystemResolution.ResY);
			break;

		case Grow:
			DesiredBufferSize = FIntPoint(FMath::Max((uint32)GetBufferSizeXY().X, ViewFamily.FamilySizeX),
					FMath::Max((uint32)GetBufferSizeXY().Y, ViewFamily.FamilySizeY));
			break;

		default:
			checkNoEntry();
	}


	// we want to shrink the buffer but as we can have multiple scenecaptures per frame we have to delay that a frame to get all size requests
	{
		// this allows The BufferSize to not grow below the SceneCapture requests (happen before scene rendering, in the same frame with a Grow request)
		LargestDesiredSizeThisFrame = LargestDesiredSizeThisFrame.ComponentMax(DesiredBufferSize);

		uint32 FrameNumber = ViewFamily.FrameNumber;

		// this could be refined to be some time or multiple frame if we have SceneCaptures not running each frame any more
		if(ThisFrameNumber != FrameNumber)
		{
			// this allows the BufferSize to shrink each frame (in game)
			ThisFrameNumber = FrameNumber;
			LargestDesiredSizeLastFrame = LargestDesiredSizeThisFrame;
			LargestDesiredSizeThisFrame = FIntPoint(0, 0);
		}

		DesiredBufferSize = DesiredBufferSize.ComponentMax(LargestDesiredSizeLastFrame);
	}

	return DesiredBufferSize;
}

uint16 FSceneRenderTargets::GetNumSceneColorMSAASamples(ERHIFeatureLevel::Type InFeatureLevel)
{
	uint16 NumSamples = 1;

	if (InFeatureLevel >= ERHIFeatureLevel::SM4)
	{
		static IConsoleVariable* CVarDefaultAntiAliasing = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DefaultFeature.AntiAliasing"));
		EAntiAliasingMethod Method = (EAntiAliasingMethod)CVarDefaultAntiAliasing->GetInt();

		if (IsForwardShadingEnabled(InFeatureLevel) && Method == AAM_MSAA)
		{
			NumSamples = FMath::Max(1, CVarMSAACount.GetValueOnRenderThread());

			if (NumSamples != 1 && NumSamples != 2 && NumSamples != 4)
			{
				UE_LOG(LogRenderer, Warning, TEXT("Requested %d samples for MSAA, but this is not supported; falling back to 1 sample"), NumSamples);
				NumSamples = 1;
			}
		}
	}
	else
	{
		NumSamples = CVarMobileMSAA.GetValueOnRenderThread();
		if (NumSamples != 1 && NumSamples != 2 && NumSamples != 4 && NumSamples != 8)
		{
			UE_LOG(LogRenderer, Warning, TEXT("Requested %d samples for MSAA, but this is not supported; falling back to 1 sample"), NumSamples);
			NumSamples = 1;
		}
	}

	if (NumSamples > 1 && !RHISupportsMSAA(GShaderPlatformForFeatureLevel[InFeatureLevel]))
	{
		NumSamples = 1;

		static bool bWarned = false;

		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogRenderer, Log, TEXT("MSAA requested but the platform doesn't support MSAA, falling back to Temporal AA"));
		}
	}

	return NumSamples;
}

void FSceneRenderTargets::Allocate(FRHICommandList& RHICmdList, const FSceneViewFamily& ViewFamily)
{
	check(IsInRenderingThread());
	// ViewFamily setup wasn't complete
	check(ViewFamily.FrameNumber != UINT_MAX);

	// If feature level has changed, release all previously allocated targets to the pool. If feature level has changed but
	const auto NewFeatureLevel = ViewFamily.Scene->GetFeatureLevel();
	CurrentShadingPath = ViewFamily.Scene->GetShadingPath();

	bRequireSceneColorAlpha = false;

	for (int32 ViewIndex = 0; ViewIndex < ViewFamily.Views.Num(); ViewIndex++)
	{
		// Planar reflections and scene captures use scene color alpha to keep track of where content has been rendered, for compositing into a different scene later
		if (ViewFamily.Views[ViewIndex]->bIsPlanarReflection || ViewFamily.Views[ViewIndex]->bIsSceneCapture)
		{
			bRequireSceneColorAlpha = true;
		}
	}

	FIntPoint DesiredBufferSize = ComputeDesiredSize(ViewFamily);
	check(DesiredBufferSize.X > 0 && DesiredBufferSize.Y > 0);
	QuantizeSceneBufferSize(DesiredBufferSize.X, DesiredBufferSize.Y);

	int GBufferFormat = CVarGBufferFormat.GetValueOnRenderThread();

	// Set default clear values
	const bool bUseMonoClearValue = ViewFamily.IsMonoscopicFarFieldEnabled() && ViewFamily.MonoParameters.Mode != EMonoscopicFarFieldMode::StereoNoClipping;
	SetDefaultColorClear(bUseMonoClearValue ? FClearValueBinding() : FClearValueBinding::Black);
	SetDefaultDepthClear(bUseMonoClearValue ? FClearValueBinding(ViewFamily.MonoParameters.StereoDepthClip, 0) : FClearValueBinding::DepthFar);

	int SceneColorFormat;
	{
		static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.SceneColorFormat"));

		SceneColorFormat = CVar->GetValueOnRenderThread();
	}
		
	bool bNewAllowStaticLighting;
	{
		static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.AllowStaticLighting"));

		bNewAllowStaticLighting = CVar->GetValueOnRenderThread() != 0;
	}

	bool bDownsampledOcclusionQueries = GDownsampledOcclusionQueries != 0;

	int32 MaxShadowResolution = GetCachedScalabilityCVars().MaxShadowResolution;

	int32 RSMResolution = FMath::Clamp(CVarRSMResolution.GetValueOnRenderThread(), 1, 2048);

	if (ViewFamily.Scene->GetShadingPath() == EShadingPath::Mobile)
	{
		// ensure there is always enough space for mobile renderer's tiled shadow maps
		// by reducing the shadow map resolution.
		int32 MaxShadowDepthBufferDim = FMath::Max(GMaxShadowDepthBufferSizeX, GMaxShadowDepthBufferSizeY);
		if (MaxShadowResolution * 2 >  MaxShadowDepthBufferDim)
		{
			MaxShadowResolution = MaxShadowDepthBufferDim / 2;
		}
	}

	int32 TranslucencyLightingVolumeDim = GTranslucencyLightingVolumeDim;

	uint32 Mobile32bpp = !IsMobileHDR() || IsMobileHDR32bpp();

	int32 MSAACount = GetNumSceneColorMSAASamples(NewFeatureLevel);

	bool bLightPropagationVolume = UseLightPropagationVolumeRT(NewFeatureLevel);

	uint32 MinShadowResolution;
	{
		static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Shadow.MinResolution"));

		MinShadowResolution = CVar->GetValueOnRenderThread();
	}

	if( (BufferSize.X != DesiredBufferSize.X) ||
		(BufferSize.Y != DesiredBufferSize.Y) ||
		(CurrentGBufferFormat != GBufferFormat) ||
		(CurrentSceneColorFormat != SceneColorFormat) ||
		(bAllowStaticLighting != bNewAllowStaticLighting) ||
		(bUseDownsizedOcclusionQueries != bDownsampledOcclusionQueries) ||
		(CurrentMaxShadowResolution != MaxShadowResolution) ||
 		(CurrentRSMResolution != RSMResolution) ||
		(CurrentTranslucencyLightingVolumeDim != TranslucencyLightingVolumeDim) ||
		(CurrentMobile32bpp != Mobile32bpp) ||
		(CurrentMSAACount != MSAACount) ||
		(bCurrentLightPropagationVolume != bLightPropagationVolume) ||
		(CurrentMinShadowResolution != MinShadowResolution))
	{
		CurrentGBufferFormat = GBufferFormat;
		CurrentSceneColorFormat = SceneColorFormat;
		bAllowStaticLighting = bNewAllowStaticLighting;
		bUseDownsizedOcclusionQueries = bDownsampledOcclusionQueries;
		CurrentMaxShadowResolution = MaxShadowResolution;
		CurrentRSMResolution = RSMResolution;
		CurrentTranslucencyLightingVolumeDim = TranslucencyLightingVolumeDim;
		CurrentMobile32bpp = Mobile32bpp;
		CurrentMSAACount = MSAACount;
		CurrentMinShadowResolution = MinShadowResolution;
		bCurrentLightPropagationVolume = bLightPropagationVolume;

		// Reinitialize the render targets for the given size.
		SetBufferSize(DesiredBufferSize.X, DesiredBufferSize.Y);

		UE_LOG(LogRenderer, Log, TEXT("Reallocating scene render targets to support %ux%u NumSamples %u (Frame:%u)."), BufferSize.X, BufferSize.Y, CurrentMSAACount, ViewFamily.FrameNumber);

		UpdateRHI();
	}

	// Do allocation of render targets if they aren't available for the current shading path
	CurrentFeatureLevel = NewFeatureLevel;
	AllocateRenderTargets(RHICmdList);
	if (ViewFamily.IsMonoscopicFarFieldEnabled())
	{
		AllocSceneMonoRenderTargets(RHICmdList, *ViewFamily.Views[2]);
	}
}

void FSceneRenderTargets::BeginRenderingSceneColor(FRHICommandList& RHICmdList, ESimpleRenderTargetMode RenderTargetMode/*=EUninitializedColorExistingDepth*/, FExclusiveDepthStencil DepthStencilAccess, bool bTransitionWritable)
{
	SCOPED_DRAW_EVENT(RHICmdList, BeginRenderingSceneColor);
	AllocSceneColor(RHICmdList);
	SetRenderTarget(RHICmdList, GetSceneColorSurface(), GetSceneDepthSurface(), RenderTargetMode, DepthStencilAccess, bTransitionWritable);
} 

void FSceneRenderTargets::BeginRenderingSceneMonoColor(FRHICommandList& RHICmdList, ESimpleRenderTargetMode RenderTargetMode, FExclusiveDepthStencil DepthStencilAccess)
{
	SCOPED_DRAW_EVENT(RHICmdList, BeginRenderingSceneMonoColor);

	SetRenderTarget(RHICmdList, GetSceneMonoColorSurface(), GetSceneMonoDepthSurface(), RenderTargetMode, DepthStencilAccess, true);
}

int32 FSceneRenderTargets::GetGBufferRenderTargets(ERenderTargetLoadAction ColorLoadAction, FRHIRenderTargetView OutRenderTargets[MaxSimultaneousRenderTargets], int32& OutVelocityRTIndex)
{
	int32 MRTCount = 0;
	OutRenderTargets[MRTCount++] = FRHIRenderTargetView(GetSceneColorSurface(), 0, -1, ColorLoadAction, ERenderTargetStoreAction::EStore);
	OutRenderTargets[MRTCount++] = FRHIRenderTargetView(GBufferA->GetRenderTargetItem().TargetableTexture, 0, -1, ColorLoadAction, ERenderTargetStoreAction::EStore);
	OutRenderTargets[MRTCount++] = FRHIRenderTargetView(GBufferB->GetRenderTargetItem().TargetableTexture, 0, -1, ColorLoadAction, ERenderTargetStoreAction::EStore);
	OutRenderTargets[MRTCount++] = FRHIRenderTargetView(GBufferC->GetRenderTargetItem().TargetableTexture, 0, -1, ColorLoadAction, ERenderTargetStoreAction::EStore);

	// The velocity buffer needs to be bound before other optionnal rendertargets (when UseSelecUseSelectiveBasePassOutputs() is true).
	// Otherwise there is an issue on some AMD hardware where the target does not get updated. Seems to be related to the velocity buffer format as it works fine with other targets.
	if (bAllocateVelocityGBuffer)
	{
		OutVelocityRTIndex = MRTCount;
		check(OutVelocityRTIndex == 4); // As defined in BasePassPixelShader.usf
		OutRenderTargets[MRTCount++] = FRHIRenderTargetView(GBufferVelocity->GetRenderTargetItem().TargetableTexture, 0, -1, ColorLoadAction, ERenderTargetStoreAction::EStore);
	}
	else
	{
		OutVelocityRTIndex = -1;
	}

	OutRenderTargets[MRTCount++] = FRHIRenderTargetView(GBufferD->GetRenderTargetItem().TargetableTexture, 0, -1, ColorLoadAction, ERenderTargetStoreAction::EStore);

	if (bAllowStaticLighting)
	{
		check(MRTCount == (bAllocateVelocityGBuffer ? 6 : 5)); // As defined in BasePassPixelShader.usf
		OutRenderTargets[MRTCount++] = FRHIRenderTargetView(GBufferE->GetRenderTargetItem().TargetableTexture, 0, -1, ColorLoadAction, ERenderTargetStoreAction::EStore);
	}

	check(MRTCount <= MaxSimultaneousRenderTargets);
	return MRTCount;
}

void FSceneRenderTargets::SetQuadOverdrawUAV(FRHICommandList& RHICmdList, bool bBindQuadOverdrawBuffers, FRHISetRenderTargetsInfo& Info)
{
	if (bBindQuadOverdrawBuffers && AllowDebugViewPS(DVSM_QuadComplexity, GetFeatureLevelShaderPlatform(CurrentFeatureLevel)))
	{
		if (QuadOverdrawBuffer.IsValid() && QuadOverdrawBuffer->GetRenderTargetItem().UAV.IsValid())
		{
			QuadOverdrawIndex = IsAnyForwardShadingEnabled(GetFeatureLevelShaderPlatform(CurrentFeatureLevel)) ? 1 : 7; // As defined in QuadOverdraw.usf

			// Increase the rendertarget count in order to control the bound slot of the UAV.
			check(Info.NumColorRenderTargets <= QuadOverdrawIndex);
			Info.NumColorRenderTargets = QuadOverdrawIndex;
			Info.UnorderedAccessView[Info.NumUAVs++] = QuadOverdrawBuffer->GetRenderTargetItem().UAV;

			// Clear to default value
			ClearUAV(RHICmdList, GMaxRHIFeatureLevel, QuadOverdrawBuffer->GetRenderTargetItem(), FLinearColor::Transparent);
			RHICmdList.TransitionResource(EResourceTransitionAccess::ERWBarrier, EResourceTransitionPipeline::EGfxToGfx, QuadOverdrawBuffer->GetRenderTargetItem().UAV);
		}
	}
}

void FSceneRenderTargets::BeginRenderingGBuffer(FRHICommandList& RHICmdList, ERenderTargetLoadAction ColorLoadAction, ERenderTargetLoadAction DepthLoadAction, FExclusiveDepthStencil::Type DepthStencilAccess, bool bBindQuadOverdrawBuffers, const FLinearColor& ClearColor/*=(0,0,0,1)*/)
{
	SCOPED_DRAW_EVENT(RHICmdList, BeginRenderingSceneColor);
	check(CurrentFeatureLevel >= ERHIFeatureLevel::SM4);
	AllocSceneColor(RHICmdList);

	FRHIRenderTargetView RenderTargets[MaxSimultaneousRenderTargets];
	const ERenderTargetStoreAction DepthStoreAction = (DepthStencilAccess & FExclusiveDepthStencil::DepthWrite) ? ERenderTargetStoreAction::EStore : ERenderTargetStoreAction::ENoAction;
	FRHIDepthRenderTargetView DepthView(GetSceneDepthSurface(), DepthLoadAction, DepthStoreAction, DepthStencilAccess);

	if (IsAnyForwardShadingEnabled(GetFeatureLevelShaderPlatform(CurrentFeatureLevel)))
	{
		int32 MRTCount = 0;
		RenderTargets[MRTCount++] = FRHIRenderTargetView(GetSceneColorSurface(), 0, -1, ColorLoadAction, ERenderTargetStoreAction::EStore);

		FRHISetRenderTargetsInfo Info(MRTCount, RenderTargets, DepthView);	

		SetQuadOverdrawUAV(RHICmdList, bBindQuadOverdrawBuffers, Info);

		RHICmdList.SetRenderTargetsAndClear(Info);
	}
	else
	{
		bool bClearColor = ColorLoadAction == ERenderTargetLoadAction::EClear;
		bool bClearDepth = DepthLoadAction == ERenderTargetLoadAction::EClear;

		//if the desired clear color doesn't match the bound hwclear value, or there isn't one at all (editor code)
		//then we need to fall back to a shader clear.
		const FTextureRHIRef& SceneColorTex = GetSceneColorSurface();
		bool bShaderClear = false;
		if (bClearColor)
		{
			if (!SceneColorTex->HasClearValue() || (ClearColor != SceneColorTex->GetClearColor()))
			{
				ColorLoadAction = ERenderTargetLoadAction::ENoAction;
				bShaderClear = true;
			}
			else
			{
				bGBuffersFastCleared = true;
			}
		}

		int32 VelocityRTIndex;
		int32 MRTCount = GetGBufferRenderTargets(ColorLoadAction, RenderTargets, VelocityRTIndex);

		//make sure our conditions for shader clear fallback are valid.
		check(RenderTargets[0].Texture == SceneColorTex);

		FRHISetRenderTargetsInfo Info(MRTCount, RenderTargets, DepthView);

		if (bClearDepth)
		{
			bSceneDepthCleared = true;
		}

		SetQuadOverdrawUAV(RHICmdList, bBindQuadOverdrawBuffers, Info);

		// set the render target
		RHICmdList.SetRenderTargetsAndClear(Info);
		if (bShaderClear)
		{
			FLinearColor ClearColors[MaxSimultaneousRenderTargets];
			FTextureRHIParamRef Textures[MaxSimultaneousRenderTargets];
			ClearColors[0] = ClearColor;
			Textures[0] = RenderTargets[0].Texture;
			for (int32 i = 1; i < MRTCount; ++i)
			{
				ClearColors[i] = RenderTargets[i].Texture->GetClearColor();
				Textures[i] = RenderTargets[i].Texture;
			}
			//depth/stencil should have been handled by the fast clear.  only color for RT0 can get changed.
			DrawClearQuadMRT(RHICmdList, GMaxRHIFeatureLevel, true, MRTCount, ClearColors, false, 0, false, 0);
		}

		//bind any clear data that won't be bound automatically by the preceding SetRenderTargetsAndClear
		bool bBindClearColor = !bClearColor && bGBuffersFastCleared;
		bool bBindClearDepth = !bClearDepth && bSceneDepthCleared;
		RHICmdList.BindClearMRTValues(bBindClearColor, bBindClearDepth, bBindClearDepth);
	}
}

void FSceneRenderTargets::FinishRenderingGBuffer(FRHICommandListImmediate& RHICmdList)
{
	int32 VelocityRTIndex;
	FRHIRenderTargetView RenderTargets[MaxSimultaneousRenderTargets];
	int32 NumMRTs = GetGBufferRenderTargets(ERenderTargetLoadAction::ELoad, RenderTargets, VelocityRTIndex);

	FResolveParams ResolveParams;
	for (int32 i = 0; i < NumMRTs; ++i)
	{
		 // When the basepass outputs to the velocity buffer, don't resolve it yet if selective outputs are enabled, as it will be resolved after the velocity pass.
		if (i != VelocityRTIndex || !UseSelectiveBasePassOutputs())
		{
			RHICmdList.CopyToResolveTarget(RenderTargets[i].Texture, RenderTargets[i].Texture, true, ResolveParams);
		}
	}

	QuadOverdrawIndex = INDEX_NONE;
}

int32 FSceneRenderTargets::GetNumGBufferTargets() const
{
	int32 NumGBufferTargets = 1;

	if (CurrentFeatureLevel >= ERHIFeatureLevel::SM4 && !IsAnyForwardShadingEnabled(GetFeatureLevelShaderPlatform(CurrentFeatureLevel)))
	{
		// This needs to match TBasePassPixelShaderBaseType::ModifyCompilationEnvironment()
		NumGBufferTargets = bAllowStaticLighting ? 6 : 5;

		if (bAllocateVelocityGBuffer)
		{
			++NumGBufferTargets;
		}
	}
	return NumGBufferTargets;
}

void FSceneRenderTargets::AllocSceneColor(FRHICommandList& RHICmdList)
{
	TRefCountPtr<IPooledRenderTarget>& SceneColorTarget = GetSceneColorForCurrentShadingPath();
	if (SceneColorTarget && 
		SceneColorTarget->GetRenderTargetItem().TargetableTexture->HasClearValue() && 
		!(SceneColorTarget->GetRenderTargetItem().TargetableTexture->GetClearBinding() == DefaultColorClear))
	{
		const FLinearColor CurrentClearColor = SceneColorTarget->GetRenderTargetItem().TargetableTexture->GetClearBinding().GetClearColor();
		const FLinearColor NewClearColor = DefaultColorClear.GetClearColor();
		UE_LOG(LogRenderer, Log, TEXT("Releasing previous color target to switch default clear from: %f %f %f %f to: %f %f %f %f"), 
			CurrentClearColor.R, 
			CurrentClearColor.G, 
			CurrentClearColor.B, 
			CurrentClearColor.A, 
			NewClearColor.R, 
			NewClearColor.G, 
			NewClearColor.B, 
			NewClearColor.A);
		SceneColorTarget.SafeRelease();
	}

	if (GetSceneColorForCurrentShadingPath())
	{
		return;
	}

	EPixelFormat SceneColorBufferFormat = GetSceneColorFormat();

	// Create the scene color.
	{
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(BufferSize, SceneColorBufferFormat, DefaultColorClear, TexCreate_None, TexCreate_RenderTargetable, false));
		if (CVarFastVRamSceneColor.GetValueOnRenderThread() >= 1)
		{
			Desc.Flags |= TexCreate_FastVRAM;
		}
		Desc.NumSamples = GetNumSceneColorMSAASamples(CurrentFeatureLevel);

		if (CurrentFeatureLevel >= ERHIFeatureLevel::SM5 && Desc.NumSamples == 1)
		{
			// GCNPerformanceTweets.pdf Tip 37: Warning: Causes additional synchronization between draw calls when using a render target allocated with this flag, use sparingly
			Desc.TargetableFlags |= TexCreate_UAV;
		}

		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, GetSceneColorForCurrentShadingPath(), GetSceneColorTargetName(CurrentShadingPath));
	}

	check(GetSceneColorForCurrentShadingPath());
}

void FSceneRenderTargets::AllocMobileMultiViewSceneColor(FRHICommandList& RHICmdList)
{
	// For mono support. 
	// Ensure we clear alpha to 0. We use alpha to tag which pixels had objects rendered into them so we can mask them out for the mono pass
	if (MobileMultiViewSceneColor && !(MobileMultiViewSceneColor->GetRenderTargetItem().TargetableTexture->GetClearBinding() == DefaultColorClear))
	{
		MobileMultiViewSceneColor.SafeRelease();
	}

	if (!MobileMultiViewSceneColor)
	{
		const EPixelFormat SceneColorBufferFormat = GetSceneColorFormat();
		const FIntPoint MultiViewBufferSize(BufferSize.X / 2, BufferSize.Y);

		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(MultiViewBufferSize, SceneColorBufferFormat, DefaultColorClear, TexCreate_None, TexCreate_RenderTargetable, false));
		Desc.Flags |= TexCreate_FastVRAM;
		Desc.NumSamples = GetNumSceneColorMSAASamples(CurrentFeatureLevel);
		Desc.ArraySize = 2;
		Desc.bIsArray = true;
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, MobileMultiViewSceneColor, TEXT("MobileMultiViewSceneColor"));
	}
	check(MobileMultiViewSceneColor);
}

void FSceneRenderTargets::AllocMobileMultiViewDepth(FRHICommandList& RHICmdList)
{
	// For mono support. We change the default depth clear value to the mono clip plane to clip the stereo portion of the frustum.
	if (MobileMultiViewSceneDepthZ && !(MobileMultiViewSceneDepthZ->GetRenderTargetItem().TargetableTexture->GetClearBinding() == DefaultDepthClear))
	{
		MobileMultiViewSceneDepthZ.SafeRelease();
	}

	if (!MobileMultiViewSceneDepthZ)
	{
		const FIntPoint MultiViewBufferSize(BufferSize.X / 2, BufferSize.Y);

		// Using the result of GetDepthFormat() without stencil due to packed depth-stencil not working in array frame buffers.
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(MultiViewBufferSize, PF_D24, DefaultDepthClear, TexCreate_None, TexCreate_DepthStencilTargetable, false));
		Desc.Flags |= TexCreate_FastVRAM;
		Desc.NumSamples = GetNumSceneColorMSAASamples(CurrentFeatureLevel);
		Desc.ArraySize = 2;
		Desc.bIsArray = true;
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, MobileMultiViewSceneDepthZ, TEXT("MobileMultiViewSceneDepthZ"));
	}
	check(MobileMultiViewSceneDepthZ);
}

void FSceneRenderTargets::AllocSceneMonoRenderTargets(FRHICommandList& RHICmdList, const FSceneView& MonoView)
{
	if (SceneMonoColor && SceneMonoDepthZ)
	{
		return;
	}

	const EPixelFormat SceneColorBufferFormat = GetSceneColorFormat();

	{
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(MonoView.ViewRect.Max, SceneColorBufferFormat, FClearValueBinding::Black, TexCreate_None, TexCreate_RenderTargetable, false));
	
		Desc.Flags |= TexCreate_FastVRAM;
		Desc.NumSamples = GetNumSceneColorMSAASamples(CurrentFeatureLevel);

		if (CurrentFeatureLevel >= ERHIFeatureLevel::SM5 && Desc.NumSamples == 1)
		{
			Desc.TargetableFlags |= TexCreate_UAV;
		}

		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, SceneMonoColor, TEXT("SceneMonoColor"));
	}

	{
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(MonoView.ViewRect.Max, PF_DepthStencil, FClearValueBinding::DepthFar, TexCreate_None, TexCreate_DepthStencilTargetable, false));
		Desc.NumSamples = GetNumSceneColorMSAASamples(CurrentFeatureLevel);
		Desc.Flags |= TexCreate_FastVRAM;
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, SceneMonoDepthZ, TEXT("SceneMonoDepthZ"));
	}

	UE_LOG(LogRenderer, Log, TEXT("Allocating monoscopic scene render targets to support %ux%u"), MonoView.ViewRect.Max.X, MonoView.ViewRect.Max.Y);

	check(SceneMonoColor);
	check(SceneMonoDepthZ);
}

void FSceneRenderTargets::AllocLightAttenuation(FRHICommandList& RHICmdList)
{
	if(LightAttenuation)
	{
		// no work needed
		return;
	}

	check(IsInRenderingThread());

	// Create a texture to store the resolved light attenuation values, and a render-targetable surface to hold the unresolved light attenuation values.
	{
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(BufferSize, PF_B8G8R8A8, FClearValueBinding::White, TexCreate_None, TexCreate_RenderTargetable, false));
		if (CVarFastVRamLightAttenuation.GetValueOnRenderThread() >= 1)
		{
			Desc.Flags |= TexCreate_FastVRAM;
		}
		Desc.NumSamples = GetNumSceneColorMSAASamples(CurrentFeatureLevel);
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, LightAttenuation, TEXT("LightAttenuation"));
	}

	// otherwise we have a severe problem
	check(LightAttenuation);
}

void FSceneRenderTargets::ReleaseGBufferTargets()
{
	GBufferResourcesUniformBuffer.SafeRelease();
	GBufferA.SafeRelease();
	GBufferB.SafeRelease();
	GBufferC.SafeRelease();
	GBufferD.SafeRelease();
	GBufferE.SafeRelease();
	GBufferVelocity.SafeRelease();
}

void FSceneRenderTargets::PreallocGBufferTargets()
{
	bAllocateVelocityGBuffer = FVelocityRendering::OutputsToGBuffer();
}

void FSceneRenderTargets::GetGBufferADesc(FPooledRenderTargetDesc& Desc) const
{
	// good to see the quality loss due to precision in the gbuffer
	const bool bHighPrecisionGBuffers = (CurrentGBufferFormat >= EGBufferFormat::Force16BitsPerChannel);
	// good to profile the impact of non 8 bit formats
	const bool bEnforce8BitPerChannel = (CurrentGBufferFormat == EGBufferFormat::Force8BitsPerChannel);

	// Create the world-space normal g-buffer.
	{
		EPixelFormat NormalGBufferFormat = bHighPrecisionGBuffers ? PF_FloatRGBA : PF_A2B10G10R10;

		if (bEnforce8BitPerChannel)
		{
			NormalGBufferFormat = PF_B8G8R8A8;
		}
		else if (CurrentGBufferFormat == EGBufferFormat::HighPrecisionNormals)
		{
			NormalGBufferFormat = PF_FloatRGBA;
		}

		if (CVarFastVRamGBufferCount.GetValueOnRenderThread() >= 1)
		{
			Desc.Flags |= TexCreate_FastVRAM;
		}

		Desc = FPooledRenderTargetDesc::Create2DDesc(BufferSize, NormalGBufferFormat, FClearValueBinding::Transparent, TexCreate_None, TexCreate_RenderTargetable, false);
	}
}

void FSceneRenderTargets::AllocGBufferTargets(FRHICommandList& RHICmdList)
{	
	// AdjustGBufferRefCount +1 doesn't match -1 (within the same frame)
	ensure(GBufferRefCount == 0);

	if (GBufferA)
	{
		// no work needed
		return;
	}

	// create GBuffer on demand so it can be shared with other pooled RT
	const EShaderPlatform ShaderPlatform = GetFeatureLevelShaderPlatform(CurrentFeatureLevel);
	const bool bUseGBuffer = IsUsingGBuffers(ShaderPlatform);
	const bool bCanReadGBufferUniforms = (bUseGBuffer || IsSimpleForwardShadingEnabled(ShaderPlatform)) && CurrentFeatureLevel >= ERHIFeatureLevel::SM4;
	if (bUseGBuffer)
	{
		// good to see the quality loss due to precision in the gbuffer
		const bool bHighPrecisionGBuffers = (CurrentGBufferFormat >= EGBufferFormat::Force16BitsPerChannel);
		// good to profile the impact of non 8 bit formats
		const bool bEnforce8BitPerChannel = (CurrentGBufferFormat == EGBufferFormat::Force8BitsPerChannel);

		// Create the world-space normal g-buffer.
		{
			FPooledRenderTargetDesc Desc;
			GetGBufferADesc(Desc);
			GRenderTargetPool.FindFreeElement(RHICmdList, Desc, GBufferA, TEXT("GBufferA"));
		}

		// Create the specular color and power g-buffer.
		{
			const EPixelFormat SpecularGBufferFormat = bHighPrecisionGBuffers ? PF_FloatRGBA : PF_B8G8R8A8;

			FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(BufferSize, SpecularGBufferFormat, FClearValueBinding::Transparent, TexCreate_None, TexCreate_RenderTargetable, false));
			if (CVarFastVRamGBufferCount.GetValueOnRenderThread() >= 2)
			{
				Desc.Flags |= TexCreate_FastVRAM;
			}
			GRenderTargetPool.FindFreeElement(RHICmdList, Desc, GBufferB, TEXT("GBufferB"));
		}

		// Create the diffuse color g-buffer.
		{
			const EPixelFormat DiffuseGBufferFormat = bHighPrecisionGBuffers ? PF_FloatRGBA : PF_B8G8R8A8;
			FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(BufferSize, DiffuseGBufferFormat, FClearValueBinding::Transparent, TexCreate_SRGB, TexCreate_RenderTargetable, false));
			if (CVarFastVRamGBufferCount.GetValueOnRenderThread() >= 3)
			{
				Desc.Flags |= TexCreate_FastVRAM;
			}
			GRenderTargetPool.FindFreeElement(RHICmdList, Desc, GBufferC, TEXT("GBufferC"));
		}

		// Create the mask g-buffer (e.g. SSAO, subsurface scattering, wet surface mask, skylight mask, ...).
		{
			FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(BufferSize, PF_B8G8R8A8, FClearValueBinding::Transparent, TexCreate_None, TexCreate_RenderTargetable, false));
			if (CVarFastVRamGBufferCount.GetValueOnRenderThread() >= 4)
			{
				Desc.Flags |= TexCreate_FastVRAM;
			}
			GRenderTargetPool.FindFreeElement(RHICmdList, Desc, GBufferD, TEXT("GBufferD"));
		}

		if (bAllowStaticLighting)
		{
			FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(BufferSize, PF_B8G8R8A8, FClearValueBinding::Transparent, TexCreate_None, TexCreate_RenderTargetable, false));
			if (CVarFastVRamGBufferCount.GetValueOnRenderThread() >= 5)
			{
				Desc.Flags |= TexCreate_FastVRAM;
			}
			GRenderTargetPool.FindFreeElement(RHICmdList, Desc, GBufferE, TEXT("GBufferE"));
		}

		if (bAllocateVelocityGBuffer)
		{
			FPooledRenderTargetDesc VelocityRTDesc = FVelocityRendering::GetRenderTargetDesc();
			GRenderTargetPool.FindFreeElement(RHICmdList, VelocityRTDesc, GBufferVelocity, TEXT("GBufferVelocity"));
		}

		// otherwise we have a severe problem
		check(GBufferA);
	}

	// Create the required render targets if running Highend.
	// Allow creation of dummy resources in case of simpleforward lighting.
	if (bCanReadGBufferUniforms)
	{

		// Allocate the Gbuffer resource uniform buffer.
		const FSceneRenderTargetItem& GBufferAToUse = GBufferA ? GBufferA->GetRenderTargetItem() : GSystemTextures.BlackDummy->GetRenderTargetItem();
		const FSceneRenderTargetItem& GBufferBToUse = GBufferB ? GBufferB->GetRenderTargetItem() : GSystemTextures.BlackDummy->GetRenderTargetItem();
		const FSceneRenderTargetItem& GBufferCToUse = GBufferC ? GBufferC->GetRenderTargetItem() : GSystemTextures.BlackDummy->GetRenderTargetItem();
		const FSceneRenderTargetItem& GBufferDToUse = GBufferD ? GBufferD->GetRenderTargetItem() : GSystemTextures.BlackDummy->GetRenderTargetItem();
		const FSceneRenderTargetItem& GBufferEToUse = GBufferE ? GBufferE->GetRenderTargetItem() : GSystemTextures.BlackDummy->GetRenderTargetItem();
		const FSceneRenderTargetItem& GBufferVelocityToUse = GBufferVelocity ? GBufferVelocity->GetRenderTargetItem() : GSystemTextures.BlackDummy->GetRenderTargetItem();

		FGBufferResourceStruct GBufferResourceStruct;

		GBufferResourceStruct.GBufferATexture = GBufferAToUse.ShaderResourceTexture;
		GBufferResourceStruct.GBufferBTexture = GBufferBToUse.ShaderResourceTexture;
		GBufferResourceStruct.GBufferCTexture = GBufferCToUse.ShaderResourceTexture;
		GBufferResourceStruct.GBufferDTexture = GBufferDToUse.ShaderResourceTexture;
		GBufferResourceStruct.GBufferETexture = GBufferEToUse.ShaderResourceTexture;
		GBufferResourceStruct.GBufferVelocityTexture = GBufferVelocityToUse.ShaderResourceTexture;
		
		GBufferResourceStruct.GBufferATextureNonMS = GBufferAToUse.ShaderResourceTexture;
		GBufferResourceStruct.GBufferBTextureNonMS = GBufferBToUse.ShaderResourceTexture;
		GBufferResourceStruct.GBufferCTextureNonMS = GBufferCToUse.ShaderResourceTexture;
		GBufferResourceStruct.GBufferDTextureNonMS = GBufferDToUse.ShaderResourceTexture;
		GBufferResourceStruct.GBufferETextureNonMS = GBufferEToUse.ShaderResourceTexture;
		GBufferResourceStruct.GBufferVelocityTextureNonMS = GBufferVelocityToUse.ShaderResourceTexture;

		GBufferResourceStruct.GBufferATextureMS = GBufferAToUse.TargetableTexture;
		GBufferResourceStruct.GBufferBTextureMS = GBufferBToUse.TargetableTexture;
		GBufferResourceStruct.GBufferCTextureMS = GBufferCToUse.TargetableTexture;
		GBufferResourceStruct.GBufferDTextureMS = GBufferDToUse.TargetableTexture;
		GBufferResourceStruct.GBufferETextureMS = GBufferEToUse.TargetableTexture;
		GBufferResourceStruct.GBufferVelocityTextureMS = GBufferVelocityToUse.TargetableTexture;

		GBufferResourceStruct.GBufferATextureSampler = TStaticSamplerState<>::GetRHI();
		GBufferResourceStruct.GBufferBTextureSampler = TStaticSamplerState<>::GetRHI();
		GBufferResourceStruct.GBufferCTextureSampler = TStaticSamplerState<>::GetRHI();
		GBufferResourceStruct.GBufferDTextureSampler = TStaticSamplerState<>::GetRHI();
		GBufferResourceStruct.GBufferETextureSampler = TStaticSamplerState<>::GetRHI();
		GBufferResourceStruct.GBufferVelocityTextureSampler = TStaticSamplerState<>::GetRHI();

		GBufferResourcesUniformBuffer = FGBufferResourceStruct::CreateUniformBuffer(GBufferResourceStruct, UniformBuffer_SingleFrame);
	}

	GBufferRefCount = 1;
}

void FSceneRenderTargets::AllocDummyGBufferTargets(FRHICommandList& RHICmdList)
{
	if (GBufferDummyResourcesUniformBuffer)
	{
		return;
	}

	FTextureRHIRef BlackDummy = GSystemTextures.BlackDummy->GetRenderTargetItem().ShaderResourceTexture;	

	FGBufferResourceStruct GBufferResourceStruct;

	GBufferResourceStruct.GBufferATexture = BlackDummy;
	GBufferResourceStruct.GBufferBTexture = BlackDummy;
	GBufferResourceStruct.GBufferCTexture = BlackDummy;
	GBufferResourceStruct.GBufferDTexture = BlackDummy;
	GBufferResourceStruct.GBufferETexture = BlackDummy;
	GBufferResourceStruct.GBufferVelocityTexture = BlackDummy;

	GBufferResourceStruct.GBufferATextureNonMS = BlackDummy;
	GBufferResourceStruct.GBufferBTextureNonMS = BlackDummy;
	GBufferResourceStruct.GBufferCTextureNonMS = BlackDummy;
	GBufferResourceStruct.GBufferDTextureNonMS = BlackDummy;
	GBufferResourceStruct.GBufferETextureNonMS = BlackDummy;
	GBufferResourceStruct.GBufferVelocityTextureNonMS = BlackDummy;

	GBufferResourceStruct.GBufferATextureMS = BlackDummy;
	GBufferResourceStruct.GBufferBTextureMS = BlackDummy;
	GBufferResourceStruct.GBufferCTextureMS = BlackDummy;
	GBufferResourceStruct.GBufferDTextureMS = BlackDummy;
	GBufferResourceStruct.GBufferETextureMS = BlackDummy;
	GBufferResourceStruct.GBufferVelocityTextureMS = BlackDummy;

	GBufferResourceStruct.GBufferATextureSampler = TStaticSamplerState<>::GetRHI();
	GBufferResourceStruct.GBufferBTextureSampler = TStaticSamplerState<>::GetRHI();
	GBufferResourceStruct.GBufferCTextureSampler = TStaticSamplerState<>::GetRHI();
	GBufferResourceStruct.GBufferDTextureSampler = TStaticSamplerState<>::GetRHI();
	GBufferResourceStruct.GBufferETextureSampler = TStaticSamplerState<>::GetRHI();
	GBufferResourceStruct.GBufferVelocityTextureSampler = TStaticSamplerState<>::GetRHI();

	GBufferDummyResourcesUniformBuffer = FGBufferResourceStruct::CreateUniformBuffer(GBufferResourceStruct, UniformBuffer_SingleFrame);
}

const TRefCountPtr<IPooledRenderTarget>& FSceneRenderTargets::GetSceneColor() const
{
	if (!GetSceneColorForCurrentShadingPath())
	{
		// to avoid log/ensure spam
		static bool bFirst = true;
		if(bFirst)
		{
			bFirst = false;

			// the first called should be AllocSceneColor(), contact MartinM if that happens
			ensure(GetSceneColorForCurrentShadingPath());
		}

		return GSystemTextures.BlackDummy;
	}

	return GetSceneColorForCurrentShadingPath();
}

bool FSceneRenderTargets::IsSceneColorAllocated() const
{
	return GetSceneColorForCurrentShadingPath() != 0;
}

TRefCountPtr<IPooledRenderTarget>& FSceneRenderTargets::GetSceneColor()
{
	if (!GetSceneColorForCurrentShadingPath())
	{
		// to avoid log/ensure spam
		static bool bFirst = true;
		if(bFirst)
		{
			bFirst = false;

			// the first called should be AllocSceneColor(), contact MartinM if that happens
			ensure(GetSceneColorForCurrentShadingPath());
		}

		return GSystemTextures.BlackDummy;
	}

	return GetSceneColorForCurrentShadingPath();
}

void FSceneRenderTargets::SetSceneColor(IPooledRenderTarget* In)
{
	check(CurrentShadingPath < EShadingPath::Num);
	SceneColor[(int32)GetSceneColorFormatType()] = In;
}

void FSceneRenderTargets::SetLightAttenuation(IPooledRenderTarget* In)
{
	LightAttenuation = In;
}

const TRefCountPtr<IPooledRenderTarget>& FSceneRenderTargets::GetLightAttenuation() const
{
	if(!LightAttenuation)
	{
		// to avoid log/ensure spam
		static bool bFirst = true;
		if(bFirst)
		{
			bFirst = false;

			// First we need to call AllocLightAttenuation()
			ensure(LightAttenuation);
		}

		return GSystemTextures.WhiteDummy;
	}

	return LightAttenuation;
}

TRefCountPtr<IPooledRenderTarget>& FSceneRenderTargets::GetLightAttenuation()
{
	if(!LightAttenuation)
	{
		// to avoid log/ensure spam
		static bool bFirst = true;
		if(bFirst)
		{
			bFirst = false;

			// the first called should be AllocLightAttenuation()
			ensure(LightAttenuation);
		}

		return GSystemTextures.WhiteDummy;
	}

	return LightAttenuation;
}

void FSceneRenderTargets::AdjustGBufferRefCount(FRHICommandList& RHICmdList, int Delta)
{
	if (Delta > 0 && GBufferRefCount == 0)
	{
		AllocGBufferTargets(RHICmdList);
	}
	else
	{
		GBufferRefCount += Delta;

		if (GBufferRefCount == 0)
		{
			ReleaseGBufferTargets();
		}
	}	
}

bool FSceneRenderTargets::BeginRenderingCustomDepth(FRHICommandListImmediate& RHICmdList, bool bPrimitives)
{
	IPooledRenderTarget* CustomDepthRenderTarget = RequestCustomDepth(RHICmdList, bPrimitives);

	if(CustomDepthRenderTarget)
	{
		SCOPED_DRAW_EVENT(RHICmdList, BeginRenderingCustomDepth);

		const bool bWritesCustomStencilValues = IsCustomDepthPassWritingStencil();
		const bool bRequiresStencilColorTarget = (bWritesCustomStencilValues && CurrentFeatureLevel <= ERHIFeatureLevel::ES3_1);

		int32 NumColorTargets = 0;
		FRHIRenderTargetView ColorView = {};
		if (bRequiresStencilColorTarget)
		{
			checkSlow(MobileCustomStencil.IsValid());
			ColorView =	FRHIRenderTargetView(MobileCustomStencil->GetRenderTargetItem().ShaderResourceTexture, 0, -1, ERenderTargetLoadAction::EClear, ERenderTargetStoreAction::EStore);
			NumColorTargets = 1;
		}

		FRHIDepthRenderTargetView DepthView(CustomDepthRenderTarget->GetRenderTargetItem().ShaderResourceTexture, ERenderTargetLoadAction::EClear, ERenderTargetStoreAction::EStore, ERenderTargetLoadAction::EClear, ERenderTargetStoreAction::EStore);
		FRHISetRenderTargetsInfo Info(NumColorTargets, &ColorView, DepthView);
		Info.bClearStencil = bWritesCustomStencilValues;
		check(DepthView.Texture->GetStencilClearValue() == 0);
		RHICmdList.SetRenderTargetsAndClear(Info);

		return true;
	}

	return false;
}

void FSceneRenderTargets::FinishRenderingCustomDepth(FRHICommandListImmediate& RHICmdList, const FResolveRect& ResolveRect)
{
	SCOPED_DRAW_EVENT(RHICmdList, FinishRenderingCustomDepth);

	RHICmdList.CopyToResolveTarget(CustomDepth->GetRenderTargetItem().TargetableTexture, CustomDepth->GetRenderTargetItem().ShaderResourceTexture, true, FResolveParams(ResolveRect));
	
	if (CurrentFeatureLevel <= ERHIFeatureLevel::ES3_1 && IsCustomDepthPassWritingStencil() && MobileCustomStencil.IsValid())
	{
		RHICmdList.CopyToResolveTarget(MobileCustomStencil->GetRenderTargetItem().TargetableTexture, MobileCustomStencil->GetRenderTargetItem().ShaderResourceTexture, true, FResolveParams(ResolveRect));
	}

	bCustomDepthIsValid = true;
}

void FSceneRenderTargets::BeginRenderingPrePass(FRHICommandList& RHICmdList, bool bPerformClear)
{
	SCOPED_DRAW_EVENT(RHICmdList, BeginRenderingPrePass);

	FTextureRHIRef ColorTarget;
	FTexture2DRHIRef DepthTarget = GetSceneDepthSurface();
	
	if (bPerformClear)
	{				
		FRHIRenderTargetView ColorView(ColorTarget, 0, -1, ERenderTargetLoadAction::ENoAction, ERenderTargetStoreAction::ENoAction);
		FRHIDepthRenderTargetView DepthView(DepthTarget, ERenderTargetLoadAction::EClear, ERenderTargetStoreAction::EStore);

		// Clear the depth buffer.
		// Note, this is a reversed Z depth surface, so 0.0f is the far plane.
		FRHISetRenderTargetsInfo Info(1, &ColorView, DepthView);
	
		RHICmdList.SetRenderTargetsAndClear(Info);
		bSceneDepthCleared = true;	
	}
	else
	{
		// Set the scene depth surface and a DUMMY buffer as color buffer
		// (as long as it's the same dimension as the depth buffer),	
		FRHIRenderTargetView ColorView(ColorTarget, 0, -1, ERenderTargetLoadAction::ENoAction, ERenderTargetStoreAction::ENoAction);
		FRHIDepthRenderTargetView DepthRTV(DepthTarget, ERenderTargetLoadAction::ELoad, ERenderTargetStoreAction::EStore);
		
		RHICmdList.SetRenderTargets(1, &ColorView, &DepthRTV, 0, NULL);
		
		RHICmdList.BindClearMRTValues(false, true, true);
	}
}

void FSceneRenderTargets::FinishRenderingPrePass(FRHICommandListImmediate& RHICmdList)
{
	SCOPED_DRAW_EVENT(RHICmdList, FinishRenderingPrePass);
	GRenderTargetPool.VisualizeTexture.SetCheckPoint(RHICmdList, SceneDepthZ);
}

void FSceneRenderTargets::BeginRenderingSceneAlphaCopy(FRHICommandListImmediate& RHICmdList)
{
	SCOPED_DRAW_EVENT(RHICmdList, BeginRenderingSceneAlphaCopy);
	GRenderTargetPool.VisualizeTexture.SetCheckPoint(RHICmdList, SceneAlphaCopy);
	SetRenderTarget(RHICmdList, GetSceneAlphaCopySurface(), 0);
}

void FSceneRenderTargets::FinishRenderingSceneAlphaCopy(FRHICommandListImmediate& RHICmdList)
{
	SCOPED_DRAW_EVENT(RHICmdList, FinishRenderingSceneAlphaCopy);
	RHICmdList.CopyToResolveTarget(GetSceneAlphaCopySurface(), SceneAlphaCopy->GetRenderTargetItem().ShaderResourceTexture, false, FResolveParams(FResolveRect()));
	GRenderTargetPool.VisualizeTexture.SetCheckPoint(RHICmdList, SceneAlphaCopy);
}


void FSceneRenderTargets::BeginRenderingLightAttenuation(FRHICommandList& RHICmdList, bool bClearToWhite)
{
	SCOPED_CONDITIONAL_DRAW_EVENT(RHICmdList, ClearLightAttenuation, bClearToWhite);
	SCOPED_CONDITIONAL_DRAW_EVENT(RHICmdList, BeginRenderingLightAttenuation, !bClearToWhite);

	AllocLightAttenuation(RHICmdList);

	GRenderTargetPool.VisualizeTexture.SetCheckPoint(RHICmdList, GetLightAttenuation());

	// Set the light attenuation surface as the render target, and the scene depth buffer as the depth-stencil surface.
	if (bClearToWhite)
	{
		TransitionSetRenderTargetsHelper(RHICmdList, GetLightAttenuationSurface(), GetSceneDepthSurface(), FExclusiveDepthStencil::DepthRead_StencilWrite);

		FRHIRenderTargetView View = FRHIRenderTargetView(GetLightAttenuationSurface(), ERenderTargetLoadAction::EClear);
		FRHISetRenderTargetsInfo Info(1, &View, FRHIDepthRenderTargetView(GetSceneDepthSurface(), ERenderTargetLoadAction::ELoad, ERenderTargetStoreAction::ENoAction, ERenderTargetLoadAction::ELoad, ERenderTargetStoreAction::ENoAction));
		RHICmdList.SetRenderTargetsAndClear(Info);
	}
	else
	{
		SetRenderTarget(RHICmdList, GetLightAttenuationSurface(), GetSceneDepthSurface(), ESimpleRenderTargetMode::EExistingColorAndDepth, FExclusiveDepthStencil::DepthRead_StencilWrite, true);
	}
}

void FSceneRenderTargets::FinishRenderingLightAttenuation(FRHICommandList& RHICmdList)
{
	SCOPED_DRAW_EVENT(RHICmdList, FinishRenderingLightAttenuation);

	// Resolve the light attenuation surface.
	RHICmdList.CopyToResolveTarget(GetLightAttenuationSurface(), LightAttenuation->GetRenderTargetItem().ShaderResourceTexture, false, FResolveParams(FResolveRect()));
	
	GRenderTargetPool.VisualizeTexture.SetCheckPoint(RHICmdList, GetLightAttenuation());
}

TRefCountPtr<IPooledRenderTarget>& FSceneRenderTargets::GetSeparateTranslucency(FRHICommandList& RHICmdList, FIntPoint Size)
{
	if (!SeparateTranslucencyRT || SeparateTranslucencyRT->GetDesc().Extent != Size)
	{
		uint32 Flags = TexCreate_RenderTargetable;

		// Create the SeparateTranslucency render target (alpha is needed to lerping)
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(Size, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_None, Flags, false));
		Desc.AutoWritable = false;
		Desc.NumSamples = GetNumSceneColorMSAASamples(CurrentFeatureLevel);
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, SeparateTranslucencyRT, TEXT("SeparateTranslucency"));
	}
	return SeparateTranslucencyRT;
}

TRefCountPtr<IPooledRenderTarget>& FSceneRenderTargets::GetSeparateTranslucencyDepth(FRHICommandList& RHICmdList, FIntPoint Size)
{
	if (!SeparateTranslucencyDepthRT || SeparateTranslucencyDepthRT->GetDesc().Extent != Size)
	{
		// Create the SeparateTranslucency depth render target 
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(Size, PF_DepthStencil, FClearValueBinding::None, TexCreate_None, TexCreate_DepthStencilTargetable, false));
		Desc.NumSamples = GetNumSceneColorMSAASamples(CurrentFeatureLevel);
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, SeparateTranslucencyDepthRT, TEXT("SeparateTranslucencyDepth"));
	}
	return SeparateTranslucencyDepthRT;
}

void FSceneRenderTargets::BeginRenderingTranslucency(FRHICommandList& RHICmdList, const FViewInfo& View, bool bFirstTimeThisFrame)
{
	// Use the scene color buffer.
	BeginRenderingSceneColor(RHICmdList, ESimpleRenderTargetMode::EExistingColorAndDepth, FExclusiveDepthStencil::DepthRead_StencilWrite);

	if (bFirstTimeThisFrame)
	{
		// Clear the stencil buffer for ResponsiveAA
		const FTexture2DRHIRef& DepthSurface = GetSceneDepthSurface();
		DrawClearQuad(RHICmdList, CurrentFeatureLevel, false, FLinearColor(), false, 0, true, 0, FIntPoint(DepthSurface->GetSizeX(), DepthSurface->GetSizeY()), View.ViewRect);
	}
		
	// viewport to match view size
	RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0.0f, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1.0f);
}

void FSceneRenderTargets::BeginRenderingSeparateTranslucency(FRHICommandList& RHICmdList, const FViewInfo& View, bool bFirstTimeThisFrame)
{
	check(IsSeparateTranslucencyActive(View));

	bSeparateTranslucencyPass = true;

	FIntPoint ScaledSize;
	float Scale = 1.0f;
	GetSeparateTranslucencyDimensions(ScaledSize, Scale);

	SCOPED_DRAW_EVENT(RHICmdList, BeginSeparateTranslucency);

	TRefCountPtr<IPooledRenderTarget>* SeparateTranslucency;
	if (bSnapshot)
	{
		check(SeparateTranslucencyRT.GetReference());
		SeparateTranslucency = &SeparateTranslucencyRT;
	}
	else
	{
		SeparateTranslucency = &GetSeparateTranslucency(RHICmdList, ScaledSize);
	}
	const FTexture2DRHIRef &SeparateTranslucencyDepth = Scale < 1.0f ? (const FTexture2DRHIRef&)GetSeparateTranslucencyDepth(RHICmdList, ScaledSize)->GetRenderTargetItem().TargetableTexture : GetSceneDepthSurface();

	check((*SeparateTranslucency)->GetRenderTargetItem().TargetableTexture->GetClearColor() == FLinearColor::Black);
	// clear the render target the first time, re-use afterwards
	SetRenderTarget(RHICmdList, (*SeparateTranslucency)->GetRenderTargetItem().TargetableTexture, SeparateTranslucencyDepth,
		bFirstTimeThisFrame ? ESimpleRenderTargetMode::EClearColorExistingDepth : ESimpleRenderTargetMode::EExistingColorAndDepth, FExclusiveDepthStencil::DepthRead_StencilWrite);


	if (!bFirstTimeThisFrame)
	{
		// Clear the stencil buffer for ResponsiveAA
		RHICmdList.BindClearMRTValues(true, false, true);
	}

	RHICmdList.SetViewport(View.ViewRect.Min.X * Scale, View.ViewRect.Min.Y * Scale, 0.0f, View.ViewRect.Max.X * Scale, View.ViewRect.Max.Y * Scale, 1.0f);
}

void FSceneRenderTargets::FinishRenderingSeparateTranslucency(FRHICommandList& RHICmdList)
{
	SCOPED_DRAW_EVENT(RHICmdList, FinishSeparateTranslucency);

	TRefCountPtr<IPooledRenderTarget>* SeparateTranslucency;
	TRefCountPtr<IPooledRenderTarget>* SeparateTranslucencyDepth;
	if (bSnapshot)
	{
		check(SeparateTranslucencyRT.GetReference());
		SeparateTranslucency = &SeparateTranslucencyRT;
		SeparateTranslucencyDepth = &SeparateTranslucencyDepthRT;
	}
	else
	{
		FIntPoint ScaledSize;
		float Scale = 1.0f;
		GetSeparateTranslucencyDimensions(ScaledSize, Scale);
		SeparateTranslucency = &GetSeparateTranslucency(RHICmdList, ScaledSize);
		SeparateTranslucencyDepth = &GetSeparateTranslucencyDepth(RHICmdList, ScaledSize);
	}

	RHICmdList.CopyToResolveTarget((*SeparateTranslucency)->GetRenderTargetItem().TargetableTexture, (*SeparateTranslucency)->GetRenderTargetItem().ShaderResourceTexture, true, FResolveParams());
	RHICmdList.CopyToResolveTarget((*SeparateTranslucencyDepth)->GetRenderTargetItem().TargetableTexture, (*SeparateTranslucencyDepth)->GetRenderTargetItem().ShaderResourceTexture, true, FResolveParams());

	bSeparateTranslucencyPass = false;
}

void FSceneRenderTargets::ResolveSceneDepthTexture(FRHICommandList& RHICmdList, const FResolveRect& ResolveRect)
{
	SCOPED_DRAW_EVENT(RHICmdList, ResolveSceneDepthTexture);

	if (ResolveRect.IsValid())
	{
		RHICmdList.SetScissorRect(true, ResolveRect.X1, ResolveRect.Y1, ResolveRect.X2, ResolveRect.Y2);
	}

	RHICmdList.CopyToResolveTarget(GetSceneDepthSurface(), GetSceneDepthTexture(), true, FResolveParams());

	if (ResolveRect.IsValid())
	{
		RHICmdList.SetScissorRect(false, 0, 0, 0, 0);
	}
}

void FSceneRenderTargets::ResolveSceneDepthToAuxiliaryTexture(FRHICommandList& RHICmdList)
{
	// Resolve the scene depth to an auxiliary texture when SM3/SM4 is in use. This needs to happen so the auxiliary texture can be bound as a shader parameter
	// while the primary scene depth texture can be bound as the target. Simultaneously binding a single DepthStencil resource as a parameter and target
	// is unsupported in d3d feature level 10.
	if(!GSupportsDepthFetchDuringDepthTest)
	{
		SCOPED_DRAW_EVENT(RHICmdList, ResolveSceneDepthToAuxiliaryTexture);
		RHICmdList.CopyToResolveTarget(GetSceneDepthSurface(), GetAuxiliarySceneDepthTexture(), true, FResolveParams());
	}
}

void FSceneRenderTargets::CleanUpEditorPrimitiveTargets()
{
	EditorPrimitivesDepth.SafeRelease();
	EditorPrimitivesColor.SafeRelease();
}

int32 FSceneRenderTargets::GetEditorMSAACompositingSampleCount() const
{
	int32 Value = 1;

	// only supported on SM5 yet (SM4 doesn't have MSAA sample load functionality which makes it harder to implement)
	if (CurrentFeatureLevel >= ERHIFeatureLevel::SM5 && GRHISupportsMSAADepthSampleAccess)
	{
		static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.MSAA.CompositingSampleCount"));

		Value = CVar->GetValueOnRenderThread();

		if(Value <= 1)
		{
			Value = 1;
		}
		else if(Value <= 2)
		{
			Value = 2;
		}
		else if(Value <= 4)
		{
			Value = 4;
		}
		else
		{
			Value = 8;
		}
	}

	return Value;
}

const FTexture2DRHIRef& FSceneRenderTargets::GetEditorPrimitivesColor(FRHICommandList& RHICmdList)
{
	const bool bIsValid = IsValidRef(EditorPrimitivesColor);

	if( !bIsValid || EditorPrimitivesColor->GetDesc().NumSamples != GetEditorMSAACompositingSampleCount() )
	{
		// If the target is does not match the MSAA settings it needs to be recreated
		InitEditorPrimitivesColor(RHICmdList);
	}

	return (const FTexture2DRHIRef&)EditorPrimitivesColor->GetRenderTargetItem().TargetableTexture;
}


const FTexture2DRHIRef& FSceneRenderTargets::GetEditorPrimitivesDepth(FRHICommandList& RHICmdList)
{
	const bool bIsValid = IsValidRef(EditorPrimitivesDepth);

	if (!bIsValid || (CurrentFeatureLevel >= ERHIFeatureLevel::SM5 && EditorPrimitivesDepth->GetDesc().NumSamples != GetEditorMSAACompositingSampleCount()) )
	{
		// If the target is does not match the MSAA settings it needs to be recreated
		InitEditorPrimitivesDepth(RHICmdList);
	}

	return (const FTexture2DRHIRef&)EditorPrimitivesDepth->GetRenderTargetItem().TargetableTexture;
}

TAutoConsoleVariable<int32> FSceneRenderTargets::CVarSetSeperateTranslucencyEnabled(
	TEXT("r.SeparateTranslucency"),
	1,
	TEXT("Allows to disable the separate translucency feature (all translucency is rendered in separate RT and composited\n")
	TEXT("after DOF, if not specified otherwise in the material).\n")
	TEXT(" 0: off (translucency is affected by depth of field)\n")
	TEXT(" 1: on costs GPU performance and memory but keeps translucency unaffected by Depth of Field. (default)"),
	ECVF_RenderThreadSafe);

bool FSceneRenderTargets::IsSeparateTranslucencyActive(const FViewInfo& View) const
{	
	int32 Value = FSceneRenderTargets::CVarSetSeperateTranslucencyEnabled.GetValueOnRenderThread();

	return (Value != 0) && CurrentFeatureLevel >= ERHIFeatureLevel::SM4
		&& (View.Family->EngineShowFlags.PostProcessing || View.Family->EngineShowFlags.ShaderComplexity)
		&& View.Family->EngineShowFlags.SeparateTranslucency;
}

void FSceneRenderTargets::InitEditorPrimitivesColor(FRHICommandList& RHICmdList)
{
	FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(BufferSize, 
		PF_B8G8R8A8,
		FClearValueBinding::Transparent,
		TexCreate_None, 
		TexCreate_ShaderResource | TexCreate_RenderTargetable,
		false));

	Desc.NumSamples = GetEditorMSAACompositingSampleCount();

	GRenderTargetPool.FindFreeElement(RHICmdList, Desc, EditorPrimitivesColor, TEXT("EditorPrimitivesColor"));
}

void FSceneRenderTargets::InitEditorPrimitivesDepth(FRHICommandList& RHICmdList)
{
	FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(BufferSize, 
		PF_DepthStencil,
		FClearValueBinding::DepthFar,
		TexCreate_None, 
		TexCreate_ShaderResource | TexCreate_DepthStencilTargetable,
		false));

	Desc.NumSamples = GetEditorMSAACompositingSampleCount();

	GRenderTargetPool.FindFreeElement(RHICmdList, Desc, EditorPrimitivesDepth, TEXT("EditorPrimitivesDepth"));
}

void FSceneRenderTargets::SetBufferSize(int32 InBufferSizeX, int32 InBufferSizeY)
{
	QuantizeSceneBufferSize(InBufferSizeX, InBufferSizeY);
	BufferSize.X = InBufferSizeX;
	BufferSize.Y = InBufferSizeY;
}

void FSceneRenderTargets::SetSeparateTranslucencyBufferSize(bool bAnyViewWantsDownsampledSeparateTranslucency)
{
	static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataFloat(TEXT("r.SeparateTranslucencyScreenPercentage"));
	const float CVarScale = FMath::Clamp(CVar->GetValueOnRenderThread() / 100.0f, 0.0f, 100.0f);
	float EffectiveScale = CVarScale;

	// 'r.SeparateTranslucencyScreenPercentage' CVar wins over automatic downsampling
	if (FMath::Abs(CVarScale - 1.0f) < .001f && bAnyViewWantsDownsampledSeparateTranslucency)
	{
		EffectiveScale = .5f;
	}

	int32 ScaledX = GetBufferSizeXY().X * EffectiveScale;
	int32 ScaledY = GetBufferSizeXY().Y * EffectiveScale;
	SeparateTranslucencyBufferSize = FIntPoint(FMath::Max(ScaledX, 1), FMath::Max(ScaledY, 1));
	SeparateTranslucencyScale = EffectiveScale;
}

void FSceneRenderTargets::AllocateMobileRenderTargets(FRHICommandList& RHICmdList)
{
	// on ES2 we don't do on demand allocation of SceneColor yet (in non ES2 it's released in the Tonemapper Process())
	AllocSceneColor(RHICmdList);
	AllocateCommonDepthTargets(RHICmdList);

#if PLATFORM_ANDROID
	static const auto MobileMultiViewCVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("vr.MobileMultiView"));
	static const auto CVarMobileMultiViewDirect = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("vr.MobileMultiView.Direct"));

	const bool bIsUsingMobileMultiView = GSupportsMobileMultiView && (MobileMultiViewCVar && MobileMultiViewCVar->GetValueOnAnyThread() != 0);

	// TODO: Test platform support for direct
	const bool bIsMobileMultiViewDirectEnabled = (CVarMobileMultiViewDirect && CVarMobileMultiViewDirect->GetValueOnAnyThread() != 0);

	if (bIsUsingMobileMultiView)
	{
		if (!bIsMobileMultiViewDirectEnabled)
		{
			AllocMobileMultiViewSceneColor(RHICmdList);
		}

		AllocMobileMultiViewDepth(RHICmdList);
	}
#endif

	AllocateDebugViewModeTargets(RHICmdList);

	EPixelFormat Format = GetSceneColor()->GetDesc().Format;

#if PLATFORM_HTML5
	// For 64-bit ES2 without framebuffer fetch, create extra render target for copy of alpha channel.
	if((Format == PF_FloatRGBA) && (GSupportsShaderFramebufferFetch == false)) 
	{
		// creating a PF_R16F (a true one-channel renderable fp texture) is only supported on GL if EXT_texture_rg is available.  It's present
		// on iOS, but not in WebGL or Android.
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(BufferSize, PF_FloatRGBA, FClearValueBinding::None, TexCreate_None, TexCreate_RenderTargetable, false));
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, SceneAlphaCopy, TEXT("SceneAlphaCopy"));
	}
	else
#endif
	{
		SceneAlphaCopy = GSystemTextures.MaxFP16Depth;
	}
}

// for easier use of "VisualizeTexture"
static TCHAR* const GetVolumeName(uint32 Id, bool bDirectional)
{
	// (TCHAR*) for non VisualStudio
	switch(Id)
	{
		case 0: return bDirectional ? (TCHAR*)TEXT("TranslucentVolumeDir0") : (TCHAR*)TEXT("TranslucentVolume0");
		case 1: return bDirectional ? (TCHAR*)TEXT("TranslucentVolumeDir1") : (TCHAR*)TEXT("TranslucentVolume1");
		case 2: return bDirectional ? (TCHAR*)TEXT("TranslucentVolumeDir2") : (TCHAR*)TEXT("TranslucentVolume2");

		default:
			check(0);
	}
	return (TCHAR*)TEXT("InvalidName");
}

void FSceneRenderTargets::AllocateReflectionTargets(FRHICommandList& RHICmdList, int32 TargetSize)
{
	if (GSupportsRenderTargetFormat_PF_FloatRGBA)
	{
		const int32 NumReflectionCaptureMips = FMath::CeilLogTwo(TargetSize) + 1;

		if (ReflectionColorScratchCubemap[0] && ReflectionColorScratchCubemap[0]->GetRenderTargetItem().TargetableTexture->GetNumMips() != NumReflectionCaptureMips)
		{
			ReflectionColorScratchCubemap[0].SafeRelease();
			ReflectionColorScratchCubemap[1].SafeRelease();
		}

		// Reflection targets are shared between both mobile and deferred shading paths. If we have already allocated for one and are now allocating for the other,
		// we can skip these targets.
		bool bSharedReflectionTargetsAllocated = ReflectionColorScratchCubemap[0] != nullptr;

		if (!bSharedReflectionTargetsAllocated)
		{
			// We write to these cubemap faces individually during filtering
			uint32 CubeTexFlags = TexCreate_TargetArraySlicesIndependently;

			{
				// Create scratch cubemaps for filtering passes
				FPooledRenderTargetDesc Desc2(FPooledRenderTargetDesc::CreateCubemapDesc(TargetSize, PF_FloatRGBA, FClearValueBinding(FLinearColor(0, 10000, 0, 0)), CubeTexFlags, TexCreate_RenderTargetable, false, 1, NumReflectionCaptureMips));
				GRenderTargetPool.FindFreeElement(RHICmdList, Desc2, ReflectionColorScratchCubemap[0], TEXT("ReflectionColorScratchCubemap0"));
				GRenderTargetPool.FindFreeElement(RHICmdList, Desc2, ReflectionColorScratchCubemap[1], TEXT("ReflectionColorScratchCubemap1"));
			}

			extern int32 GDiffuseIrradianceCubemapSize;
			const int32 NumDiffuseIrradianceMips = FMath::CeilLogTwo(GDiffuseIrradianceCubemapSize) + 1;

			{
				FPooledRenderTargetDesc Desc2(FPooledRenderTargetDesc::CreateCubemapDesc(GDiffuseIrradianceCubemapSize, PF_FloatRGBA, FClearValueBinding(FLinearColor(0, 10000, 0, 0)), CubeTexFlags, TexCreate_RenderTargetable, false, 1, NumDiffuseIrradianceMips));
				GRenderTargetPool.FindFreeElement(RHICmdList, Desc2, DiffuseIrradianceScratchCubemap[0], TEXT("DiffuseIrradianceScratchCubemap0"));
				GRenderTargetPool.FindFreeElement(RHICmdList, Desc2, DiffuseIrradianceScratchCubemap[1], TEXT("DiffuseIrradianceScratchCubemap1"));
			}

			{
				FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(FSHVector3::MaxSHBasis, 1), PF_FloatRGBA, FClearValueBinding(FLinearColor(0, 10000, 0, 0)), TexCreate_None, TexCreate_RenderTargetable, false));
				GRenderTargetPool.FindFreeElement(RHICmdList, Desc, SkySHIrradianceMap, TEXT("SkySHIrradianceMap"));
			}
		}
	}
}

void FSceneRenderTargets::AllocateDebugViewModeTargets(FRHICommandList& RHICmdList)
{
	// If the shader/quad complexity shader need a quad overdraw buffer to be bind, allocate it.
	if (AllowDebugViewPS(DVSM_QuadComplexity, GetFeatureLevelShaderPlatform(CurrentFeatureLevel)))
	{
		FIntPoint QuadOverdrawSize;
		QuadOverdrawSize.X = 2 * FMath::Max<uint32>((BufferSize.X + 1) / 2, 1); // The size is time 2 since left side is QuadDescriptor, and right side QuadComplexity.
		QuadOverdrawSize.Y = FMath::Max<uint32>((BufferSize.Y + 1) / 2, 1);

		FPooledRenderTargetDesc QuadOverdrawDesc = FPooledRenderTargetDesc::Create2DDesc(
			QuadOverdrawSize, 
			PF_R32_UINT,
			FClearValueBinding::None,
			0,
			TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV,
			false
			);

		GRenderTargetPool.FindFreeElement(RHICmdList, QuadOverdrawDesc, QuadOverdrawBuffer, TEXT("QuadOverdrawBuffer"));
	}
}

void FSceneRenderTargets::AllocateCommonDepthTargets(FRHICommandList& RHICmdList)
{
	if (SceneDepthZ && !(SceneDepthZ->GetRenderTargetItem().TargetableTexture->GetClearBinding() == DefaultDepthClear))
	{
		uint32 StencilCurrent, StencilNew;
		float DepthCurrent, DepthNew;
		SceneDepthZ->GetRenderTargetItem().TargetableTexture->GetClearBinding().GetDepthStencil(DepthCurrent, StencilCurrent);
		DefaultDepthClear.GetDepthStencil(DepthNew, StencilNew);
		UE_LOG(LogRenderer, Log, TEXT("Releasing previous depth to switch default clear from depth: %f stencil: %u to depth: %f stencil: %u"), DepthCurrent, StencilCurrent, DepthNew, StencilNew);
		SceneDepthZ.SafeRelease();
	}

	if (!SceneDepthZ)
	{
		// Create a texture to store the resolved scene depth, and a render-targetable surface to hold the unresolved scene depth.
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(BufferSize, PF_DepthStencil, DefaultDepthClear, TexCreate_None, TexCreate_DepthStencilTargetable, false));
		Desc.NumSamples = GetNumSceneColorMSAASamples(CurrentFeatureLevel);
		if (CVarFastVRamSceneDepth.GetValueOnRenderThread() >= 1)
		{
			Desc.Flags |= TexCreate_FastVRAM;
		}
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, SceneDepthZ, TEXT("SceneDepthZ"));
		SceneStencilSRV = RHICreateShaderResourceView((FTexture2DRHIRef&)SceneDepthZ->GetRenderTargetItem().TargetableTexture, 0, 1, PF_X24_G8);
	}

	// When targeting DX Feature Level 10, create an auxiliary texture to store the resolved scene depth, and a render-targetable surface to hold the unresolved scene depth.
	if (!AuxiliarySceneDepthZ && !GSupportsDepthFetchDuringDepthTest)
	{
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(BufferSize, PF_DepthStencil, DefaultDepthClear, TexCreate_None, TexCreate_DepthStencilTargetable, false));
		Desc.AutoWritable = false;
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, AuxiliarySceneDepthZ, TEXT("AuxiliarySceneDepthZ"));
	}
}

const FTexture2DRHIRef& FSceneRenderTargets::GetOptionalShadowDepthColorSurface(FRHICommandList& RHICmdList, int32 Width, int32 Height) const
{
	// Look for matching resolution
	int32 EmptySlot = -1;
	for (int32 Index = 0; Index < ARRAY_COUNT(OptionalShadowDepthColor); Index++)
	{
		if (OptionalShadowDepthColor[Index])
		{
			const FTexture2DRHIRef& TargetTexture = (const FTexture2DRHIRef&)OptionalShadowDepthColor[Index]->GetRenderTargetItem().TargetableTexture;
			if (TargetTexture->GetSizeX() == Width && TargetTexture->GetSizeY() == Height)
			{
				return TargetTexture;
			}
		}
		else
		{
			// Remember this as a free slot for allocation attempt
			EmptySlot = Index;
		}
	}

	if (EmptySlot == -1)
	{
		UE_LOG(LogRenderer, Fatal, TEXT("Exceeded storage space for OptionalShadowDepthColorSurface. Increase array size."));
	}

	// Allocate new shadow color buffer (it must be the same resolution as the depth target!)
	const FIntPoint ShadowColorBufferResolution = FIntPoint(Width, Height);
	FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(ShadowColorBufferResolution, PF_B8G8R8A8, FClearValueBinding::None, TexCreate_None, TexCreate_RenderTargetable, false));
	GRenderTargetPool.FindFreeElement(RHICmdList, Desc, (TRefCountPtr<IPooledRenderTarget>&)OptionalShadowDepthColor[EmptySlot], TEXT("OptionalShadowDepthColor"));
	UE_LOG(LogRenderer, Log, TEXT("Allocated OptionalShadowDepthColorSurface %d x %d"), Width, Height);

	return (const FTexture2DRHIRef&)OptionalShadowDepthColor[EmptySlot]->GetRenderTargetItem().TargetableTexture;
}

void FSceneRenderTargets::AllocateLightingChannelTexture(FRHICommandList& RHICmdList)
{
	if (!LightingChannels)
	{
		// Only need 3 bits for lighting channels
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(BufferSize, PF_R16_UINT, FClearValueBinding::None, TexCreate_None, TexCreate_RenderTargetable, false));
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, LightingChannels, TEXT("LightingChannels"));
	}
}

void FSceneRenderTargets::AllocateDeferredShadingPathRenderTargets(FRHICommandList& RHICmdList)
{
	AllocateCommonDepthTargets(RHICmdList);

	// Create a quarter-sized version of the scene depth.
	{
		FIntPoint SmallDepthZSize(FMath::Max<uint32>(BufferSize.X / SmallColorDepthDownsampleFactor, 1), FMath::Max<uint32>(BufferSize.Y / SmallColorDepthDownsampleFactor, 1));

		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(SmallDepthZSize, PF_DepthStencil, FClearValueBinding::None, TexCreate_None, TexCreate_DepthStencilTargetable, true));
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, SmallDepthZ, TEXT("SmallDepthZ"));
	}

	// Create the required render targets if running Highend.
	if (CurrentFeatureLevel >= ERHIFeatureLevel::SM4)
	{
		// Create the screen space ambient occlusion buffer
		{
			FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(BufferSize, PF_G8, FClearValueBinding::White, TexCreate_None, TexCreate_RenderTargetable, false));

			if (CurrentFeatureLevel >= ERHIFeatureLevel::SM5)
			{
				// UAV is only needed to support "r.AmbientOcclusion.Compute"
				// todo: ideally this should be only UAV or RT, not both
				Desc.TargetableFlags |= TexCreate_UAV;
			}
			GRenderTargetPool.FindFreeElement(RHICmdList, Desc, ScreenSpaceAO, TEXT("ScreenSpaceAO"));
		}
		
		{
			// TODO: We can skip the and TLV allocations when rendering in forward shading mode
			uint32 TranslucencyTargetFlags = TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_ReduceMemoryWithTilingMode;

			if (CurrentFeatureLevel >= ERHIFeatureLevel::SM5)
			{
				TranslucencyTargetFlags |= TexCreate_UAV;
			}

			for (int32 RTSetIndex = 0; RTSetIndex < NumTranslucentVolumeRenderTargetSets; RTSetIndex++)
			{
				GRenderTargetPool.FindFreeElement(
					RHICmdList,
					FPooledRenderTargetDesc(FPooledRenderTargetDesc::CreateVolumeDesc(
						GTranslucencyLightingVolumeDim,
						GTranslucencyLightingVolumeDim,
						GTranslucencyLightingVolumeDim,
						PF_FloatRGBA,
						FClearValueBinding::Transparent,
						0,
						TranslucencyTargetFlags,
						false,
						1,
						false)),
					TranslucencyLightingVolumeAmbient[RTSetIndex],
					GetVolumeName(RTSetIndex, false)
					);

				//Tests to catch UE-31578, UE-32536 and UE-22073 crash (Defferred Render Targets not being allocated)
				ensureMsgf(TranslucencyLightingVolumeAmbient[RTSetIndex], TEXT("Failed to allocate render target %s with dimension %i and flags %i"),
					GetVolumeName(RTSetIndex, false),
					GTranslucencyLightingVolumeDim,
					TranslucencyTargetFlags);

				GRenderTargetPool.FindFreeElement(
					RHICmdList,
					FPooledRenderTargetDesc(FPooledRenderTargetDesc::CreateVolumeDesc(
						GTranslucencyLightingVolumeDim,
						GTranslucencyLightingVolumeDim,
						GTranslucencyLightingVolumeDim,
						PF_FloatRGBA,
						FClearValueBinding::Transparent,
						0,
						TranslucencyTargetFlags,
						false,
						1,
						false)),
					TranslucencyLightingVolumeDirectional[RTSetIndex],
					GetVolumeName(RTSetIndex, true)
					);

				//Tests to catch UE-31578, UE-32536 and UE-22073 crash
				ensureMsgf(TranslucencyLightingVolumeDirectional[RTSetIndex], TEXT("Failed to allocate render target %s with dimension %i and flags %i"),
					GetVolumeName(RTSetIndex, true),
					GTranslucencyLightingVolumeDim,
					TranslucencyTargetFlags);
			}
		}
	}

	// LPV : Dynamic directional occlusion for diffuse and specular
	if(UseLightPropagationVolumeRT(CurrentFeatureLevel))
	{
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(BufferSize, PF_R8G8, FClearValueBinding::Transparent, TexCreate_None, TexCreate_RenderTargetable, false));
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, DirectionalOcclusion, TEXT("DirectionalOcclusion"));
	}

	if (CurrentFeatureLevel >= ERHIFeatureLevel::SM4) 
	{
		FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(BufferSize, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_None, TexCreate_RenderTargetable, false));
		if (CurrentFeatureLevel >= ERHIFeatureLevel::SM5)
		{
			Desc.TargetableFlags |= TexCreate_UAV;
		}
		GRenderTargetPool.FindFreeElement(RHICmdList, Desc, LightAccumulation, TEXT("LightAccumulation"));
	}

	AllocateDebugViewModeTargets(RHICmdList);

	if (bAllocateVelocityGBuffer)
	{
		FPooledRenderTargetDesc VelocityRTDesc = FVelocityRendering::GetRenderTargetDesc();
		GRenderTargetPool.FindFreeElement(RHICmdList, VelocityRTDesc, GBufferVelocity, TEXT("GBufferVelocity"));
	}
}

EPixelFormat FSceneRenderTargets::GetSceneColorFormat() const
{
	EPixelFormat SceneColorBufferFormat = PF_FloatRGBA;

	if (CurrentFeatureLevel < ERHIFeatureLevel::SM4)
	{
		// Potentially allocate an alpha channel in th -fe scene color texture to store the resolved scene depth.
		SceneColorBufferFormat = GSupportsRenderTargetFormat_PF_FloatRGBA ? PF_FloatRGBA : PF_B8G8R8A8;
		if (!IsMobileHDR() || IsMobileHDR32bpp()) 
		{
			SceneColorBufferFormat = PF_B8G8R8A8;
		}
	}
	else
    {
	    switch(CurrentSceneColorFormat)
	    {
		    case 0:
			    SceneColorBufferFormat = PF_R8G8B8A8; break;
		    case 1:
			    SceneColorBufferFormat = PF_A2B10G10R10; break;
		    case 2:	
			    SceneColorBufferFormat = PF_FloatR11G11B10; break;
		    case 3:	
			    SceneColorBufferFormat = PF_FloatRGB; break;
		    case 4:
			    // default
			    break;
		    case 5:
			    SceneColorBufferFormat = PF_A32B32G32R32F; break;
	    }
    
		// Fallback in case the scene color selected isn't supported.
	    if (!GPixelFormats[SceneColorBufferFormat].Supported)
	    {
		    SceneColorBufferFormat = PF_FloatRGBA;
	    }

		if (bRequireSceneColorAlpha)
		{
			SceneColorBufferFormat = PF_FloatRGBA;
		}
	}

	return SceneColorBufferFormat;
}

void FSceneRenderTargets::AllocateRenderTargets(FRHICommandList& RHICmdList)
{
	if (BufferSize.X > 0 && BufferSize.Y > 0 && (!AreShadingPathRenderTargetsAllocated(GetSceneColorFormatType()) || !AreRenderTargetClearsValid(GetSceneColorFormatType())))
	{
		if ((EShadingPath)CurrentShadingPath == EShadingPath::Mobile)
		{
			AllocateMobileRenderTargets(RHICmdList);
		}
		else
		{
			AllocateDeferredShadingPathRenderTargets(RHICmdList);
		}
	}
}

void FSceneRenderTargets::ReleaseSceneColor()
{
	for (auto i = 0; i < (int32)ESceneColorFormatType::Num; ++i)
	{
		SceneColor[i].SafeRelease();
	}
	if (SceneMonoColor)
	{
		SceneMonoColor.SafeRelease();
		SceneMonoDepthZ.SafeRelease();
	}
}

void FSceneRenderTargets::ReleaseAllTargets()
{
	ReleaseGBufferTargets();

	ReleaseSceneColor();

	SceneAlphaCopy.SafeRelease();
	SceneDepthZ.SafeRelease();
	SceneStencilSRV.SafeRelease();
	LightingChannels.SafeRelease();
	AuxiliarySceneDepthZ.SafeRelease();
	SmallDepthZ.SafeRelease();
	DBufferA.SafeRelease();
	DBufferB.SafeRelease();
	DBufferC.SafeRelease();
	ScreenSpaceAO.SafeRelease();
	QuadOverdrawBuffer.SafeRelease();
	LightAttenuation.SafeRelease();
	LightAccumulation.SafeRelease();
	DirectionalOcclusion.SafeRelease();
	CustomDepth.SafeRelease();
	MobileCustomStencil.SafeRelease();
	CustomStencilSRV.SafeRelease();

	for (int32 i = 0; i < ARRAY_COUNT(OptionalShadowDepthColor); i++)
	{
		OptionalShadowDepthColor[i].SafeRelease();
	}

	for (int32 i = 0; i < ARRAY_COUNT(ReflectionColorScratchCubemap); i++)
	{
		ReflectionColorScratchCubemap[i].SafeRelease();
	}

	for (int32 i = 0; i < ARRAY_COUNT(DiffuseIrradianceScratchCubemap); i++)
	{
		DiffuseIrradianceScratchCubemap[i].SafeRelease();
	}

	SkySHIrradianceMap.SafeRelease();

	for (int32 RTSetIndex = 0; RTSetIndex < NumTranslucentVolumeRenderTargetSets; RTSetIndex++)
	{
		TranslucencyLightingVolumeAmbient[RTSetIndex].SafeRelease();
		TranslucencyLightingVolumeDirectional[RTSetIndex].SafeRelease();
	}

	MobileMultiViewSceneColor.SafeRelease();
	MobileMultiViewSceneDepthZ.SafeRelease();

	EditorPrimitivesColor.SafeRelease();
	EditorPrimitivesDepth.SafeRelease();
}

void FSceneRenderTargets::ReleaseDynamicRHI()
{
	ReleaseAllTargets();
	GRenderTargetPool.FreeUnusedResources();
}

/** Returns the size of the shadow depth buffer, taking into account platform limitations and game specific resolution limits. */
FIntPoint FSceneRenderTargets::GetShadowDepthTextureResolution() const
{
	int32 MaxShadowRes = CurrentMaxShadowResolution;
	const FIntPoint ShadowBufferResolution(
			FMath::Clamp(MaxShadowRes,1,GMaxShadowDepthBufferSizeX),
			FMath::Clamp(MaxShadowRes,1,GMaxShadowDepthBufferSizeY));
	
	return ShadowBufferResolution;
}

int32 FSceneRenderTargets::GetReflectiveShadowMapResolution() const
{
	check(IsInRenderingThread());

	return CurrentRSMResolution;
}

FIntPoint FSceneRenderTargets::GetPreShadowCacheTextureResolution() const
{
	const FIntPoint ShadowDepthResolution = GetShadowDepthTextureResolution();
	// Higher numbers increase cache hit rate but also memory usage
	const int32 ExpandFactor = 2;

	static auto CVarPreShadowResolutionFactor = IConsoleManager::Get().FindTConsoleVariableDataFloat(TEXT("r.Shadow.PreShadowResolutionFactor"));

	float Factor = CVarPreShadowResolutionFactor->GetValueOnRenderThread();

	FIntPoint Ret;

	Ret.X = FMath::Clamp(FMath::TruncToInt(ShadowDepthResolution.X * Factor) * ExpandFactor, 1, GMaxShadowDepthBufferSizeX);
	Ret.Y = FMath::Clamp(FMath::TruncToInt(ShadowDepthResolution.Y * Factor) * ExpandFactor, 1, GMaxShadowDepthBufferSizeY);

	return Ret;
}

FIntPoint FSceneRenderTargets::GetTranslucentShadowDepthTextureResolution() const
{
	FIntPoint ShadowDepthResolution = GetShadowDepthTextureResolution();

	int32 Factor = GetTranslucentShadowDownsampleFactor();

	ShadowDepthResolution.X = FMath::Clamp(ShadowDepthResolution.X / Factor, 1, GMaxShadowDepthBufferSizeX);
	ShadowDepthResolution.Y = FMath::Clamp(ShadowDepthResolution.Y / Factor, 1, GMaxShadowDepthBufferSizeY);

	return ShadowDepthResolution;
}

const FTextureRHIRef& FSceneRenderTargets::GetSceneColorSurface() const							
{
	if (!GetSceneColorForCurrentShadingPath())
	{
		return GBlackTexture->TextureRHI;
	}

	return (const FTextureRHIRef&)GetSceneColor()->GetRenderTargetItem().TargetableTexture;
}

const FTextureRHIRef& FSceneRenderTargets::GetSceneColorTexture() const
{
	if (!GetSceneColorForCurrentShadingPath())
	{
		return GBlackTexture->TextureRHI;
	}

	return (const FTextureRHIRef&)GetSceneColor()->GetRenderTargetItem().ShaderResourceTexture; 
}

const FTexture2DRHIRef* FSceneRenderTargets::GetActualDepthTexture() const
{
	const FTexture2DRHIRef* DepthTexture = NULL;
	if((CurrentFeatureLevel >= ERHIFeatureLevel::SM4) || IsPCPlatform(GShaderPlatformForFeatureLevel[CurrentFeatureLevel]))
	{
		if(GSupportsDepthFetchDuringDepthTest)
		{
			DepthTexture = &GetSceneDepthTexture();
		}
		else
		{
			DepthTexture = &GetAuxiliarySceneDepthSurface();
		}
	}
	else if (IsMobilePlatform(GShaderPlatformForFeatureLevel[CurrentFeatureLevel]))
	{
		// TODO: avoid depth texture fetch when shader needs fragment previous depth and device supports framebuffer fetch

		//bool bSceneDepthInAlpha = (GetSceneColor()->GetDesc().Format == PF_FloatRGBA);
		//bool bOnChipDepthFetch = (GSupportsShaderDepthStencilFetch || (bSceneDepthInAlpha && GSupportsShaderFramebufferFetch));
		//
		//if (bOnChipDepthFetch)
		//{
		//	DepthTexture = (const FTexture2DRHIRef*)(&GSystemTextures.DepthDummy->GetRenderTargetItem().ShaderResourceTexture);
		//}
		//else
		{
			DepthTexture = &GetSceneDepthTexture();
		}
	}

	check(DepthTexture != NULL);

	return DepthTexture;
}


IPooledRenderTarget* FSceneRenderTargets::GetGBufferVelocityRT()
{
	if (!bAllocateVelocityGBuffer)
	{
		return nullptr;
	}
	
	return GBufferVelocity;
}

IPooledRenderTarget* FSceneRenderTargets::RequestCustomDepth(FRHICommandListImmediate& RHICmdList, bool bPrimitives)
{
	int Value = CVarCustomDepth.GetValueOnRenderThread();
	const bool bCustomDepthPassWritingStencil = IsCustomDepthPassWritingStencil();
	const bool bMobilePath = (CurrentFeatureLevel <= ERHIFeatureLevel::ES3_1);

	if ((Value == 1 && bPrimitives) || Value == 2 || bCustomDepthPassWritingStencil)
	{
		bool bHasValidCustomDepth = (CustomDepth.IsValid() && BufferSize == CustomDepth->GetDesc().Extent);
		bool bHasValidCustomStencil;
		if (bMobilePath)
		{
			bHasValidCustomStencil = (MobileCustomStencil.IsValid() && BufferSize == MobileCustomStencil->GetDesc().Extent);
		}
		else
		{
			bHasValidCustomStencil = CustomStencilSRV.IsValid();
		}
						
		if (!(bHasValidCustomDepth && bHasValidCustomStencil))
		{
			// Skip depth decompression, custom depth doesn't benefit from it
			// Also disables fast clears, but typically only a small portion of custom depth is written to anyway
			uint32 CustomDepthFlags = TexCreate_NoFastClear;

			// Todo: Could check if writes stencil here and create min viable target
			FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(BufferSize, PF_DepthStencil, FClearValueBinding::DepthFar, CustomDepthFlags, TexCreate_DepthStencilTargetable, false));
			GRenderTargetPool.FindFreeElement(RHICmdList, Desc, CustomDepth, TEXT("CustomDepth"));
			
			if (bMobilePath)
			{
				FPooledRenderTargetDesc MobileCustomStencilDesc(FPooledRenderTargetDesc::Create2DDesc(BufferSize, PF_B8G8R8A8, FClearValueBinding::Transparent, TexCreate_None, TexCreate_RenderTargetable, false));
				GRenderTargetPool.FindFreeElement(RHICmdList, MobileCustomStencilDesc, MobileCustomStencil, TEXT("MobileCustomStencil"));
			}
			else
			{
				CustomStencilSRV = RHICreateShaderResourceView((FTexture2DRHIRef&)CustomDepth->GetRenderTargetItem().TargetableTexture, 0, 1, PF_X24_G8);
			}
		}
		return CustomDepth;
	}

	return 0;
}

bool FSceneRenderTargets::IsCustomDepthPassWritingStencil() const
{
	return (CVarCustomDepth.GetValueOnRenderThread() == 3);
}

/** Returns an index in the range [0, NumCubeShadowDepthSurfaces) given an input resolution. */
int32 FSceneRenderTargets::GetCubeShadowDepthZIndex(int32 ShadowResolution) const
{
	static auto CVarMinShadowResolution = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Shadow.MinResolution"));
	FIntPoint ObjectShadowBufferResolution = GetShadowDepthTextureResolution();

	// Use a lower resolution because cubemaps use a lot of memory
	ObjectShadowBufferResolution.X /= 2;
	ObjectShadowBufferResolution.Y /= 2;
	const int32 SurfaceSizes[NumCubeShadowDepthSurfaces] =
	{
		ObjectShadowBufferResolution.X,
		ObjectShadowBufferResolution.X / 2,
		ObjectShadowBufferResolution.X / 4,
		ObjectShadowBufferResolution.X / 8,
		CVarMinShadowResolution->GetValueOnRenderThread()
	};

	for (int32 SearchIndex = 0; SearchIndex < NumCubeShadowDepthSurfaces; SearchIndex++)
	{
		if (ShadowResolution >= SurfaceSizes[SearchIndex])
		{
			return SearchIndex;
		}
	}

	check(0);
	return 0;
}

/** Returns the appropriate resolution for a given cube shadow index. */
int32 FSceneRenderTargets::GetCubeShadowDepthZResolution(int32 ShadowIndex) const
{
	checkSlow(ShadowIndex >= 0 && ShadowIndex < NumCubeShadowDepthSurfaces);

	static auto CVarMinShadowResolution = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Shadow.MinResolution"));
	FIntPoint ObjectShadowBufferResolution = GetShadowDepthTextureResolution();

	// Use a lower resolution because cubemaps use a lot of memory
	ObjectShadowBufferResolution.X = FMath::Max(ObjectShadowBufferResolution.X / 2, 1);
	ObjectShadowBufferResolution.Y = FMath::Max(ObjectShadowBufferResolution.Y / 2, 1);
	const int32 SurfaceSizes[NumCubeShadowDepthSurfaces] =
	{
		ObjectShadowBufferResolution.X,
		FMath::Max(ObjectShadowBufferResolution.X / 2, 1),
		FMath::Max(ObjectShadowBufferResolution.X / 4, 1),
		FMath::Max(ObjectShadowBufferResolution.X / 8, 1),
		CVarMinShadowResolution->GetValueOnRenderThread()
	};
	return SurfaceSizes[ShadowIndex];
}

bool FSceneRenderTargets::AreRenderTargetClearsValid(ESceneColorFormatType InSceneColorFormatType) const
{
	switch (InSceneColorFormatType)
	{
	case ESceneColorFormatType::Mobile:
		{
			const TRefCountPtr<IPooledRenderTarget>& SceneColorTarget = GetSceneColorForCurrentShadingPath();
			const bool bColorValid = SceneColorTarget && (SceneColorTarget->GetRenderTargetItem().TargetableTexture->GetClearBinding() == DefaultColorClear);
			const bool bDepthValid = SceneDepthZ && (SceneDepthZ->GetRenderTargetItem().TargetableTexture->GetClearBinding() == DefaultDepthClear);
#if PLATFORM_ANDROID
			// For mobile multi-view + mono support
			const bool bMobileMultiViewColorValid = (!MobileMultiViewSceneColor || MobileMultiViewSceneColor->GetRenderTargetItem().TargetableTexture->GetClearBinding() == DefaultColorClear);
			const bool bMobileMultiViewDepthValid = (!MobileMultiViewSceneDepthZ || MobileMultiViewSceneDepthZ->GetRenderTargetItem().TargetableTexture->GetClearBinding() == DefaultDepthClear);
			return bColorValid && bDepthValid && bMobileMultiViewColorValid && bMobileMultiViewDepthValid;
#else
			return bColorValid && bDepthValid;
#endif
		}
	default:
		{
			return true;
		}
	}
}

bool FSceneRenderTargets::AreShadingPathRenderTargetsAllocated(ESceneColorFormatType InSceneColorFormatType) const
{
	switch (InSceneColorFormatType)
	{
	case ESceneColorFormatType::Mobile:
		{
			return (SceneColor[(int32)ESceneColorFormatType::Mobile] != nullptr);
		}
	case ESceneColorFormatType::HighEndWithAlpha:
		{
			return (SceneColor[(int32)ESceneColorFormatType::HighEndWithAlpha] != nullptr);
		}
	case ESceneColorFormatType::HighEnd:
		{
			return (SceneColor[(int32)ESceneColorFormatType::HighEnd] != nullptr);
		}
	default:
		{
			checkNoEntry();
			return false;
		}
	}
}

/*-----------------------------------------------------------------------------
FSceneTextureShaderParameters
-----------------------------------------------------------------------------*/

//
void FSceneTextureShaderParameters::Bind(const FShaderParameterMap& ParameterMap)
{
	// only used if Material has an expression that requires SceneColorTexture
	SceneColorTextureParameter.Bind(ParameterMap,TEXT("SceneColorTexture"));
	SceneColorTextureParameterSampler.Bind(ParameterMap,TEXT("SceneColorTextureSampler"));
	// only used if Material has an expression that requires SceneDepthTexture
	SceneDepthTextureParameter.Bind(ParameterMap,TEXT("SceneDepthTexture"));
	SceneDepthTextureParameterSampler.Bind(ParameterMap,TEXT("SceneDepthTextureSampler"));
	// Only used if Material has an expression that requires SceneAlphaCopyTexture
	SceneAlphaCopyTextureParameter.Bind(ParameterMap,TEXT("SceneAlphaCopyTexture"));
	SceneAlphaCopyTextureParameterSampler.Bind(ParameterMap,TEXT("SceneAlphaCopyTextureSampler"));
	//
	SceneDepthTextureNonMS.Bind(ParameterMap,TEXT("SceneDepthTextureNonMS"));
	SceneColorSurfaceParameter.Bind(ParameterMap,TEXT("SceneColorSurface"));
	// only used if Material has an expression that requires SceneColorTextureMSAA
	SceneDepthSurfaceParameter.Bind(ParameterMap,TEXT("SceneDepthSurface"));
	DirectionalOcclusionSampler.Bind(ParameterMap, TEXT("DirectionalOcclusionSampler"));
	DirectionalOcclusionTexture.Bind(ParameterMap, TEXT("DirectionalOcclusionTexture"));
	//
	MobileCustomStencilTexture.Bind(ParameterMap, TEXT("MobileCustomStencilTexture"));
	MobileCustomStencilTextureSampler.Bind(ParameterMap, TEXT("MobileCustomStencilTextureSampler"));
	//
	SceneStencilTextureParameter.Bind(ParameterMap,TEXT("SceneStencilTexture"));
}

template< typename ShaderRHIParamRef, typename TRHICmdList >
void FSceneTextureShaderParameters::Set(
	TRHICmdList& RHICmdList,
	const ShaderRHIParamRef& ShaderRHI,
	const FSceneView& View,
	ESceneRenderTargetsMode::Type TextureMode,
	ESamplerFilter ColorFilter ) const
{
	if (TextureMode == ESceneRenderTargetsMode::SetTextures)
	{
		FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

		// optimization possible: TShaderRHIParamRef is no param Ref
		if (SceneColorTextureParameter.IsBound())
		{
			FSamplerStateRHIRef Filter;
			switch ( ColorFilter )
			{
			case SF_Bilinear:
				Filter = TStaticSamplerState<SF_Bilinear,AM_Clamp,AM_Clamp,AM_Clamp>::GetRHI();
				break;
			case SF_Trilinear:
				Filter = TStaticSamplerState<SF_Trilinear,AM_Clamp,AM_Clamp,AM_Clamp>::GetRHI();
				break;
			case SF_AnisotropicPoint:
				Filter = TStaticSamplerState<SF_AnisotropicPoint,AM_Clamp,AM_Clamp,AM_Clamp>::GetRHI();
				break;
			case SF_AnisotropicLinear:
				Filter = TStaticSamplerState<SF_AnisotropicLinear,AM_Clamp,AM_Clamp,AM_Clamp>::GetRHI();
				break;
			case SF_Point:
			default:
				Filter = TStaticSamplerState<SF_Point,AM_Clamp,AM_Clamp,AM_Clamp>::GetRHI();
				break;
			}

			SetTextureParameter(
				RHICmdList, 
				ShaderRHI,
				SceneColorTextureParameter,
				SceneColorTextureParameterSampler,
				Filter,
				SceneContext.GetSceneColorTexture()
				);
		}

		if (SceneAlphaCopyTextureParameter.IsBound() && SceneContext.HasSceneAlphaCopyTexture())
		{
			FSamplerStateRHIRef Filter;
			Filter = TStaticSamplerState<SF_Point,AM_Clamp,AM_Clamp,AM_Clamp>::GetRHI();
			SetTextureParameter(
				RHICmdList, 
				ShaderRHI,
				SceneAlphaCopyTextureParameter,
				SceneAlphaCopyTextureParameterSampler,
				Filter,
				SceneContext.GetSceneAlphaCopyTexture()
				);
		}

		if(SceneDepthTextureParameter.IsBound() || SceneDepthTextureParameterSampler.IsBound())
		{
			const FTexture2DRHIRef* DepthTexture = SceneContext.GetActualDepthTexture();

			if (SceneContext.IsSeparateTranslucencyPass() && SceneContext.IsSeparateTranslucencyDepthValid())
			{
				FIntPoint OutScaledSize;
				float OutScale;
				SceneContext.GetSeparateTranslucencyDimensions(OutScaledSize, OutScale);

				if (OutScale < 1.0f)
				{
					DepthTexture = &SceneContext.GetSeparateTranslucencyDepthSurface();
				}
			}

			SetTextureParameter(
				RHICmdList, 
				ShaderRHI,
				SceneDepthTextureParameter,
				SceneDepthTextureParameterSampler,
				TStaticSamplerState<SF_Point,AM_Clamp,AM_Clamp,AM_Clamp>::GetRHI(),
				*DepthTexture
				);
		}

		const auto FeatureLevel = View.GetFeatureLevel();

		if (FeatureLevel >= ERHIFeatureLevel::SM5)
		{
			SetTextureParameter(RHICmdList, ShaderRHI, SceneColorSurfaceParameter, SceneContext.GetSceneColorSurface());
		}
		if (FeatureLevel >= ERHIFeatureLevel::SM4)
		{
			if(GSupportsDepthFetchDuringDepthTest)
			{
				if(SceneDepthSurfaceParameter.IsBound())
				{
					SetTextureParameter(RHICmdList, ShaderRHI, SceneDepthSurfaceParameter, SceneContext.GetSceneDepthSurface());
				}
				if(SceneDepthTextureNonMS.IsBound())
				{
					SetTextureParameter(RHICmdList, ShaderRHI, SceneDepthTextureNonMS, SceneContext.GetSceneDepthTexture());
				}
			}
			else
			{
				if(SceneDepthSurfaceParameter.IsBound())
				{
					SetTextureParameter(RHICmdList, ShaderRHI, SceneDepthSurfaceParameter, SceneContext.GetAuxiliarySceneDepthSurface());
				}
				if(SceneDepthTextureNonMS.IsBound())
				{
					SetTextureParameter(RHICmdList, ShaderRHI, SceneDepthTextureNonMS, SceneContext.GetAuxiliarySceneDepthSurface());
				}
			}

			if (SceneStencilTextureParameter.IsBound())
			{
				if (SceneContext.SceneStencilSRV.GetReference())
				{
					SetSRVParameter(RHICmdList, ShaderRHI, SceneStencilTextureParameter, SceneContext.SceneStencilSRV);
				}
				else
				{
					SetTextureParameter(RHICmdList, ShaderRHI, SceneStencilTextureParameter, GSystemTextures.BlackDummy->GetRenderTargetItem().ShaderResourceTexture);
				}
			}
		}

		if (FeatureLevel <= ERHIFeatureLevel::ES3_1)
		{
			if (MobileCustomStencilTexture.IsBound() && SceneContext.MobileCustomStencil.IsValid())
			{
				SetTextureParameter(
					RHICmdList, 
					ShaderRHI,
					MobileCustomStencilTexture,
					MobileCustomStencilTextureSampler,
					TStaticSamplerState<SF_Point,AM_Clamp,AM_Clamp,AM_Clamp>::GetRHI(),
					SceneContext.MobileCustomStencil->GetRenderTargetItem().ShaderResourceTexture
					);
			}
		}
	}
	else if (TextureMode == ESceneRenderTargetsMode::DontSet)
	{
		// Verify that none of these were bound if we were told not to set them
		ensure(!SceneColorTextureParameter.IsBound()
			&& !SceneDepthTextureParameter.IsBound()
			&& !SceneColorSurfaceParameter.IsBound()
			&& !SceneDepthSurfaceParameter.IsBound()
			&& !SceneDepthTextureNonMS.IsBound()
			&& !SceneStencilTextureParameter.IsBound());
	}
	else if (TextureMode == ESceneRenderTargetsMode::DontSetIgnoreBoundByEditorCompositing)
	{
		// Verify that none of these were bound if we were told not to set them
		// ignore SceneDepthTextureNonMS
		ensure(!SceneColorTextureParameter.IsBound()
			&& !SceneDepthTextureParameter.IsBound()
			&& !SceneColorSurfaceParameter.IsBound()
			&& !SceneDepthSurfaceParameter.IsBound()
			&& !SceneStencilTextureParameter.IsBound());
	}
	else if (TextureMode == ESceneRenderTargetsMode::InvalidScene)
	{
		FTextureRHIParamRef BlackDefault2D = GSystemTextures.BlackDummy->GetRenderTargetItem().ShaderResourceTexture;
		FTextureRHIParamRef DepthDefault = GSystemTextures.DepthDummy->GetRenderTargetItem().ShaderResourceTexture;
		FSamplerStateRHIRef Filter = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

		if (SceneColorTextureParameter.IsBound())
		{
			
			
			SetTextureParameter(
				RHICmdList,
				ShaderRHI,
				SceneColorTextureParameter,
				SceneColorTextureParameterSampler,
				Filter,
				BlackDefault2D
				);
		}

		if (SceneAlphaCopyTextureParameter.IsBound())
		{			
			SetTextureParameter(
				RHICmdList,
				ShaderRHI,
				SceneAlphaCopyTextureParameter,
				SceneAlphaCopyTextureParameterSampler,
				Filter,
				BlackDefault2D
				);
		}

		if (SceneDepthTextureParameter.IsBound() || SceneDepthTextureParameterSampler.IsBound())
		{
			SetTextureParameter(
				RHICmdList,
				ShaderRHI,
				SceneDepthTextureParameter,
				SceneDepthTextureParameterSampler,
				Filter,
				DepthDefault
				);
		}

		const auto FeatureLevel = View.GetFeatureLevel();

		if (FeatureLevel >= ERHIFeatureLevel::SM5)
		{
			SetTextureParameter(RHICmdList, ShaderRHI, SceneColorSurfaceParameter, BlackDefault2D);
		}
		if (FeatureLevel >= ERHIFeatureLevel::SM4)
		{
			if (SceneDepthSurfaceParameter.IsBound())
			{
				SetTextureParameter(RHICmdList, ShaderRHI, SceneDepthSurfaceParameter, DepthDefault);
			}
			if (SceneDepthTextureNonMS.IsBound())
			{
				SetTextureParameter(RHICmdList, ShaderRHI, SceneDepthTextureNonMS, DepthDefault);
			}
			if (SceneStencilTextureParameter.IsBound())
			{
				SetTextureParameter(RHICmdList, ShaderRHI, SceneStencilTextureParameter, BlackDefault2D);
			}
		}

		if (FeatureLevel <= ERHIFeatureLevel::ES3_1)
		{
			if (MobileCustomStencilTexture.IsBound())
			{
				SetTextureParameter(
					RHICmdList,
					ShaderRHI,
					MobileCustomStencilTexture,
					MobileCustomStencilTextureSampler,
					Filter,
					BlackDefault2D
					);
			}
		}
	}

	if( DirectionalOcclusionSampler.IsBound() )
	{
		bool bDirectionalOcclusion = false;
		FSceneViewState* ViewState = (FSceneViewState*)View.State;
		if (ViewState != nullptr)
		{
			FLightPropagationVolume* Lpv = ViewState->GetLightPropagationVolume(View.GetFeatureLevel());
		    if(Lpv != nullptr)
		    {
			    bDirectionalOcclusion = Lpv->IsDirectionalOcclusionEnabled(); 
		    }
		}

		FTextureRHIParamRef DirectionalOcclusion = nullptr;
		if( bDirectionalOcclusion )
		{
			FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);
			DirectionalOcclusion = SceneContext.GetDirectionalOcclusionTexture();
		}
		else
		{
			DirectionalOcclusion = GWhiteTexture->TextureRHI;
		}

		FSamplerStateRHIRef Filter;
		Filter = TStaticSamplerState<SF_Bilinear,AM_Clamp,AM_Clamp,AM_Clamp>::GetRHI();
		SetTextureParameter(
			RHICmdList,
			ShaderRHI,
			DirectionalOcclusionTexture,
			DirectionalOcclusionSampler,
			Filter,
			DirectionalOcclusion
			);
	}
}

#define IMPLEMENT_SCENE_TEXTURE_PARAM_SET( ShaderRHIParamRef ) \
	template void FSceneTextureShaderParameters::Set< ShaderRHIParamRef >( \
		FRHICommandList& RHICmdList,				\
		const ShaderRHIParamRef& ShaderRHI,			\
		const FSceneView& View,						\
		ESceneRenderTargetsMode::Type TextureMode,	\
		ESamplerFilter ColorFilter					\
	) const;

IMPLEMENT_SCENE_TEXTURE_PARAM_SET( FVertexShaderRHIParamRef );
IMPLEMENT_SCENE_TEXTURE_PARAM_SET( FHullShaderRHIParamRef );
IMPLEMENT_SCENE_TEXTURE_PARAM_SET( FDomainShaderRHIParamRef );
IMPLEMENT_SCENE_TEXTURE_PARAM_SET( FGeometryShaderRHIParamRef );
IMPLEMENT_SCENE_TEXTURE_PARAM_SET( FPixelShaderRHIParamRef );
IMPLEMENT_SCENE_TEXTURE_PARAM_SET( FComputeShaderRHIParamRef );

FArchive& operator<<(FArchive& Ar,FSceneTextureShaderParameters& Parameters)
{
	Ar << Parameters.SceneColorTextureParameter;
	Ar << Parameters.SceneColorTextureParameterSampler;
	Ar << Parameters.SceneAlphaCopyTextureParameter;
	Ar << Parameters.SceneAlphaCopyTextureParameterSampler;
	Ar << Parameters.SceneColorSurfaceParameter;
	Ar << Parameters.SceneDepthTextureParameter;
	Ar << Parameters.SceneDepthTextureParameterSampler;
	Ar << Parameters.SceneDepthSurfaceParameter;
	Ar << Parameters.SceneDepthTextureNonMS;
	Ar << Parameters.DirectionalOcclusionSampler;
	Ar << Parameters.DirectionalOcclusionTexture;
	Ar << Parameters.MobileCustomStencilTexture;
	Ar << Parameters.MobileCustomStencilTextureSampler;
	Ar << Parameters.SceneStencilTextureParameter;
	return Ar;
}

// Note this is not just for Deferred rendering, it also applies to mobile rendering.
void FDeferredPixelShaderParameters::Bind(const FShaderParameterMap& ParameterMap)
{
	SceneTextureParameters.Bind(ParameterMap);
	
	GBufferResources.Bind(ParameterMap,TEXT("GBuffers"));
	DBufferATextureMS.Bind(ParameterMap,TEXT("DBufferATextureMS"));
	DBufferBTextureMS.Bind(ParameterMap,TEXT("DBufferBTextureMS"));
	DBufferCTextureMS.Bind(ParameterMap,TEXT("DBufferCTextureMS"));
	ScreenSpaceAOTextureMS.Bind(ParameterMap,TEXT("ScreenSpaceAOTextureMS"));
	DBufferATextureNonMS.Bind(ParameterMap,TEXT("DBufferATextureNonMS"));
	DBufferBTextureNonMS.Bind(ParameterMap,TEXT("DBufferBTextureNonMS"));
	DBufferCTextureNonMS.Bind(ParameterMap,TEXT("DBufferCTextureNonMS"));
	ScreenSpaceAOTextureNonMS.Bind(ParameterMap,TEXT("ScreenSpaceAOTextureNonMS"));
	CustomDepthTextureNonMS.Bind(ParameterMap,TEXT("CustomDepthTextureNonMS"));
	DBufferATexture.Bind(ParameterMap,TEXT("DBufferATexture"));
	DBufferATextureSampler.Bind(ParameterMap,TEXT("DBufferATextureSampler"));
	DBufferBTexture.Bind(ParameterMap,TEXT("DBufferBTexture"));
	DBufferBTextureSampler.Bind(ParameterMap,TEXT("DBufferBTextureSampler"));
	DBufferCTexture.Bind(ParameterMap,TEXT("DBufferCTexture"));
	DBufferCTextureSampler.Bind(ParameterMap,TEXT("DBufferCTextureSampler"));
	ScreenSpaceAOTexture.Bind(ParameterMap,TEXT("ScreenSpaceAOTexture"));
	ScreenSpaceAOTextureSampler.Bind(ParameterMap,TEXT("ScreenSpaceAOTextureSampler"));
	CustomDepthTexture.Bind(ParameterMap,TEXT("CustomDepthTexture"));
	CustomDepthTextureSampler.Bind(ParameterMap,TEXT("CustomDepthTextureSampler"));
	CustomStencilTexture.Bind(ParameterMap,TEXT("CustomStencilTexture"));
	DBufferRenderMask.Bind(ParameterMap, TEXT("DBufferMask"));
}

bool IsDBufferEnabled();

template< typename ShaderRHIParamRef, typename TRHICmdList >
void FDeferredPixelShaderParameters::Set(TRHICmdList& RHICmdList, const ShaderRHIParamRef ShaderRHI, const FSceneView& View, ESceneRenderTargetsMode::Type TextureMode) const
{
	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);
	// This is needed on PC ES2 for SceneAlphaCopy, probably should be refactored for performance.
	SceneTextureParameters.Set(RHICmdList, ShaderRHI, View, TextureMode, SF_Point);

	// if() is purely an optimization and could be removed
	if (IsDBufferEnabled())
	{
		IPooledRenderTarget* DBufferA = SceneContext.DBufferA ? SceneContext.DBufferA : GSystemTextures.BlackAlphaOneDummy;
		IPooledRenderTarget* DBufferB = SceneContext.DBufferB ? SceneContext.DBufferB : GSystemTextures.MidGrayDummy;
		IPooledRenderTarget* DBufferC = SceneContext.DBufferC ? SceneContext.DBufferC : GSystemTextures.GreenDummy;

		// todo: optimize out when not needed
		SetTextureParameter(RHICmdList, ShaderRHI, DBufferATexture, DBufferATextureSampler, TStaticSamplerState<>::GetRHI(), DBufferA->GetRenderTargetItem().ShaderResourceTexture);
		SetTextureParameter(RHICmdList, ShaderRHI, DBufferBTexture, DBufferBTextureSampler, TStaticSamplerState<>::GetRHI(), DBufferB->GetRenderTargetItem().ShaderResourceTexture);
		SetTextureParameter(RHICmdList, ShaderRHI, DBufferCTexture, DBufferCTextureSampler, TStaticSamplerState<>::GetRHI(), DBufferC->GetRenderTargetItem().ShaderResourceTexture);
		 
		if (GSupportsRenderTargetWriteMask)
		{
			if (SceneContext.DBufferMask)
			{
				SetTextureParameter(RHICmdList, ShaderRHI, DBufferRenderMask, SceneContext.DBufferMask->GetRenderTargetItem().TargetableTexture);
			}
			else
			{
				SetTextureParameter(RHICmdList, ShaderRHI, DBufferRenderMask, GSystemTextures.WhiteDummy->GetRenderTargetItem().TargetableTexture);
			}
		}

		SetTextureParameter(RHICmdList, ShaderRHI, DBufferATextureMS, DBufferA->GetRenderTargetItem().TargetableTexture);
		SetTextureParameter(RHICmdList, ShaderRHI, DBufferBTextureMS, DBufferB->GetRenderTargetItem().TargetableTexture);
		SetTextureParameter(RHICmdList, ShaderRHI, DBufferCTextureMS, DBufferC->GetRenderTargetItem().TargetableTexture);
		SetTextureParameter(RHICmdList, ShaderRHI, DBufferATextureNonMS, DBufferA->GetRenderTargetItem().ShaderResourceTexture);
		SetTextureParameter(RHICmdList, ShaderRHI, DBufferBTextureNonMS, DBufferB->GetRenderTargetItem().ShaderResourceTexture);
		SetTextureParameter(RHICmdList, ShaderRHI, DBufferCTextureNonMS, DBufferC->GetRenderTargetItem().ShaderResourceTexture);
	}

	const auto FeatureLevel = View.GetFeatureLevel();

	if (TextureMode == ESceneRenderTargetsMode::SetTextures)
	{
		// if there is no ambient occlusion it's better to have white there
		IPooledRenderTarget* ScreenSpaceAO = SceneContext.ScreenSpaceAO;
		if(!SceneContext.bScreenSpaceAOIsValid)
		{
			ScreenSpaceAO = GSystemTextures.WhiteDummy;
		}

		// if there is no custom depth it's better to have the far distance there
		IPooledRenderTarget* CustomDepth = SceneContext.bCustomDepthIsValid ? SceneContext.CustomDepth.GetReference() : 0;
		if(!CustomDepth)
		{
			CustomDepth = GSystemTextures.BlackDummy;
		}
		SetTextureParameter(RHICmdList, ShaderRHI, CustomDepthTexture, CustomDepthTextureSampler, TStaticSamplerState<>::GetRHI(), CustomDepth->GetRenderTargetItem().ShaderResourceTexture);

		if (FeatureLevel >= ERHIFeatureLevel::SM4)
		{
			if (GBufferResources.IsBound())
			{
				SetUniformBufferParameter(RHICmdList, ShaderRHI, GBufferResources, SceneContext.GetGBufferResourcesUniformBuffer());
			}

			SetTextureParameter(RHICmdList, ShaderRHI, ScreenSpaceAOTexture, ScreenSpaceAOTextureSampler, TStaticSamplerState<>::GetRHI(), ScreenSpaceAO->GetRenderTargetItem().ShaderResourceTexture);
			SetTextureParameter(RHICmdList, ShaderRHI, ScreenSpaceAOTextureMS, ScreenSpaceAO->GetRenderTargetItem().TargetableTexture);
			SetTextureParameter(RHICmdList, ShaderRHI, ScreenSpaceAOTextureNonMS, ScreenSpaceAO->GetRenderTargetItem().ShaderResourceTexture);

			SetTextureParameter(RHICmdList, ShaderRHI, CustomDepthTextureNonMS, CustomDepth->GetRenderTargetItem().ShaderResourceTexture);

			if (CustomStencilTexture.IsBound())
			{
				if (SceneContext.bCustomDepthIsValid && SceneContext.CustomStencilSRV.GetReference())
				{
					SetSRVParameter(RHICmdList, ShaderRHI, CustomStencilTexture, SceneContext.CustomStencilSRV);
				}
				else
				{
					SetTextureParameter(RHICmdList, ShaderRHI, CustomStencilTexture, GSystemTextures.BlackDummy->GetRenderTargetItem().ShaderResourceTexture);
				}
			}
		}
	}
	else if (TextureMode == ESceneRenderTargetsMode::DontSet ||
		TextureMode == ESceneRenderTargetsMode::DontSetIgnoreBoundByEditorCompositing)
	{
		// Verify that none of these are actually bound
		checkSlow(!GBufferResources.IsBound());
	}
	else if (TextureMode == ESceneRenderTargetsMode::InvalidScene)
	{
		FTextureRHIParamRef BlackDefault2D = GSystemTextures.BlackDummy->GetRenderTargetItem().ShaderResourceTexture;
		FTextureRHIParamRef WhiteDefault2D = GSystemTextures.WhiteDummy->GetRenderTargetItem().ShaderResourceTexture;
		FTextureRHIParamRef DepthDefault = GSystemTextures.DepthDummy->GetRenderTargetItem().ShaderResourceTexture;
		
		SetTextureParameter(RHICmdList, ShaderRHI, CustomDepthTexture, CustomDepthTextureSampler, TStaticSamplerState<>::GetRHI(), DepthDefault);

		if (FeatureLevel >= ERHIFeatureLevel::SM4)
		{
			if (GBufferResources.IsBound())
			{
				SetUniformBufferParameter(RHICmdList, ShaderRHI, GBufferResources, SceneContext.GetDummyGBufferResourcesUniformBuffer());
			}

			SetTextureParameter(RHICmdList, ShaderRHI, ScreenSpaceAOTexture, ScreenSpaceAOTextureSampler, TStaticSamplerState<>::GetRHI(), WhiteDefault2D);
			SetTextureParameter(RHICmdList, ShaderRHI, ScreenSpaceAOTextureMS, WhiteDefault2D);
			SetTextureParameter(RHICmdList, ShaderRHI, ScreenSpaceAOTextureNonMS, WhiteDefault2D);

			SetTextureParameter(RHICmdList, ShaderRHI, CustomDepthTextureNonMS, DepthDefault);

			if (CustomStencilTexture.IsBound())
			{
				SetTextureParameter(RHICmdList, ShaderRHI, CustomStencilTexture, BlackDefault2D);
			}
		}
	}
}

#define IMPLEMENT_DEFERRED_PARAMETERS_SET( ShaderRHIParamRef, TRHICmdList ) \
	template void FDeferredPixelShaderParameters::Set< ShaderRHIParamRef, TRHICmdList >(\
		TRHICmdList& RHICmdList,				\
		const ShaderRHIParamRef ShaderRHI,			\
		const FSceneView& View,						\
		ESceneRenderTargetsMode::Type TextureMode	\
	) const;

IMPLEMENT_DEFERRED_PARAMETERS_SET( FVertexShaderRHIParamRef, FRHICommandList );
IMPLEMENT_DEFERRED_PARAMETERS_SET( FHullShaderRHIParamRef, FRHICommandList );
IMPLEMENT_DEFERRED_PARAMETERS_SET( FDomainShaderRHIParamRef, FRHICommandList );
IMPLEMENT_DEFERRED_PARAMETERS_SET( FGeometryShaderRHIParamRef, FRHICommandList );
IMPLEMENT_DEFERRED_PARAMETERS_SET( FPixelShaderRHIParamRef, FRHICommandList );
IMPLEMENT_DEFERRED_PARAMETERS_SET( FComputeShaderRHIParamRef, FRHICommandList );

IMPLEMENT_DEFERRED_PARAMETERS_SET(FVertexShaderRHIParamRef, FRHICommandListImmediate);
IMPLEMENT_DEFERRED_PARAMETERS_SET(FHullShaderRHIParamRef, FRHICommandListImmediate);
IMPLEMENT_DEFERRED_PARAMETERS_SET(FDomainShaderRHIParamRef, FRHICommandListImmediate);
IMPLEMENT_DEFERRED_PARAMETERS_SET(FGeometryShaderRHIParamRef, FRHICommandListImmediate);
IMPLEMENT_DEFERRED_PARAMETERS_SET(FPixelShaderRHIParamRef, FRHICommandListImmediate);
IMPLEMENT_DEFERRED_PARAMETERS_SET(FComputeShaderRHIParamRef, FRHICommandListImmediate);
IMPLEMENT_DEFERRED_PARAMETERS_SET(FComputeShaderRHIParamRef, FRHIAsyncComputeCommandListImmediate );

FArchive& operator<<(FArchive& Ar,FDeferredPixelShaderParameters& Parameters)
{
	Ar << Parameters.SceneTextureParameters;

	Ar << Parameters.GBufferResources;
	Ar << Parameters.DBufferATextureMS;
	Ar << Parameters.DBufferBTextureMS;
	Ar << Parameters.DBufferCTextureMS;
	Ar << Parameters.ScreenSpaceAOTextureMS;
	Ar << Parameters.DBufferATextureNonMS;
	Ar << Parameters.DBufferBTextureNonMS;
	Ar << Parameters.DBufferCTextureNonMS;
	Ar << Parameters.ScreenSpaceAOTextureNonMS;
	Ar << Parameters.CustomDepthTextureNonMS;
	Ar << Parameters.DBufferATexture;
	Ar << Parameters.DBufferRenderMask;
	Ar << Parameters.DBufferATextureSampler;
	Ar << Parameters.DBufferBTexture;
	Ar << Parameters.DBufferBTextureSampler;
	Ar << Parameters.DBufferCTexture;
	Ar << Parameters.DBufferCTextureSampler;
	Ar << Parameters.ScreenSpaceAOTexture;
	Ar << Parameters.ScreenSpaceAOTextureSampler;
	Ar << Parameters.CustomDepthTexture;
	Ar << Parameters.CustomDepthTextureSampler;
	Ar << Parameters.CustomStencilTexture;

	return Ar;
}
