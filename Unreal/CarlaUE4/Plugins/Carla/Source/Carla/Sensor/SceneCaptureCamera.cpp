// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "Carla.h"
#include "SceneCaptureCamera.h"

#include "Sensor/SensorDataView.h"

#include "Components/DrawFrustumComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HighResScreenshot.h"
#include "Materials/Material.h"
#include "Paths.h"
#include "StaticMeshResources.h"
#include "TextureResource.h"

static constexpr auto DEPTH_MAT_PATH =
#if PLATFORM_LINUX
    TEXT("Material'/Carla/PostProcessingMaterials/DepthEffectMaterial_GLSL.DepthEffectMaterial_GLSL'");
#elif PLATFORM_WINDOWS
    TEXT("Material'/Carla/PostProcessingMaterials/DepthEffectMaterial.DepthEffectMaterial'");
#else
#  error No depth material defined for this platform
#endif

static constexpr auto SEMANTIC_SEGMENTATION_MAT_PATH =
    TEXT("Material'/Carla/PostProcessingMaterials/GTMaterial.GTMaterial'");

static void RemoveShowFlags(FEngineShowFlags &ShowFlags);

ASceneCaptureCamera::ASceneCaptureCamera(const FObjectInitializer& ObjectInitializer) :
  Super(ObjectInitializer),
  SizeX(720u),
  SizeY(512u),
  PostProcessEffect(EPostProcessEffect::SceneFinal)
{
  PrimaryActorTick.bCanEverTick = true;
  PrimaryActorTick.TickGroup = TG_PrePhysics;

  MeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("CamMesh0"));

  MeshComp->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);

  MeshComp->bHiddenInGame = true;
  MeshComp->CastShadow = false;
  MeshComp->PostPhysicsComponentTick.bCanEverTick = false;
  RootComponent = MeshComp;

  DrawFrustum = CreateDefaultSubobject<UDrawFrustumComponent>(TEXT("DrawFrust0"));
  DrawFrustum->bIsEditorOnly = true;
  DrawFrustum->SetupAttachment(MeshComp);

  CaptureRenderTarget = CreateDefaultSubobject<UTextureRenderTarget2D>(TEXT("CaptureRenderTarget0"));

  CaptureComponent2D = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("SceneCaptureComponent2D"));
  CaptureComponent2D->SetupAttachment(MeshComp);

  // Load post-processing materials.
  static ConstructorHelpers::FObjectFinder<UMaterial> DEPTH(DEPTH_MAT_PATH);
  PostProcessDepth = DEPTH.Object;
  static ConstructorHelpers::FObjectFinder<UMaterial> SEMANTIC_SEGMENTATION(SEMANTIC_SEGMENTATION_MAT_PATH);
  PostProcessSemanticSegmentation = SEMANTIC_SEGMENTATION.Object;
}

void ASceneCaptureCamera::PostActorCreated()
{
  Super::PostActorCreated();

  // no need load the editor mesh when there is no editor
#if WITH_EDITOR
  if(MeshComp)
  {
    if (!IsRunningCommandlet())
    {
      if( !MeshComp->GetStaticMesh())
      {
        UStaticMesh* CamMesh = LoadObject<UStaticMesh>(NULL, TEXT("/Engine/EditorMeshes/MatineeCam_SM.MatineeCam_SM"), NULL, LOAD_None, NULL);
        MeshComp->SetStaticMesh(CamMesh);
      }
    }
  }
#endif // WITH_EDITOR

  // Sync component with CameraActor frustum settings.
  UpdateDrawFrustum();
}

void ASceneCaptureCamera::BeginPlay()
{
  const bool bRemovePostProcessing = (PostProcessEffect != EPostProcessEffect::SceneFinal);

  // Setup render target.
  const bool bInForceLinearGamma = bRemovePostProcessing;
  CaptureRenderTarget->InitCustomFormat(SizeX, SizeY, PF_B8G8R8A8, bInForceLinearGamma);

  CaptureComponent2D->Deactivate();
  CaptureComponent2D->TextureTarget = CaptureRenderTarget;

  // Setup camera post-processing.
  if (PostProcessEffect != EPostProcessEffect::None) {
    CaptureComponent2D->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
  }
  if (bRemovePostProcessing) {
    RemoveShowFlags(CaptureComponent2D->ShowFlags);
  }
  if (PostProcessEffect == EPostProcessEffect::Depth) {
    CaptureComponent2D->PostProcessSettings.AddBlendable(PostProcessDepth, 1.0f);
  } else if (PostProcessEffect == EPostProcessEffect::SemanticSegmentation) {
    CaptureComponent2D->PostProcessSettings.AddBlendable(PostProcessSemanticSegmentation, 1.0f);
  }

  CaptureComponent2D->UpdateContent();
  CaptureComponent2D->Activate();

  Super::BeginPlay();
}

void ASceneCaptureCamera::Tick(const float DeltaSeconds)
{
  Super::Tick(DeltaSeconds);

  /// @todo This should be done on render thread.

  struct {
    uint32 Width;
    uint32 Height;
    uint32 Type;
    float FOV;
  } ImageHeader = {
    SizeX,
    SizeY,
    PostProcessEffect::ToUInt(PostProcessEffect),
    CaptureComponent2D->FOVAngle
  };

  static_assert(sizeof(ImageHeader) == 4u * sizeof(uint32), "Invalid header size");

  TArray<FColor> BitMap;
  if (ReadPixels(BitMap)) {
    FSensorDataView DataView(
        GetId(),
        FReadOnlyBufferView{reinterpret_cast<const void *>(&ImageHeader), sizeof(ImageHeader)},
        FReadOnlyBufferView{BitMap});

    WriteSensorData(DataView);
  }
}

float ASceneCaptureCamera::GetFOVAngle() const
{
  check(CaptureComponent2D != nullptr);
  return CaptureComponent2D->FOVAngle;
}

void ASceneCaptureCamera::SetImageSize(uint32 otherSizeX, uint32 otherSizeY)
{
  SizeX = otherSizeX;
  SizeY = otherSizeY;
}

void ASceneCaptureCamera::SetPostProcessEffect(EPostProcessEffect otherPostProcessEffect)
{
  PostProcessEffect = otherPostProcessEffect;
  auto &PostProcessSettings = CaptureComponent2D->PostProcessSettings;
  if (PostProcessEffect != EPostProcessEffect::SceneFinal) {
    PostProcessSettings.bOverride_AutoExposureMethod = false;
    PostProcessSettings.bOverride_AutoExposureMinBrightness = false;
    PostProcessSettings.bOverride_AutoExposureMaxBrightness = false;
    PostProcessSettings.bOverride_AutoExposureBias = false;
  }
}

void ASceneCaptureCamera::SetFOVAngle(const float FOVAngle)
{
  check(CaptureComponent2D != nullptr);
  CaptureComponent2D->FOVAngle = FOVAngle;
}

void ASceneCaptureCamera::SetTargetGamma(const float TargetGamma)
{
  check(CaptureRenderTarget != nullptr);
  CaptureRenderTarget->TargetGamma = TargetGamma;
}

void ASceneCaptureCamera::Set(const UCameraDescription &CameraDescription)
{
  Super::Set(CameraDescription);

  if (CameraDescription.bOverrideCameraPostProcessParameters) {
    auto &Override = CameraDescription.CameraPostProcessParameters;
    auto &PostProcessSettings = CaptureComponent2D->PostProcessSettings;
    PostProcessSettings.bOverride_AutoExposureMethod = true;
    PostProcessSettings.AutoExposureMethod = Override.AutoExposureMethod;
    PostProcessSettings.bOverride_AutoExposureMinBrightness = true;
    PostProcessSettings.AutoExposureMinBrightness = Override.AutoExposureMinBrightness;
    PostProcessSettings.bOverride_AutoExposureMaxBrightness = true;
    PostProcessSettings.AutoExposureMaxBrightness = Override.AutoExposureMaxBrightness;
    PostProcessSettings.bOverride_AutoExposureBias = true;
    PostProcessSettings.AutoExposureBias = Override.AutoExposureBias;
  }
  SetImageSize(CameraDescription.ImageSizeX, CameraDescription.ImageSizeY);
  SetPostProcessEffect(CameraDescription.PostProcessEffect);
  SetFOVAngle(CameraDescription.FOVAngle);
}

bool ASceneCaptureCamera::ReadPixels(TArray<FColor> &BitMap) const
{
  FTextureRenderTargetResource* RTResource = CaptureRenderTarget->GameThread_GetRenderTargetResource();
  if (RTResource == nullptr) {
    UE_LOG(LogCarla, Error, TEXT("SceneCaptureCamera: Missing render target"));
    return false;
  }
  FReadSurfaceDataFlags ReadPixelFlags(RCM_UNorm);
  ReadPixelFlags.SetLinearToGamma(true);
  return RTResource->ReadPixels(BitMap, ReadPixelFlags);
}

void ASceneCaptureCamera::UpdateDrawFrustum()
{
  if(DrawFrustum && CaptureComponent2D)
  {
    DrawFrustum->FrustumStartDist = GNearClippingPlane;

    // 1000 is the default frustum distance, ideally this would be infinite but that might cause rendering issues
    DrawFrustum->FrustumEndDist = (CaptureComponent2D->MaxViewDistanceOverride > DrawFrustum->FrustumStartDist)
      ? CaptureComponent2D->MaxViewDistanceOverride : 1000.0f;

    DrawFrustum->FrustumAngle = CaptureComponent2D->FOVAngle;
    //DrawFrustum->FrustumAspectRatio = CaptureComponent2D->AspectRatio;
  }
}

// Remove the show flags that might interfere with post-processing effects like
// depth and semantic segmentation.
static void RemoveShowFlags(FEngineShowFlags &ShowFlags)
{
  ShowFlags.SetAmbientOcclusion(false);
  ShowFlags.SetAntiAliasing(false);
  ShowFlags.SetAtmosphericFog(false);
  // ShowFlags.SetAudioRadius(false);
  // ShowFlags.SetBillboardSprites(false);
  ShowFlags.SetBloom(false);
  // ShowFlags.SetBounds(false);
  // ShowFlags.SetBrushes(false);
  // ShowFlags.SetBSP(false);
  // ShowFlags.SetBSPSplit(false);
  // ShowFlags.SetBSPTriangles(false);
  // ShowFlags.SetBuilderBrush(false);
  // ShowFlags.SetCameraAspectRatioBars(false);
  // ShowFlags.SetCameraFrustums(false);
  ShowFlags.SetCameraImperfections(false);
  ShowFlags.SetCameraInterpolation(false);
  // ShowFlags.SetCameraSafeFrames(false);
  // ShowFlags.SetCollision(false);
  // ShowFlags.SetCollisionPawn(false);
  // ShowFlags.SetCollisionVisibility(false);
  ShowFlags.SetColorGrading(false);
  // ShowFlags.SetCompositeEditorPrimitives(false);
  // ShowFlags.SetConstraints(false);
  // ShowFlags.SetCover(false);
  // ShowFlags.SetDebugAI(false);
  // ShowFlags.SetDecals(false);
  // ShowFlags.SetDeferredLighting(false);
  ShowFlags.SetDepthOfField(false);
  ShowFlags.SetDiffuse(false);
  ShowFlags.SetDirectionalLights(false);
  ShowFlags.SetDirectLighting(false);
  // ShowFlags.SetDistanceCulledPrimitives(false);
  // ShowFlags.SetDistanceFieldAO(false);
  // ShowFlags.SetDistanceFieldGI(false);
  ShowFlags.SetDynamicShadows(false);
  // ShowFlags.SetEditor(false);
  ShowFlags.SetEyeAdaptation(false);
  ShowFlags.SetFog(false);
  // ShowFlags.SetGame(false);
  // ShowFlags.SetGameplayDebug(false);
  // ShowFlags.SetGBufferHints(false);
  ShowFlags.SetGlobalIllumination(false);
  ShowFlags.SetGrain(false);
  // ShowFlags.SetGrid(false);
  // ShowFlags.SetHighResScreenshotMask(false);
  // ShowFlags.SetHitProxies(false);
  ShowFlags.SetHLODColoration(false);
  ShowFlags.SetHMDDistortion(false);
  // ShowFlags.SetIndirectLightingCache(false);
  // ShowFlags.SetInstancedFoliage(false);
  // ShowFlags.SetInstancedGrass(false);
  // ShowFlags.SetInstancedStaticMeshes(false);
  // ShowFlags.SetLandscape(false);
  // ShowFlags.SetLargeVertices(false);
  ShowFlags.SetLensFlares(false);
  ShowFlags.SetLevelColoration(false);
  ShowFlags.SetLightComplexity(false);
  ShowFlags.SetLightFunctions(false);
  ShowFlags.SetLightInfluences(false);
  ShowFlags.SetLighting(false);
  ShowFlags.SetLightMapDensity(false);
  ShowFlags.SetLightRadius(false);
  ShowFlags.SetLightShafts(false);
  // ShowFlags.SetLOD(false);
  ShowFlags.SetLODColoration(false);
  // ShowFlags.SetMaterials(false);
  // ShowFlags.SetMaterialTextureScaleAccuracy(false);
  // ShowFlags.SetMeshEdges(false);
  // ShowFlags.SetMeshUVDensityAccuracy(false);
  // ShowFlags.SetModeWidgets(false);
  ShowFlags.SetMotionBlur(false);
  // ShowFlags.SetNavigation(false);
  ShowFlags.SetOnScreenDebug(false);
  // ShowFlags.SetOutputMaterialTextureScales(false);
  // ShowFlags.SetOverrideDiffuseAndSpecular(false);
  // ShowFlags.SetPaper2DSprites(false);
  ShowFlags.SetParticles(false);
  // ShowFlags.SetPivot(false);
  ShowFlags.SetPointLights(false);
  // ShowFlags.SetPostProcessing(false);
  // ShowFlags.SetPostProcessMaterial(false);
  // ShowFlags.SetPrecomputedVisibility(false);
  // ShowFlags.SetPrecomputedVisibilityCells(false);
  // ShowFlags.SetPreviewShadowsIndicator(false);
  // ShowFlags.SetPrimitiveDistanceAccuracy(false);
  ShowFlags.SetPropertyColoration(false);
  // ShowFlags.SetQuadOverdraw(false);
  // ShowFlags.SetReflectionEnvironment(false);
  // ShowFlags.SetReflectionOverride(false);
  ShowFlags.SetRefraction(false);
  // ShowFlags.SetRendering(false);
  ShowFlags.SetSceneColorFringe(false);
  // ShowFlags.SetScreenPercentage(false);
  ShowFlags.SetScreenSpaceAO(false);
  ShowFlags.SetScreenSpaceReflections(false);
  // ShowFlags.SetSelection(false);
  // ShowFlags.SetSelectionOutline(false);
  // ShowFlags.SetSeparateTranslucency(false);
  // ShowFlags.SetShaderComplexity(false);
  // ShowFlags.SetShaderComplexityWithQuadOverdraw(false);
  // ShowFlags.SetShadowFrustums(false);
  // ShowFlags.SetSkeletalMeshes(false);
  // ShowFlags.SetSkinCache(false);
  ShowFlags.SetSkyLighting(false);
  // ShowFlags.SetSnap(false);
  // ShowFlags.SetSpecular(false);
  // ShowFlags.SetSplines(false);
  ShowFlags.SetSpotLights(false);
  // ShowFlags.SetStaticMeshes(false);
  ShowFlags.SetStationaryLightOverlap(false);
  // ShowFlags.SetStereoRendering(false);
  // ShowFlags.SetStreamingBounds(false);
  ShowFlags.SetSubsurfaceScattering(false);
  // ShowFlags.SetTemporalAA(false);
  // ShowFlags.SetTessellation(false);
  // ShowFlags.SetTestImage(false);
  // ShowFlags.SetTextRender(false);
  // ShowFlags.SetTexturedLightProfiles(false);
  ShowFlags.SetTonemapper(false);
  // ShowFlags.SetTranslucency(false);
  // ShowFlags.SetVectorFields(false);
  // ShowFlags.SetVertexColors(false);
  // ShowFlags.SetVignette(false);
  // ShowFlags.SetVisLog(false);
  ShowFlags.SetVisualizeAdaptiveDOF(false);
  ShowFlags.SetVisualizeBloom(false);
  ShowFlags.SetVisualizeBuffer(false);
  ShowFlags.SetVisualizeDistanceFieldAO(false);
  ShowFlags.SetVisualizeDistanceFieldGI(false);
  ShowFlags.SetVisualizeDOF(false);
  ShowFlags.SetVisualizeHDR(false);
  ShowFlags.SetVisualizeLightCulling(false);
  ShowFlags.SetVisualizeLPV(false);
  ShowFlags.SetVisualizeMeshDistanceFields(false);
  ShowFlags.SetVisualizeMotionBlur(false);
  ShowFlags.SetVisualizeOutOfBoundsPixels(false);
  ShowFlags.SetVisualizeSenses(false);
  ShowFlags.SetVisualizeShadingModels(false);
  ShowFlags.SetVisualizeSSR(false);
  ShowFlags.SetVisualizeSSS(false);
  // ShowFlags.SetVolumeLightingSamples(false);
  // ShowFlags.SetVolumes(false);
  // ShowFlags.SetWidgetComponents(false);
  // ShowFlags.SetWireframe(false);
}
