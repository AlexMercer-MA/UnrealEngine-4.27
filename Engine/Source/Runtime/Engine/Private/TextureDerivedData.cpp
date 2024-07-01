// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	TextureDerivedData.cpp: Derived data management for textures.
=============================================================================*/

#include "CoreMinimal.h"
#include "Misc/CommandLine.h"
#include "Stats/Stats.h"
#include "Async/AsyncWork.h"
#include "Serialization/MemoryWriter.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "Serialization/MemoryReader.h"
#include "UObject/Package.h"
#include "RenderUtils.h"
#include "TextureResource.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "Engine/Texture2DArray.h"
#include "DeviceProfiles/DeviceProfile.h"
#include "DeviceProfiles/DeviceProfileManager.h"
#include "TextureDerivedDataTask.h"
#include "Streaming/TextureStreamingHelpers.h"
#include "Engine/VolumeTexture.h"
#include "VT/VirtualTextureBuildSettings.h"
#include "VT/VirtualTextureBuiltData.h"
#include "HAL/FileManager.h"

#if WITH_EDITOR

#include "DerivedDataCacheInterface.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Interfaces/ITextureFormat.h"
#include "ProfilingDebugging/CookStats.h"
#include "VT/VirtualTextureDataBuilder.h"

/*------------------------------------------------------------------------------
	Versioning for texture derived data.
------------------------------------------------------------------------------*/

// The current version string is set up to mimic the old versioning scheme and to make
// sure the DDC does not get invalidated right now. If you need to bump the version, replace it
// with a guid ( ex.: TEXT("855EE5B3574C43ABACC6700C4ADC62E6") )
// In case of merge conflicts with DDC versions, you MUST generate a new GUID and set this new
// guid as version

#define TEXTURE_DERIVEDDATA_VER		TEXT("E01F4250BECD420DA9CE63B5EECEC2C2")

// This GUID is mixed into DDC version for virtual textures only, this allows updating DDC version for VT without invalidating DDC for all textures
// This is useful during development, but once large numbers of VT are present in shipped content, it will have the same problem as TEXTURE_DERIVEDDATA_VER
#define TEXTURE_VT_DERIVEDDATA_VER	TEXT("85E6EE5DBB194A38A8309D7F6FA4855A")

#if ENABLE_COOK_STATS
namespace TextureCookStats
{
	static FCookStats::FDDCResourceUsageStats UsageStats;
	static FCookStats::FDDCResourceUsageStats StreamingMipUsageStats;
	static FCookStatsManager::FAutoRegisterCallback RegisterCookStats([](FCookStatsManager::AddStatFuncRef AddStat)
	{
		UsageStats.LogStats(AddStat, TEXT("Texture.Usage"), TEXT("Inline"));
		StreamingMipUsageStats.LogStats(AddStat, TEXT("Texture.Usage"), TEXT("Streaming"));
	});
}
#endif

/*------------------------------------------------------------------------------
	Derived data key generation.
------------------------------------------------------------------------------*/

/**
 * Serialize build settings for use when generating the derived data key.
 */
static void SerializeForKey(FArchive& Ar, const FTextureBuildSettings& Settings)
{
	uint32 TempUint32;
	float TempFloat;
	uint8 TempByte;
	FColor TempColor;
	FVector4 TempVector4;

	TempFloat = Settings.ColorAdjustment.AdjustBrightness; Ar << TempFloat;
	TempFloat = Settings.ColorAdjustment.AdjustBrightnessCurve; Ar << TempFloat;
	TempFloat = Settings.ColorAdjustment.AdjustSaturation; Ar << TempFloat;
	TempFloat = Settings.ColorAdjustment.AdjustVibrance; Ar << TempFloat;
	TempFloat = Settings.ColorAdjustment.AdjustRGBCurve; Ar << TempFloat;
	TempFloat = Settings.ColorAdjustment.AdjustHue; Ar << TempFloat;
	TempFloat = Settings.ColorAdjustment.AdjustMinAlpha; Ar << TempFloat;
	TempFloat = Settings.ColorAdjustment.AdjustMaxAlpha; Ar << TempFloat;
	TempFloat = Settings.MipSharpening; Ar << TempFloat;
	TempUint32 = Settings.DiffuseConvolveMipLevel; Ar << TempUint32;
	TempUint32 = Settings.SharpenMipKernelSize; Ar << TempUint32;
	// NOTE: TextureFormatName is not stored in the key here.
	// NOTE: bHDRSource is not stored in the key here.
	TempByte = Settings.MipGenSettings; Ar << TempByte;
	TempByte = Settings.bCubemap; Ar << TempByte;
	TempByte = Settings.bTextureArray; Ar << TempByte;
	TempByte = Settings.bSRGB ? (Settings.bSRGB | ( Settings.bUseLegacyGamma ? 0 : 0x2 )) : 0; Ar << TempByte;
	TempByte = Settings.bPreserveBorder; Ar << TempByte;
	TempByte = Settings.bDitherMipMapAlpha; Ar << TempByte;

	if (Settings.AlphaCoverageThresholds != FVector4(0, 0, 0, 0))
	{
		TempVector4 = Settings.AlphaCoverageThresholds; Ar << TempVector4;
	}
	
	TempByte = Settings.bComputeBokehAlpha; Ar << TempByte;
	TempByte = Settings.bReplicateRed; Ar << TempByte;
	TempByte = Settings.bReplicateAlpha; Ar << TempByte;
	TempByte = Settings.bDownsampleWithAverage; Ar << TempByte;
	
	{
		TempByte = Settings.bSharpenWithoutColorShift;

		if(Settings.bSharpenWithoutColorShift && Settings.MipSharpening != 0.0f)
		{
			// bSharpenWithoutColorShift prevented alpha sharpening. This got fixed
			// Here we update the key to get those cases recooked.
			TempByte = 2;
		}

		Ar << TempByte;
	}

	TempByte = Settings.bBorderColorBlack; Ar << TempByte;
	TempByte = Settings.bFlipGreenChannel; Ar << TempByte;
	TempByte = Settings.bApplyKernelToTopMip; Ar << TempByte;
	TempByte = Settings.CompositeTextureMode; Ar << TempByte;
	TempFloat = Settings.CompositePower; Ar << TempFloat;
	TempUint32 = Settings.MaxTextureResolution; Ar << TempUint32;
	TempByte = Settings.PowerOfTwoMode; Ar << TempByte;
	TempColor = Settings.PaddingColor; Ar << TempColor;
	TempByte = Settings.bChromaKeyTexture; Ar << TempByte;
	TempColor = Settings.ChromaKeyColor; Ar << TempColor;
	TempFloat = Settings.ChromaKeyThreshold; Ar << TempFloat;
	
	// Avoid changing key for non-VT enabled textures
	if (Settings.bVirtualStreamable)
	{
		TempByte = Settings.bVirtualStreamable; Ar << TempByte;
		TempByte = Settings.VirtualAddressingModeX; Ar << TempByte;
		TempByte = Settings.VirtualAddressingModeY; Ar << TempByte;
		TempUint32 = Settings.VirtualTextureTileSize; Ar << TempUint32;
		TempUint32 = Settings.VirtualTextureBorderSize; Ar << TempUint32;
		TempByte = Settings.bVirtualTextureEnableCompressZlib; Ar << TempByte;
		TempByte = Settings.bVirtualTextureEnableCompressCrunch; Ar << TempByte;
		TempByte = Settings.LossyCompressionAmount; Ar << TempByte; // Lossy compression currently only used by VT
		TempByte = Settings.bApplyYCoCgBlockScale; Ar << TempByte; // YCoCg currently only used by VT
	}

	// Avoid changing key if texture is not being downscaled
	if (Settings.Downscale > 1.0)
	{
		TempFloat = Settings.Downscale; Ar << TempFloat;
		TempByte = Settings.DownscaleOptions; Ar << TempByte;
	}

	//Settings.LossyCompressionAmount
	//Settings.CompressionQuality

	//note : LossyCompressionAmount and CompressionQuality are not put in DDC key
	// it is up to textureformats that use them to put them in
}

/**
 * Computes the derived data key suffix for a texture with the specified compression settings.
 * @param Texture - The texture for which to compute the derived data key.
 * @param BuildSettings - Build settings for which to compute the derived data key.
 * @param OutKeySuffix - The derived data key suffix.
 */
 void GetTextureDerivedDataKeySuffix(const UTexture& Texture, const FTextureBuildSettings* BuildSettingsPerLayer, FString& OutKeySuffix)
{
	uint16 Version = 0;

	// Build settings for layer0 (used by default)
	const FTextureBuildSettings& BuildSettings = BuildSettingsPerLayer[0];

	// get the version for this texture's platform format
	ITargetPlatformManagerModule* TPM = GetTargetPlatformManager();
	const ITextureFormat* TextureFormat = NULL;
	if (TPM)
	{
		TextureFormat = TPM->FindTextureFormat(BuildSettings.TextureFormatName);
		if (TextureFormat)
		{
			Version = TextureFormat->GetVersion(BuildSettings.TextureFormatName, &BuildSettings);
		}
	}
	
	FString CompositeTextureStr;

	if(IsValid(Texture.CompositeTexture) && Texture.CompositeTextureMode != CTM_Disabled)
	{
		CompositeTextureStr += TEXT("_");
		CompositeTextureStr += Texture.CompositeTexture->Source.GetIdString();
	}

	// build the key, but don't use include the version if it's 0 to be backwards compatible
	OutKeySuffix = FString::Printf(TEXT("%s_%s%s%s_%02u_%s"),
		*BuildSettings.TextureFormatName.GetPlainNameString(),
		Version == 0 ? TEXT("") : *FString::Printf(TEXT("%d_"), Version),
		*Texture.Source.GetIdString(),
		*CompositeTextureStr,
		(uint32)NUM_INLINE_DERIVED_MIPS,
		(TextureFormat == NULL) ? TEXT("") : *TextureFormat->GetDerivedDataKeyString(Texture, &BuildSettings)
		);

	// Add key data for extra layers beyond the first
	const int32 NumLayers = Texture.Source.GetNumLayers();
	for (int32 LayerIndex = 1; LayerIndex < NumLayers; ++LayerIndex)
	{
		const FTextureBuildSettings& LayerBuildSettings = BuildSettingsPerLayer[LayerIndex];
		const ITextureFormat* LayerTextureFormat = NULL;
		if (TPM)
		{
			LayerTextureFormat = TPM->FindTextureFormat(LayerBuildSettings.TextureFormatName);
		}

		uint16 LayerVersion = 0;
		if (LayerTextureFormat)
		{
			LayerVersion = LayerTextureFormat->GetVersion(LayerBuildSettings.TextureFormatName, &LayerBuildSettings);
		}
		OutKeySuffix.Append(FString::Printf(TEXT("%s%d%s_"),
			*LayerBuildSettings.TextureFormatName.GetPlainNameString(),
			LayerVersion,
			(LayerTextureFormat == NULL) ? TEXT("") : *LayerTextureFormat->GetDerivedDataKeyString(Texture, &LayerBuildSettings)));
	}

	if (BuildSettings.bVirtualStreamable)
	{
		// Additional GUID for virtual textures, make it easier to force these to rebuild while developing
		OutKeySuffix.Append(FString::Printf(TEXT("VT%s_"), TEXTURE_VT_DERIVEDDATA_VER));
	}

	// Serialize the compressor settings into a temporary array. The archive
	// is flagged as persistent so that machines of different endianness produce
	// identical binary results.
	TArray<uint8> TempBytes; 
	TempBytes.Reserve(64);
	FMemoryWriter Ar(TempBytes, /*bIsPersistent=*/ true);
	SerializeForKey(Ar, BuildSettings);

	for (int32 LayerIndex = 1; LayerIndex < NumLayers; ++LayerIndex)
	{
		const FTextureBuildSettings& LayerBuildSettings = BuildSettingsPerLayer[LayerIndex];
		SerializeForKey(Ar, LayerBuildSettings);
	}

	// Now convert the raw bytes to a string.
	const uint8* SettingsAsBytes = TempBytes.GetData();
	OutKeySuffix.Reserve(OutKeySuffix.Len() + TempBytes.Num());
	for (int32 ByteIndex = 0; ByteIndex < TempBytes.Num(); ++ByteIndex)
	{
		ByteToHex(SettingsAsBytes[ByteIndex], OutKeySuffix);
	}
}

/**
 * Constructs a derived data key from the key suffix.
 * @param KeySuffix - The key suffix.
 * @param OutKey - The full derived data key.
 */
static void GetTextureDerivedDataKeyFromSuffix(const FString& KeySuffix, FString& OutKey)
{
	OutKey = FDerivedDataCacheInterface::BuildCacheKey(
		TEXT("TEXTURE"),
		TEXTURE_DERIVEDDATA_VER,
		*KeySuffix
		);
}

/**
 * Constructs the derived data key for an individual mip.
 * @param KeySuffix - The key suffix.
 * @param MipIndex - The mip index.
 * @param OutKey - The full derived data key for the mip.
 */
static void GetTextureDerivedMipKey(
	int32 MipIndex,
	const FTexture2DMipMap& Mip,
	const FString& KeySuffix,
	FString& OutKey
	)
{
	OutKey = FDerivedDataCacheInterface::BuildCacheKey(
		TEXT("TEXTURE"),
		TEXTURE_DERIVEDDATA_VER,
		*FString::Printf(TEXT("%s_MIP%u_%dx%d"), *KeySuffix, MipIndex, Mip.SizeX, Mip.SizeY)
		);
}

/**
 * Computes the derived data key for a texture with the specified compression settings.
 * @param Texture - The texture for which to compute the derived data key.
 * @param BuildSettingsPerLayer - Array of FTextureBuildSettings (1 per layer) for which to compute the derived data key.
 * @param OutKey - The derived data key.
 */
static void GetTextureDerivedDataKey(
	const UTexture& Texture,
	const FTextureBuildSettings* BuildSettingsPerLayer,
	FString& OutKey
	)
{
	FString KeySuffix;
	GetTextureDerivedDataKeySuffix(Texture, BuildSettingsPerLayer, KeySuffix);
	GetTextureDerivedDataKeyFromSuffix(KeySuffix, OutKey);
}

#endif // #if WITH_EDITOR

/*------------------------------------------------------------------------------
	Texture compression.
------------------------------------------------------------------------------*/

#if WITH_EDITOR

static void FinalizeBuildSettingsForLayer(const UTexture& Texture, int32 LayerIndex, FTextureBuildSettings& OutSettings)
{
	FTextureFormatSettings FormatSettings;
	Texture.GetLayerFormatSettings(LayerIndex, FormatSettings);

	OutSettings.bHDRSource = Texture.HasHDRSource(LayerIndex);
	OutSettings.bSRGB = FormatSettings.SRGB;
	OutSettings.bApplyYCoCgBlockScale = FormatSettings.CompressionYCoCg;

	if (FormatSettings.CompressionSettings == TC_Displacementmap || FormatSettings.CompressionSettings == TC_DistanceFieldFont)
	{
		OutSettings.bReplicateAlpha = true;
	}
	else if (FormatSettings.CompressionSettings == TC_Grayscale || FormatSettings.CompressionSettings == TC_Alpha)
	{
		OutSettings.bReplicateRed = true;
	}
}

/**
 * Sets texture build settings.
 * @param Texture - The texture for which to build compressor settings.
 * @param OutBuildSettings - Build settings.
 */
static void GetTextureBuildSettings(
	const UTexture& Texture,
	const UTextureLODSettings& TextureLODSettings,
	const ITargetPlatform& CurrentPlatform,
	FTextureBuildSettings& OutBuildSettings
	)
{
	const bool bPlatformSupportsTextureStreaming = CurrentPlatform.SupportsFeature(ETargetPlatformFeatures::TextureStreaming);
	const bool bPlatformSupportsVirtualTextureStreaming = CurrentPlatform.SupportsFeature(ETargetPlatformFeatures::VirtualTextureStreaming);
		
	OutBuildSettings.ColorAdjustment.AdjustBrightness = Texture.AdjustBrightness;
	OutBuildSettings.ColorAdjustment.AdjustBrightnessCurve = Texture.AdjustBrightnessCurve;
	OutBuildSettings.ColorAdjustment.AdjustVibrance = Texture.AdjustVibrance;
	OutBuildSettings.ColorAdjustment.AdjustSaturation = Texture.AdjustSaturation;
	OutBuildSettings.ColorAdjustment.AdjustRGBCurve = Texture.AdjustRGBCurve;
	OutBuildSettings.ColorAdjustment.AdjustHue = Texture.AdjustHue;
	OutBuildSettings.ColorAdjustment.AdjustMinAlpha = Texture.AdjustMinAlpha;
	OutBuildSettings.ColorAdjustment.AdjustMaxAlpha = Texture.AdjustMaxAlpha;
	OutBuildSettings.bUseLegacyGamma = Texture.bUseLegacyGamma;
	OutBuildSettings.bPreserveBorder = Texture.bPreserveBorder;
	OutBuildSettings.bDitherMipMapAlpha = Texture.bDitherMipMapAlpha;
	OutBuildSettings.AlphaCoverageThresholds = Texture.AlphaCoverageThresholds;
	OutBuildSettings.bComputeBokehAlpha = (Texture.LODGroup == TEXTUREGROUP_Bokeh);
	OutBuildSettings.bReplicateAlpha = false;
	OutBuildSettings.bReplicateRed = false;
	OutBuildSettings.bVolume = false;
	OutBuildSettings.bCubemap = false;
	OutBuildSettings.bTextureArray = false;
	OutBuildSettings.DiffuseConvolveMipLevel = 0;
	OutBuildSettings.bLongLatSource = false;
	OutBuildSettings.bStreamable = false;

	if (Texture.MaxTextureSize > 0)
	{
		OutBuildSettings.MaxTextureResolution = Texture.MaxTextureSize;
	}

	if (Texture.IsA(UTextureCube::StaticClass()))
	{
		OutBuildSettings.bCubemap = true;
		OutBuildSettings.DiffuseConvolveMipLevel = GDiffuseConvolveMipLevel;
		const UTextureCube* Cube = CastChecked<UTextureCube>(&Texture);
		OutBuildSettings.bLongLatSource = (Cube->Source.GetNumSlices() == 1);
	}
	else if (Texture.IsA(UTexture2DArray::StaticClass()))
	{
		OutBuildSettings.bStreamable = GSupportsTexture2DArrayStreaming;
		OutBuildSettings.bTextureArray = true;
	}
	else if (Texture.IsA(UVolumeTexture::StaticClass()))
	{
		OutBuildSettings.bStreamable = GSupportsVolumeTextureStreaming;
		OutBuildSettings.bVolume = true;
	}
	else if (Texture.IsA(UTexture2D::StaticClass()))
	{
		OutBuildSettings.bStreamable = true;
	}

	bool bDownsampleWithAverage;
	bool bSharpenWithoutColorShift;
	bool bBorderColorBlack;
	TextureMipGenSettings MipGenSettings;
	TextureLODSettings.GetMipGenSettings( 
		Texture,
		MipGenSettings,
		OutBuildSettings.MipSharpening,
		OutBuildSettings.SharpenMipKernelSize,
		bDownsampleWithAverage,
		bSharpenWithoutColorShift,
		bBorderColorBlack
		);

	static const auto CVarVirtualTexturesEnabled = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.VirtualTextures")); check(CVarVirtualTexturesEnabled);
	const bool bVirtualTextureStreaming = CVarVirtualTexturesEnabled->GetValueOnAnyThread() && bPlatformSupportsVirtualTextureStreaming && Texture.VirtualTextureStreaming;
	const FIntPoint SourceSize = Texture.Source.GetLogicalSize();

	OutBuildSettings.MipGenSettings = MipGenSettings;
	OutBuildSettings.bDownsampleWithAverage = bDownsampleWithAverage;
	OutBuildSettings.bSharpenWithoutColorShift = bSharpenWithoutColorShift;
	OutBuildSettings.bBorderColorBlack = bBorderColorBlack;
	OutBuildSettings.bFlipGreenChannel = Texture.bFlipGreenChannel;
	OutBuildSettings.CompositeTextureMode = Texture.CompositeTextureMode;
	OutBuildSettings.CompositePower = Texture.CompositePower;
	OutBuildSettings.LODBias = TextureLODSettings.CalculateLODBias(SourceSize.X, SourceSize.Y, Texture.MaxTextureSize, Texture.LODGroup, Texture.LODBias, Texture.NumCinematicMipLevels, Texture.MipGenSettings, bVirtualTextureStreaming);
	OutBuildSettings.LODBiasWithCinematicMips = TextureLODSettings.CalculateLODBias(SourceSize.X, SourceSize.Y, Texture.MaxTextureSize, Texture.LODGroup, Texture.LODBias, 0, Texture.MipGenSettings, bVirtualTextureStreaming);
	OutBuildSettings.bStreamable &= bPlatformSupportsTextureStreaming && !Texture.NeverStream && (Texture.LODGroup != TEXTUREGROUP_UI);
	OutBuildSettings.bVirtualStreamable = bVirtualTextureStreaming;
	OutBuildSettings.PowerOfTwoMode = Texture.PowerOfTwoMode;
	OutBuildSettings.PaddingColor = Texture.PaddingColor;
	OutBuildSettings.ChromaKeyColor = Texture.ChromaKeyColor;
	OutBuildSettings.bChromaKeyTexture = Texture.bChromaKeyTexture;
	OutBuildSettings.ChromaKeyThreshold = Texture.ChromaKeyThreshold;
	OutBuildSettings.CompressionQuality = Texture.CompressionQuality - 1; // translate from enum's 0 .. 5 to desired compression (-1 .. 4, where -1 is default while 0 .. 4 are actual quality setting override)
	
	OutBuildSettings.LossyCompressionAmount = Texture.LossyCompressionAmount.GetValue();

	// if LossyCompressionAmount is Default, inherit from LODGroup :
	const FTextureLODGroup& LODGroup = TextureLODSettings.GetTextureLODGroup(Texture.LODGroup);
	if ( OutBuildSettings.LossyCompressionAmount == TLCA_Default )
		OutBuildSettings.LossyCompressionAmount = LODGroup.LossyCompressionAmount;

	OutBuildSettings.Downscale = 1.0f;
	if (MipGenSettings == TMGS_NoMipmaps && 
		Texture.IsA(UTexture2D::StaticClass()))	// TODO: support more texture types
	{
		TextureLODSettings.GetDownscaleOptions(Texture, CurrentPlatform, OutBuildSettings.Downscale, (ETextureDownscaleOptions&)OutBuildSettings.DownscaleOptions);
	}
	
	// For virtual texturing we take the address mode into consideration
	if (OutBuildSettings.bVirtualStreamable)
	{
		const UTexture2D *Texture2D = Cast<UTexture2D>(&Texture);
		checkf(Texture2D, TEXT("Virtual texturing is only supported on 2D textures"));
		if (Texture.Source.GetNumBlocks() > 1)
		{
			// Multi-block textures (UDIM) interpret UVs outside [0,1) range as different blocks, so wrapping within a given block doesn't make sense
			// We want to make sure address mode is set to clamp here, otherwise border pixels along block edges will have artifacts
			OutBuildSettings.VirtualAddressingModeX = TA_Clamp;
			OutBuildSettings.VirtualAddressingModeY = TA_Clamp;
		}
		else
		{
			OutBuildSettings.VirtualAddressingModeX = Texture2D->AddressX;
			OutBuildSettings.VirtualAddressingModeY = Texture2D->AddressY;
		}

		FVirtualTextureBuildSettings VirtualTextureBuildSettings;
		Texture.GetVirtualTextureBuildSettings(VirtualTextureBuildSettings);
		OutBuildSettings.bVirtualTextureEnableCompressZlib = VirtualTextureBuildSettings.bEnableCompressZlib;
		OutBuildSettings.bVirtualTextureEnableCompressCrunch = VirtualTextureBuildSettings.bEnableCompressCrunch;
		OutBuildSettings.VirtualTextureTileSize = FMath::RoundUpToPowerOfTwo(VirtualTextureBuildSettings.TileSize);

		// Apply any LOD group tile size bias here
		const int32 TileSizeBias = TextureLODSettings.GetTextureLODGroup(Texture.LODGroup).VirtualTextureTileSizeBias;
		OutBuildSettings.VirtualTextureTileSize >>= (TileSizeBias < 0) ? -TileSizeBias : 0;
		OutBuildSettings.VirtualTextureTileSize <<= (TileSizeBias > 0) ? TileSizeBias : 0;

		// Don't allow max resolution to be less than VT tile size
		OutBuildSettings.MaxTextureResolution = FMath::Max<uint32>(OutBuildSettings.MaxTextureResolution, OutBuildSettings.VirtualTextureTileSize);

		// 0 is a valid value for border size
		// 1 would be OK in some cases, but breaks BC compressed formats, since it will result in physical tiles that aren't divisible by block size (4)
		// Could allow border size of 1 for non BC compressed virtual textures, but somewhat complicated to get that correct, especially with multiple layers
		// Doesn't seem worth the complexity for now, so clamp the size to be at least 2
		OutBuildSettings.VirtualTextureBorderSize = (VirtualTextureBuildSettings.TileBorderSize > 0) ? FMath::RoundUpToPowerOfTwo(FMath::Max(VirtualTextureBuildSettings.TileBorderSize, 2)) : 0;
	}
	else
	{
		OutBuildSettings.VirtualAddressingModeX = TA_Wrap;
		OutBuildSettings.VirtualAddressingModeY = TA_Wrap;
		OutBuildSettings.VirtualTextureTileSize = 0;
		OutBuildSettings.VirtualTextureBorderSize = 0;
		OutBuildSettings.bVirtualTextureEnableCompressZlib = false;
		OutBuildSettings.bVirtualTextureEnableCompressCrunch = false;
	}

	// By default, initialize settings for layer0
	FinalizeBuildSettingsForLayer(Texture, 0, OutBuildSettings);
}

/**
 * Sets build settings for a texture on the current running platform
 * @param Texture - The texture for which to build compressor settings.
 * @param OutBuildSettings - Array of desired texture settings
 */
static void GetBuildSettingsForRunningPlatform(
	const UTexture& Texture,
	TArray<FTextureBuildSettings>& OutSettingPerLayer
	)
{
	// Compress to whatever formats the active target platforms want
	ITargetPlatformManagerModule* TPM = GetTargetPlatformManager();
	if (TPM)
	{
		ITargetPlatform* CurrentPlatform = NULL;
		const TArray<ITargetPlatform*>& Platforms = TPM->GetActiveTargetPlatforms();

		check(Platforms.Num());

		CurrentPlatform = Platforms[0];

		for (int32 Index = 1; Index < Platforms.Num(); Index++)
		{
			if (Platforms[Index]->IsRunningPlatform())
			{
				CurrentPlatform = Platforms[Index];
				break;
			}
		}

		check(CurrentPlatform != NULL);

		const UTextureLODSettings* LODSettings = (UTextureLODSettings*)UDeviceProfileManager::Get().FindProfile(CurrentPlatform->PlatformName());
		FTextureBuildSettings SourceBuildSettings;
		GetTextureBuildSettings(Texture, *LODSettings, *CurrentPlatform, SourceBuildSettings);

		TArray< TArray<FName> > PlatformFormats;
		CurrentPlatform->GetTextureFormats(&Texture, PlatformFormats);
		check(PlatformFormats.Num() > 0);

		const int32 NumLayers = Texture.Source.GetNumLayers();
		check(PlatformFormats[0].Num() == NumLayers);

		OutSettingPerLayer.Reserve(NumLayers);
		for (int32 LayerIndex = 0; LayerIndex < NumLayers; ++LayerIndex)
		{
			FTextureBuildSettings& OutSettings = OutSettingPerLayer.Add_GetRef(SourceBuildSettings);
			OutSettings.TextureFormatName = PlatformFormats[0][LayerIndex];
			FinalizeBuildSettingsForLayer(Texture, LayerIndex, OutSettings);

			if (SourceBuildSettings.bVirtualStreamable)
			{
				OutSettings.TextureFormatName = CurrentPlatform->FinalizeVirtualTextureLayerFormat(OutSettings.TextureFormatName);
			}
		}
	}
}

static void GetBuildSettingsPerFormat(const UTexture& Texture, const FTextureBuildSettings& SourceBuildSettings, const ITargetPlatform* TargetPlatform, TArray< TArray<FTextureBuildSettings> >& OutBuildSettingsPerFormat)
{
	const int32 NumLayers = Texture.Source.GetNumLayers();

	TArray< TArray<FName> > PlatformFormats;
	TargetPlatform->GetTextureFormats(&Texture, PlatformFormats);

	OutBuildSettingsPerFormat.Reserve(PlatformFormats.Num());
	for (TArray<FName>& PlatformFormatsPerLayer : PlatformFormats)
	{
		check(PlatformFormatsPerLayer.Num() == NumLayers);
		TArray<FTextureBuildSettings>& OutSettingPerLayer = OutBuildSettingsPerFormat.AddDefaulted_GetRef();
		OutSettingPerLayer.Reserve(NumLayers);
		for (int32 LayerIndex = 0; LayerIndex < NumLayers; ++LayerIndex)
		{
			FTextureBuildSettings& OutSettings = OutSettingPerLayer.Add_GetRef(SourceBuildSettings);
			OutSettings.TextureFormatName = PlatformFormatsPerLayer[LayerIndex];
			FinalizeBuildSettingsForLayer(Texture, LayerIndex, OutSettings);

			if (SourceBuildSettings.bVirtualStreamable)
			{
				OutSettings.TextureFormatName = TargetPlatform->FinalizeVirtualTextureLayerFormat(OutSettings.TextureFormatName);
			}
		}
	}
}

/**
 * Stores derived data in the DDC.
 * After this returns, all bulk data from streaming (non-inline) mips will be sent separately to the DDC and the BulkData for those mips removed.
 * @param DerivedData - The data to store in the DDC.
 * @param DerivedDataKeySuffix - The key suffix at which to store derived data.
 * @param bForceAllMipsToBeInlined - Whether to store all mips in the main DDC. Relates to how the texture resources get initialized (not supporting streaming).
 * @return number of bytes put to the DDC (total, including all mips)
 */
uint32 PutDerivedDataInCache(FTexturePlatformData* DerivedData, const FString& DerivedDataKeySuffix, const FStringView& TextureName, bool bForceAllMipsToBeInlined)
{
	TArray<uint8> RawDerivedData;
	FString DerivedDataKey;
	uint32 TotalBytesPut = 0;

	// Build the key with which to cache derived data.
	GetTextureDerivedDataKeyFromSuffix(DerivedDataKeySuffix, DerivedDataKey);

	FString LogString;
	if (UE_LOG_ACTIVE(LogTexture,Verbose))
	{
		LogString = FString::Printf(
			TEXT("Storing texture in DDC:\n  Key: %s\n  Format: %s\n"),
			*DerivedDataKey,
			GPixelFormats[DerivedData->PixelFormat].Name
			);
	}

	// Write out individual mips to the derived data cache.
	const int32 MipCount = DerivedData->Mips.Num();
	const int32 FirstInlineMip = bForceAllMipsToBeInlined ? 0 : FMath::Max(0, MipCount - FMath::Max((int32)NUM_INLINE_DERIVED_MIPS, (int32)DerivedData->GetNumMipsInTail()));
	const int32 WritableMipCount = MipCount - ((DerivedData->GetNumMipsInTail() > 0) ? (DerivedData->GetNumMipsInTail() - 1) : 0);
	for (int32 MipIndex = 0; MipIndex < WritableMipCount; ++MipIndex)
	{
		FString MipDerivedDataKey;
		FTexture2DMipMap& Mip = DerivedData->Mips[MipIndex];
		const bool bInline = (MipIndex >= FirstInlineMip);
		GetTextureDerivedMipKey(MipIndex, Mip, DerivedDataKeySuffix, MipDerivedDataKey);

		if (UE_LOG_ACTIVE(LogTexture,Verbose))
		{
			LogString += FString::Printf(TEXT("  Mip%d %dx%d %d bytes%s %s\n"),
				MipIndex,
				Mip.SizeX,
				Mip.SizeY,
				Mip.BulkData.GetBulkDataSize(),
				bInline ? TEXT(" [inline]") : TEXT(""),
				*MipDerivedDataKey
				);
		}

		// Note that calling StoreInDerivedDataCache() also calls RemoveBulkData().
		// This means that the resource needs to load differently inlined mips and non inlined mips.
		if (!bInline)
		{
			// store in the DDC, also drop the bulk data storage.
			TotalBytesPut += Mip.StoreInDerivedDataCache(MipDerivedDataKey, TextureName);
		}
	}

	// Write out each VT chunk to the DDC
	if (DerivedData->VTData)
	{
		const int32 ChunkCount = DerivedData->VTData->Chunks.Num();
		for (int32 ChunkIndex = 0; ChunkIndex < ChunkCount; ++ChunkIndex)
		{
			const FString ChunkDerivedDataKey = FDerivedDataCacheInterface::BuildCacheKey(
				TEXT("TEXTURE"), TEXTURE_DERIVEDDATA_VER,
				*FString::Printf(TEXT("%s_VTCHUNK%u"), *DerivedDataKeySuffix, ChunkIndex));

			FVirtualTextureDataChunk& Chunk = DerivedData->VTData->Chunks[ChunkIndex];
			TotalBytesPut += Chunk.StoreInDerivedDataCache(ChunkDerivedDataKey, TextureName);
		}
	}

	// Store derived data.
	// At this point we've stored all the non-inline data in the DDC, so this will only serialize and store the TexturePlatformData metadata and any inline mips
	FMemoryWriter Ar(RawDerivedData, /*bIsPersistent=*/ true);
	DerivedData->Serialize(Ar, NULL);
	TotalBytesPut += RawDerivedData.Num();
	GetDerivedDataCacheRef().Put(*DerivedDataKey, RawDerivedData, TextureName, /*bPutEvenIfExists*/ true);
	UE_LOG(LogTexture,Verbose,TEXT("%s  Derived Data: %d bytes"),*LogString,RawDerivedData.Num());
	return TotalBytesPut;
}

#endif // #if WITH_EDITOR

/*------------------------------------------------------------------------------
	Derived data.
------------------------------------------------------------------------------*/

#if WITH_EDITOR

/**
 * Unpack a DXT 565 color to RGB32.
 */
static uint16 UnpackDXTColor(int32* OutColors, const uint8* Block)
{
	uint16 PackedColor = (Block[1] << 8) | Block[0];
	uint8 Red = (PackedColor >> 11) & 0x1f;
	OutColors[0] = (Red << 3) | ( Red >> 2);
	uint8 Green = (PackedColor >> 5) & 0x3f;
	OutColors[1] = (Green << 2) | ( Green >> 4);
	uint8 Blue = PackedColor & 0x1f;
	OutColors[2] = (Blue << 3) | ( Blue >> 2);
	return PackedColor;
}

/**
 * Computes the squared error between a DXT compression block and the source colors.
 */
static double ComputeDXTColorBlockSquaredError(const uint8* Block, const FColor* Colors, int32 ColorPitch)
{
	int32 ColorTable[4][3];

	uint16 c0 = UnpackDXTColor(ColorTable[0], Block);
	uint16 c1 = UnpackDXTColor(ColorTable[1], Block + 2);
	if (c0 > c1)
	{
		for (int32 ColorIndex = 0; ColorIndex < 3; ++ColorIndex)
		{
			ColorTable[2][ColorIndex] = (2 * ColorTable[0][ColorIndex]) / 3 + (1 * ColorTable[1][ColorIndex]) / 3;
			ColorTable[3][ColorIndex] = (1 * ColorTable[0][ColorIndex]) / 3 + (2 * ColorTable[1][ColorIndex]) / 3;
		}
	}
	else
	{
		for (int32 ColorIndex = 0; ColorIndex < 3; ++ColorIndex)
		{
			ColorTable[2][ColorIndex] = (1 * ColorTable[0][ColorIndex]) / 2 + (1 * ColorTable[1][ColorIndex]) / 2;
			ColorTable[3][ColorIndex] = 0;
		}
	}

	double SquaredError = 0.0;
	for (int32 Y = 0; Y < 4; ++Y)
	{
		uint8 IndexTable[4];
		uint8 RowIndices = Block[4+Y];
		IndexTable[0] = RowIndices & 0x3;
		IndexTable[1] = (RowIndices >> 2) & 0x3;
		IndexTable[2] = (RowIndices >> 4) & 0x3;
		IndexTable[3] = (RowIndices >> 6) & 0x3;

		for (int32 X = 0; X < 4; ++X)
		{
			FColor Color = Colors[Y * ColorPitch + X];
			int32* DXTColor = ColorTable[IndexTable[X]];
			SquaredError += ((int32)Color.R - DXTColor[0]) * ((int32)Color.R - DXTColor[0]);
			SquaredError += ((int32)Color.G - DXTColor[1]) * ((int32)Color.G - DXTColor[1]);
			SquaredError += ((int32)Color.B - DXTColor[2]) * ((int32)Color.B - DXTColor[2]);
		}
	}
	return SquaredError;
}

/**
 * Computes the squared error between the alpha values in the block and the source colors.
 */
static double ComputeDXTAlphaBlockSquaredError(const uint8* Block, const FColor* Colors, int32 ColorPitch)
{
	int32 AlphaTable[8];

	int32 a0 = Block[0];
	int32 a1 = Block[1];

	AlphaTable[0] = a0;
	AlphaTable[1] = a1;
	if (AlphaTable[0] > AlphaTable[1])
	{
		for (int32 AlphaIndex = 0; AlphaIndex < 6; ++AlphaIndex)
		{
			AlphaTable[AlphaIndex+2] = ((6 - AlphaIndex) * a0 + (1 + AlphaIndex) * a1) / 7;
		}
	}
	else
	{
		for (int32 AlphaIndex = 0; AlphaIndex < 4; ++AlphaIndex)
		{
			AlphaTable[AlphaIndex+2] = ((4 - AlphaIndex) * a0 + (1 + AlphaIndex) * a1) / 5;
		}
		AlphaTable[6] = 0;
		AlphaTable[7] = 255;
	}

	uint64 IndexBits = (uint64)Block[7];
	IndexBits = (IndexBits << 8) | (uint64)Block[6];
	IndexBits = (IndexBits << 8) | (uint64)Block[5];
	IndexBits = (IndexBits << 8) | (uint64)Block[4];
	IndexBits = (IndexBits << 8) | (uint64)Block[3];
	IndexBits = (IndexBits << 8) | (uint64)Block[2];

	double SquaredError = 0.0;
	for (int32 Y = 0; Y < 4; ++Y)
	{
		for (int32 X = 0; X < 4; ++X)
		{
			const FColor Color = Colors[Y * ColorPitch + X];
			uint8 Index = IndexBits & 0x7;
			SquaredError += ((int32)Color.A - AlphaTable[Index]) * ((int32)Color.A - AlphaTable[Index]);
			IndexBits = IndexBits >> 3;
		}
	}
	return SquaredError;
}

/**
 * Computes the PSNR value for the compressed image.
 */
static float ComputePSNR(const FImage& SrcImage, const FCompressedImage2D& CompressedImage)
{
	double SquaredError = 0.0;
	int32 NumErrors = 0;
	const uint8* CompressedData = CompressedImage.RawData.GetData();

	if (SrcImage.Format == ERawImageFormat::BGRA8 && (CompressedImage.PixelFormat == PF_DXT1 || CompressedImage.PixelFormat == PF_DXT5))
	{
		int32 NumBlocksX = CompressedImage.SizeX / 4;
		int32 NumBlocksY = CompressedImage.SizeY / 4;
		for (int32 BlockY = 0; BlockY < NumBlocksY; ++BlockY)
		{
			for (int32 BlockX = 0; BlockX < NumBlocksX; ++BlockX)
			{
				if (CompressedImage.PixelFormat == PF_DXT1)
				{
					SquaredError += ComputeDXTColorBlockSquaredError(
						CompressedData + (BlockY * NumBlocksX + BlockX) * 8,
						(&SrcImage.AsBGRA8()[0]) + (BlockY * NumBlocksX * 16 + BlockX * 4),
						SrcImage.SizeX
						);
					NumErrors += 16 * 3;
				}
				else if (CompressedImage.PixelFormat == PF_DXT5)
				{
					SquaredError += ComputeDXTAlphaBlockSquaredError(
						CompressedData + (BlockY * NumBlocksX + BlockX) * 16,
						(&SrcImage.AsBGRA8()[0]) + (BlockY * NumBlocksX * 16 + BlockX * 4),
						SrcImage.SizeX
						);
					SquaredError += ComputeDXTColorBlockSquaredError(
						CompressedData + (BlockY * NumBlocksX + BlockX) * 16 + 8,
						(&SrcImage.AsBGRA8()[0]) + (BlockY * NumBlocksX * 16 + BlockX * 4),
						SrcImage.SizeX
						);
					NumErrors += 16 * 4;
				}
			}
		}
	}

	double MeanSquaredError = NumErrors > 0 ? SquaredError / (double)NumErrors : 0.0;
	double RMSE = FMath::Sqrt(MeanSquaredError);
	return RMSE > 0.0 ? 20.0f * (float)log10(255.0 / RMSE) : 500.0f;
}


void FTexturePlatformData::Cache(
	UTexture& InTexture,
	const FTextureBuildSettings* InSettingsPerLayer,
	uint32 InFlags,
	ITextureCompressorModule* Compressor
	)
{
	// Flush any existing async task and ignore results.
	FinishCache();

	uint32 Flags = InFlags;

	static bool bForDDC = FString(FCommandLine::Get()).Contains(TEXT("Run=DerivedDataCache"));
	if (bForDDC)
	{
		Flags |= ETextureCacheFlags::ForDDCBuild;
	}

	bool bForceRebuild = (Flags & ETextureCacheFlags::ForceRebuild) != 0;
	bool bAsync = !bForDDC && (Flags & ETextureCacheFlags::Async) != 0;
	GetTextureDerivedDataKey(InTexture, InSettingsPerLayer, DerivedDataKey);

	if (!Compressor)
	{
		Compressor = &FModuleManager::LoadModuleChecked<ITextureCompressorModule>(TEXTURE_COMPRESSOR_MODULENAME);
	}

	if (InSettingsPerLayer[0].bVirtualStreamable)
	{
		Flags |= ETextureCacheFlags::ForVirtualTextureStreamingBuild;
	}

	if (bAsync && !bForceRebuild)
	{
		AsyncTask = new FTextureAsyncCacheDerivedDataTask(Compressor, this, &InTexture, InSettingsPerLayer, Flags);
		FQueuedThreadPool* ThreadPool = GThreadPool;
#if WITH_EDITOR
		ThreadPool = GLargeThreadPool;
#endif
		AsyncTask->StartBackgroundTask(ThreadPool);
	}
	else
	{
		FTextureCacheDerivedDataWorker Worker(Compressor, this, &InTexture, InSettingsPerLayer, Flags);
		{
			COOK_STAT(auto Timer = TextureCookStats::UsageStats.TimeSyncWork());
			Worker.DoWork();
			Worker.Finalize();
			COOK_STAT(Timer.AddHitOrMiss(Worker.WasLoadedFromDDC() ? FCookStats::CallStats::EHitOrMiss::Hit : FCookStats::CallStats::EHitOrMiss::Miss, Worker.GetBytesCached()));
		}
	}
}

bool FTexturePlatformData::IsAsyncWorkComplete() const
{
	return AsyncTask == nullptr || AsyncTask->IsWorkDone();
}

void FTexturePlatformData::FinishCache()
{
	if (AsyncTask)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FTexturePlatformData::FinishCache)
		{
			COOK_STAT(auto Timer = TextureCookStats::UsageStats.TimeAsyncWait());
			AsyncTask->EnsureCompletion();
			FTextureCacheDerivedDataWorker& Worker = AsyncTask->GetTask();
			Worker.Finalize();
			COOK_STAT(Timer.AddHitOrMiss(Worker.WasLoadedFromDDC() ? FCookStats::CallStats::EHitOrMiss::Hit : FCookStats::CallStats::EHitOrMiss::Miss, Worker.GetBytesCached()));
		}
		delete AsyncTask;
		AsyncTask = NULL;
	}
}

typedef TArray<uint32, TInlineAllocator<MAX_TEXTURE_MIP_COUNT> > FAsyncMipHandles;
typedef TArray<uint32> FAsyncVTChunkHandles;

/**
 * Executes async DDC gets for mips stored in the derived data cache.
 * @param Mip - Mips to retrieve.
 * @param FirstMipToLoad - Index of the first mip to retrieve.
 * @param OutHandles - Handles to the asynchronous DDC gets.
 */
static void BeginLoadDerivedMips(TIndirectArray<FTexture2DMipMap>& Mips, int32 FirstMipToLoad, UTexture* Texture, FAsyncMipHandles& OutHandles)
{
	FDerivedDataCacheInterface& DDC = GetDerivedDataCacheRef();
	OutHandles.AddZeroed(Mips.Num());
	for (int32 MipIndex = FirstMipToLoad; MipIndex < Mips.Num(); ++MipIndex)
	{
		const FTexture2DMipMap& Mip = Mips[MipIndex];
		if (Mip.DerivedDataKey.IsEmpty() == false)
		{
			OutHandles[MipIndex] = DDC.GetAsynchronous(*Mip.DerivedDataKey, Texture ? Texture->GetPathName() : TEXT("Unknown Texture"_SV));
		}
	}
}

static void BeginLoadDerivedVTChunks(const TArray<FVirtualTextureDataChunk>& Chunks, UTexture* Texture, FAsyncVTChunkHandles& OutHandles)
{
	FDerivedDataCacheInterface& DDC = GetDerivedDataCacheRef();
	OutHandles.AddZeroed(Chunks.Num());
	for (int32 ChunkIndex = 0; ChunkIndex < Chunks.Num(); ++ChunkIndex)
	{
		const FVirtualTextureDataChunk& Chunk = Chunks[ChunkIndex];
		if (Chunk.DerivedDataKey.IsEmpty() == false)
		{
			OutHandles[ChunkIndex] = DDC.GetAsynchronous(*Chunk.DerivedDataKey, Texture ? Texture->GetPathName() : TEXT("Unknown Texture"_SV));
		}
	}
}

/** Asserts that MipSize is correct for the mipmap. */
static void CheckMipSize(FTexture2DMipMap& Mip, EPixelFormat PixelFormat, int32 MipSize)
{
	// Only volume can have SizeZ != 1
	if (MipSize != Mip.SizeZ * CalcTextureMipMapSize(Mip.SizeX, Mip.SizeY, PixelFormat, 0))
	{
		UE_LOG(LogTexture, Warning,
			TEXT("%dx%d mip of %s texture has invalid data in the DDC. Got %d bytes, expected %d. Key=%s"),
			Mip.SizeX,
			Mip.SizeY,
			GPixelFormats[PixelFormat].Name,
			MipSize,
			CalcTextureMipMapSize(Mip.SizeX, Mip.SizeY, PixelFormat, 0),
			*Mip.DerivedDataKey
			);
	}
}

bool FTexturePlatformData::TryInlineMipData(int32 FirstMipToLoad, UTexture* Texture)
{
	FAsyncMipHandles AsyncHandles;
	FAsyncVTChunkHandles AsyncVTHandles;
	TArray<uint8> TempData;
	FDerivedDataCacheInterface& DDC = GetDerivedDataCacheRef();

	BeginLoadDerivedMips(Mips, FirstMipToLoad, Texture, AsyncHandles);
	if (VTData != nullptr)
	{
		BeginLoadDerivedVTChunks(VTData->Chunks, Texture, AsyncVTHandles);
	}

	// Process regular mips
	for (int32 MipIndex = FirstMipToLoad; MipIndex < Mips.Num(); ++MipIndex)
	{
		FTexture2DMipMap& Mip = Mips[MipIndex];
		if (Mip.DerivedDataKey.IsEmpty() == false)
		{
			uint32 AsyncHandle = AsyncHandles[MipIndex];
			bool bLoadedFromDDC = false;
			{
				COOK_STAT(auto Timer = TextureCookStats::StreamingMipUsageStats.TimeAsyncWait());
				DDC.WaitAsynchronousCompletion(AsyncHandle);
				bLoadedFromDDC = DDC.GetAsynchronousResults(AsyncHandle, TempData);
				COOK_STAT(Timer.AddHitOrMiss(bLoadedFromDDC ? FCookStats::CallStats::EHitOrMiss::Hit : FCookStats::CallStats::EHitOrMiss::Miss, TempData.Num()));
			}
			if (bLoadedFromDDC)
			{
				int32 MipSize = 0;
				FMemoryReader Ar(TempData, /*bIsPersistent=*/ true);
				Ar << MipSize;

				Mip.BulkData.Lock(LOCK_READ_WRITE);
				void* MipData = Mip.BulkData.Realloc(MipSize);
				Ar.Serialize(MipData, MipSize);
				Mip.BulkData.Unlock();
				Mip.DerivedDataKey.Empty();
			}
			else
			{
				return false;
			}
			TempData.Reset();
		}
	}

	// Process VT mips
	if (VTData != nullptr)
	{
		for (int32 ChunkIndex = 0; ChunkIndex < VTData->Chunks.Num(); ++ChunkIndex)
		{
			FVirtualTextureDataChunk& Chunk = VTData->Chunks[ChunkIndex];
			if (Chunk.DerivedDataKey.IsEmpty() == false)
			{
				uint32 AsyncHandle = AsyncVTHandles[ChunkIndex];
				bool bLoadedFromDDC = false;
				{
					COOK_STAT(auto Timer = TextureCookStats::StreamingMipUsageStats.TimeAsyncWait());
					DDC.WaitAsynchronousCompletion(AsyncHandle);
					bLoadedFromDDC = DDC.GetAsynchronousResults(AsyncHandle, TempData);
					COOK_STAT(Timer.AddHitOrMiss(bLoadedFromDDC ? FCookStats::CallStats::EHitOrMiss::Hit : FCookStats::CallStats::EHitOrMiss::Miss, TempData.Num()));
				}
				if (bLoadedFromDDC)
				{
					int32 ChunkSize = 0;
					FMemoryReader Ar(TempData, /*bIsPersistent=*/ true);
					Ar << ChunkSize;
					Chunk.BulkData.Lock(LOCK_READ_WRITE);
					void* ChunkData = Chunk.BulkData.Realloc(ChunkSize);
					Ar.Serialize(ChunkData, ChunkSize);
					Chunk.BulkData.Unlock();
					Chunk.DerivedDataKey.Empty();
				}
				else
				{
					return false;
				}
				TempData.Reset();
			}
		}
	}
	return true;
}

#endif // #if WITH_EDITOR

FTexturePlatformData::FTexturePlatformData()
	: SizeX(0)
	, SizeY(0)
	, PackedData(0)
	, PixelFormat(PF_Unknown)
	, VTData(nullptr)
#if WITH_EDITORONLY_DATA
	, AsyncTask(NULL)
#endif // #if WITH_EDITORONLY_DATA
{
}

FTexturePlatformData::~FTexturePlatformData()
{
#if WITH_EDITOR
	if (AsyncTask)
	{
		AsyncTask->EnsureCompletion();
		delete AsyncTask;
		AsyncTask = NULL;
	}
#endif
	if (VTData) delete VTData;
}

bool FTexturePlatformData::IsReadyForAsyncPostLoad() const
{
#if WITH_EDITOR
	// Can't touch the Mips until async work is finished
	if (!IsAsyncWorkComplete())
	{
		return false;
	}
#endif

	for (int32 MipIndex = 0; MipIndex < Mips.Num(); ++MipIndex)
	{
		const FTexture2DMipMap& Mip = Mips[MipIndex];
		if (!Mip.BulkData.IsAsyncLoadingComplete())
		{
			return false;
		}
	}
	return true;
}

bool FTexturePlatformData::TryLoadMips(int32 FirstMipToLoad, void** OutMipData, UTexture* Texture)
{
	int32 NumMipsCached = 0;
	const int32 LoadableMips = Mips.Num() - ((GetNumMipsInTail() > 0) ? (GetNumMipsInTail() - 1) : 0);
	check(LoadableMips >= 0);

#if WITH_EDITOR
	TArray<uint8> TempData;
	FAsyncMipHandles AsyncHandles;
	FDerivedDataCacheInterface& DDC = GetDerivedDataCacheRef();
	BeginLoadDerivedMips(Mips, FirstMipToLoad, Texture, AsyncHandles);
#endif // #if WITH_EDITOR

	// Handle the case where we inlined more mips than we intend to keep resident
	// Discard unneeded mips
	for (int32 MipIndex = 0; MipIndex < FirstMipToLoad && MipIndex < LoadableMips; ++MipIndex)
	{
		FTexture2DMipMap& Mip = Mips[MipIndex];
		if (Mip.BulkData.IsBulkDataLoaded())
		{
			Mip.BulkData.Lock(LOCK_READ_ONLY);
			Mip.BulkData.Unlock();
		}
	}

	// Load remaining mips (if any) from bulk data.
	for (int32 MipIndex = FirstMipToLoad; MipIndex < LoadableMips; ++MipIndex)
	{
		FTexture2DMipMap& Mip = Mips[MipIndex];
		const int64 BulkDataSize = Mip.BulkData.GetBulkDataSize();
		if (BulkDataSize > 0)
		{
			if (OutMipData != nullptr)
			{
#if PLATFORM_SUPPORTS_TEXTURE_STREAMING
				// We want to make sure that any non-streamed mips are coming from the texture asset file, and not from an external bulk file.
				// But because "r.TextureStreaming" is driven by the project setting as well as the command line option "-NoTextureStreaming", 
				// is it possible for streaming mips to be loaded in non streaming ways.
				if (CVarSetTextureStreaming.GetValueOnAnyThread() != 0)
				{
					UE_CLOG(Mip.BulkData.IsInSeparateFile(), LogTexture, Error, TEXT("Loading non-streamed mips from an external bulk file.  This is not desireable.  File %s"), *(Mip.BulkData.GetFilename() ) );
				}
#endif
				Mip.BulkData.GetCopy(&OutMipData[MipIndex - FirstMipToLoad], true);
			}
			NumMipsCached++;
		}
	}

#if WITH_EDITOR
	// Wait for async DDC gets.
	for (int32 MipIndex = FirstMipToLoad; MipIndex < LoadableMips; ++MipIndex)
	{
		FTexture2DMipMap& Mip = Mips[MipIndex];
		if (Mip.DerivedDataKey.IsEmpty() == false)
		{
			uint32 AsyncHandle = AsyncHandles[MipIndex];
			DDC.WaitAsynchronousCompletion(AsyncHandle);
			if (DDC.GetAsynchronousResults(AsyncHandle, TempData))
			{
				int32 MipSize = 0;
				FMemoryReader Ar(TempData, /*bIsPersistent=*/ true);
				Ar << MipSize;
				CheckMipSize(Mip, PixelFormat, MipSize);
				NumMipsCached++;

				if (OutMipData)
				{
					OutMipData[MipIndex - FirstMipToLoad] = FMemory::Malloc(MipSize);
					Ar.Serialize(OutMipData[MipIndex - FirstMipToLoad], MipSize);
				}
			}
			else
			{
				UE_LOG(LogTexture, Verbose, TEXT("DDC.GetAsynchronousResults() failed for %s, MipIndex: %d"),
					Texture ? *Texture->GetPathName() : TEXT("nullptr"),
					MipIndex);
			}
			TempData.Reset();
		}
	}
#endif // #if WITH_EDITOR

	if (NumMipsCached != (LoadableMips - FirstMipToLoad))
	{
		UE_LOG(LogTexture, Verbose, TEXT("TryLoadMips failed for %s, NumMipsCached: %d, LoadableMips: %d, FirstMipToLoad: %d"),
			Texture ? *Texture->GetPathName() : TEXT("nullptr"),
			NumMipsCached,
			LoadableMips,
			FirstMipToLoad);

		// Unable to cache all mips. Release memory for those that were cached.
		for (int32 MipIndex = FirstMipToLoad; MipIndex < LoadableMips; ++MipIndex)
		{
			FTexture2DMipMap& Mip = Mips[MipIndex];
			UE_LOG(LogTexture, Verbose, TEXT("  Mip %d, BulkDataSize: %d"),
				MipIndex,
				(int32)Mip.BulkData.GetBulkDataSize());

			if (OutMipData && OutMipData[MipIndex - FirstMipToLoad])
			{
				FMemory::Free(OutMipData[MipIndex - FirstMipToLoad]);
				OutMipData[MipIndex - FirstMipToLoad] = NULL;
			}
		}
		return false;
	}

	return true;
}

int32 FTexturePlatformData::GetNumNonStreamingMips() const
{
	if (FPlatformProperties::RequiresCookedData())
	{
		// We're on a cooked platform so we should only be streaming mips that were not inlined in the texture by thecooker.
		int32 NumNonStreamingMips = Mips.Num();

		for (const FTexture2DMipMap& Mip : Mips)
		{
			if ( Mip.BulkData.IsInSeparateFile() || !Mip.BulkData.IsInlined() )
			{
				--NumNonStreamingMips;
			}
			else
			{
				break;
			}
		}

		if (NumNonStreamingMips == 0 && Mips.Num())
		{
			return 1;
		}
		else
		{
			return NumNonStreamingMips;
		}
	}
	else if (Mips.Num() > 0)
	{
		int32 MipCount = Mips.Num();
		int32 NumNonStreamingMips = 1;

		// Take in to account the min resident limit.
		NumNonStreamingMips = FMath::Max(NumNonStreamingMips, (int32)GetNumMipsInTail());
		NumNonStreamingMips = FMath::Max(NumNonStreamingMips, UTexture2D::GetStaticMinTextureResidentMipCount());
		NumNonStreamingMips = FMath::Min(NumNonStreamingMips, MipCount);
		int32 BlockSizeX = GPixelFormats[PixelFormat].BlockSizeX;
		int32 BlockSizeY = GPixelFormats[PixelFormat].BlockSizeY;
		if (BlockSizeX > 1 || BlockSizeY > 1)
		{
			NumNonStreamingMips = FMath::Max<int32>(NumNonStreamingMips, MipCount - FPlatformMath::FloorLog2(Mips[0].SizeX / BlockSizeX));
			NumNonStreamingMips = FMath::Max<int32>(NumNonStreamingMips, MipCount - FPlatformMath::FloorLog2(Mips[0].SizeY / BlockSizeY));
		}

		return NumNonStreamingMips;
	}
	else
	{
		return 0;
	}
}

int32 FTexturePlatformData::GetNumNonOptionalMips() const
{
	// TODO : Count from last mip to first.
	if (FPlatformProperties::RequiresCookedData())
	{
		int32 NumNonOptionalMips = Mips.Num();

		for (const FTexture2DMipMap& Mip : Mips)
		{
			if (Mip.BulkData.IsOptional())
			{
				--NumNonOptionalMips;
			}
			else
			{
				break;
			}
		}

		if (NumNonOptionalMips == 0 && Mips.Num())
		{
			return 1;
		}
		else
		{
			return NumNonOptionalMips;
		}
	}
	else // Otherwise, all mips are available.
	{
		return Mips.Num();
	}
}

bool FTexturePlatformData::CanBeLoaded() const
{
	for (const FTexture2DMipMap& Mip : Mips)
	{
#if WITH_EDITORONLY_DATA
		if (!Mip.DerivedDataKey.IsEmpty())
		{
			return true;
		}
#endif 
		if (Mip.BulkData.CanLoadFromDisk())
		{
			return true;
		}
	}
	return false;
}


int32 FTexturePlatformData::GetNumVTMips() const
{
	check(VTData);
	return VTData->GetNumMips();
}

EPixelFormat FTexturePlatformData::GetLayerPixelFormat(uint32 LayerIndex) const
{
	if (VTData)
	{
		check(LayerIndex < VTData->NumLayers);
		return VTData->LayerTypes[LayerIndex];
	}
	check(LayerIndex == 0u);
	return PixelFormat;
}

#if WITH_EDITOR
bool FTexturePlatformData::AreDerivedMipsAvailable() const
{
	bool bMipsAvailable = true;
	FDerivedDataCacheInterface& DDC = GetDerivedDataCacheRef();
	for (int32 MipIndex = 0; bMipsAvailable && MipIndex < Mips.Num(); ++MipIndex)
	{
		const FTexture2DMipMap& Mip = Mips[MipIndex];
		if (Mip.DerivedDataKey.IsEmpty() == false)
		{
			bMipsAvailable = DDC.CachedDataProbablyExists(*Mip.DerivedDataKey);
		}
	}
	return bMipsAvailable;
}
bool FTexturePlatformData::AreDerivedVTChunksAvailable() const
{
	bool bChunksAvailable = true;
	FDerivedDataCacheInterface& DDC = GetDerivedDataCacheRef();
	check(VTData);
	for (int32 ChunkIndex = 0; bChunksAvailable && ChunkIndex < VTData->Chunks.Num(); ++ChunkIndex)
	{
		const FVirtualTextureDataChunk& Chunk = VTData->Chunks[ChunkIndex];
		if (Chunk.DerivedDataKey.IsEmpty() == false)
		{
			bChunksAvailable = DDC.CachedDataProbablyExists(*Chunk.DerivedDataKey);
		}
	}
	return bChunksAvailable;
}
#endif // #if WITH_EDITOR

static void SerializePlatformData(
	FArchive& Ar,
	FTexturePlatformData* PlatformData,
	UTexture* Texture,
	bool bCooked,
	bool bStreamable
)
{
	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("SerializePlatformData"), STAT_Texture_SerializePlatformData, STATGROUP_LoadTime);

	UEnum* PixelFormatEnum = UTexture::GetPixelFormatEnum();

	Ar << PlatformData->SizeX;
	Ar << PlatformData->SizeY;
	Ar << PlatformData->PackedData;
	if (Ar.IsLoading())
	{
		FString PixelFormatString;
		Ar << PixelFormatString;
		PlatformData->PixelFormat = (EPixelFormat)PixelFormatEnum->GetValueByName(*PixelFormatString);
	}
	else if (Ar.IsSaving())
	{
		FString PixelFormatString = PixelFormatEnum->GetNameByValue(PlatformData->PixelFormat).GetPlainNameString();
		Ar << PixelFormatString;
	}

	if (PlatformData->GetHasOptData())
	{
		Ar << PlatformData->OptData;
	}

	int32 NumMips = PlatformData->Mips.Num();
	int32 FirstMipToSerialize = 0;

	bool bIsVirtual = false;
	if (Ar.IsSaving())
	{
		bIsVirtual = PlatformData->VTData != nullptr;
	}

	if (bCooked && bIsVirtual)
	{
		check(PlatformData->Mips.Num() == 0);
	}

	if (bCooked)
	{
#if WITH_EDITORONLY_DATA
		if (Ar.IsSaving())
		{
			check(Ar.CookingTarget());
			check(Texture);

			const int32 Width = PlatformData->SizeX;
			const int32 Height = PlatformData->SizeY;
			const int32 LODGroup = Texture->LODGroup;
			const int32 LODBias = Texture->LODBias;
			const int32 NumCinematicMipLevels = Texture->NumCinematicMipLevels;
			const TextureMipGenSettings MipGenSetting = Texture->MipGenSettings;
			const int32 LastMip = FMath::Max(NumMips - 1, 0);
			check(NumMips >= (int32)PlatformData->GetNumMipsInTail());
			const int32 FirstMipTailMip = NumMips - (int32)PlatformData->GetNumMipsInTail();

			FirstMipToSerialize = Ar.CookingTarget()->GetTextureLODSettings().CalculateLODBias(Width, Height, Texture->MaxTextureSize, LODGroup, LODBias, 0, MipGenSetting, bIsVirtual);
			if (!bIsVirtual)
			{
				FirstMipToSerialize = FMath::Clamp(FirstMipToSerialize, 0, PlatformData->GetNumMipsInTail() > 0 ? FirstMipTailMip : LastMip);
				NumMips = FMath::Max(0, NumMips - FirstMipToSerialize);
			}
			else
			{
				FirstMipToSerialize = FMath::Clamp(FirstMipToSerialize, 0, FMath::Max((int32)PlatformData->VTData->GetNumMips() - 1, 0));
			}
		}
#endif // #if WITH_EDITORONLY_DATA
		Ar << FirstMipToSerialize;
		if (Ar.IsLoading())
		{
			check(Texture);
			FirstMipToSerialize = 0;
		}
	}

	TArray<uint32> BulkDataMipFlags;

	// Force resident mips inline
	if (bCooked && Ar.IsSaving())
	{
		if (bIsVirtual == false)
		{
			BulkDataMipFlags.AddZeroed(PlatformData->Mips.Num());
			for (int32 MipIndex = 0; MipIndex < PlatformData->Mips.Num(); ++MipIndex)
			{
				BulkDataMipFlags[MipIndex] = PlatformData->Mips[MipIndex].BulkData.GetBulkDataFlags();
			}

			int32 MinMipToInline = 0;
			int32 OptionalMips = 0; // TODO: do we need to consider platforms saving texture assets as cooked files? all the info to calculate the optional is part of the editor only data
			bool DuplicateNonOptionalMips = false;
		
#if WITH_EDITORONLY_DATA
			check(Ar.CookingTarget());
			// This also needs to check whether the project enables texture streaming.
			// Currently, there is no reliable way to implement this because there is no difference
			// between the project settings (CVar) and the command line setting (from -NoTextureStreaming)
			if (bStreamable && Ar.CookingTarget()->SupportsFeature(ETargetPlatformFeatures::TextureStreaming))
#else
			if (bStreamable)
#endif
			{
				MinMipToInline = FMath::Max(0, NumMips - PlatformData->GetNumNonStreamingMips());
#if WITH_EDITORONLY_DATA
				const int32 Width = PlatformData->SizeX;
				const int32 Height = PlatformData->SizeY;
				const int32 LODGroup = Texture->LODGroup;
				const int32 LODBias = Texture->LODBias;
				const int32 NumCinematicMipLevels = Texture->NumCinematicMipLevels;

				OptionalMips = Ar.CookingTarget()->GetTextureLODSettings().CalculateNumOptionalMips(LODGroup, Width, Height, NumMips, MinMipToInline, Texture->MipGenSettings);
				DuplicateNonOptionalMips = Ar.CookingTarget()->GetTextureLODSettings().TextureLODGroups[LODGroup].DuplicateNonOptionalMips;
#endif
			}

			for (int32 MipIndex = 0; MipIndex < NumMips && MipIndex < OptionalMips; ++MipIndex) //-V654
			{
				PlatformData->Mips[MipIndex + FirstMipToSerialize].BulkData.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload | BULKDATA_OptionalPayload);
			}

			const uint32 AdditionalNonOptionalBulkDataFlags = DuplicateNonOptionalMips ? BULKDATA_DuplicateNonOptionalPayload : 0;
			for (int32 MipIndex = OptionalMips; MipIndex < NumMips && MipIndex < MinMipToInline; ++MipIndex)
			{
				PlatformData->Mips[MipIndex + FirstMipToSerialize].BulkData.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload | AdditionalNonOptionalBulkDataFlags);
			}
			for (int32 MipIndex = MinMipToInline; MipIndex < NumMips; ++MipIndex)
			{
				PlatformData->Mips[MipIndex + FirstMipToSerialize].BulkData.SetBulkDataFlags(BULKDATA_ForceInlinePayload | BULKDATA_SingleUse);
			}
		}
		else // bVirtual == false
		{
			const int32 NumChunks = PlatformData->VTData->Chunks.Num();
			BulkDataMipFlags.AddZeroed(NumChunks);
			for (int32 ChunkIndex = 0; ChunkIndex < NumChunks; ++ChunkIndex)
			{
				BulkDataMipFlags[ChunkIndex] = PlatformData->VTData->Chunks[ChunkIndex].BulkData.GetBulkDataFlags();
				PlatformData->VTData->Chunks[ChunkIndex].BulkData.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload);
			}	
		}
	}
	Ar << NumMips;
	check(NumMips >= (int32)PlatformData->GetNumMipsInTail());
	if (Ar.IsLoading())
	{
		check(FirstMipToSerialize == 0);
		PlatformData->Mips.Empty(NumMips);
		for (int32 MipIndex = 0; MipIndex < NumMips; ++MipIndex)
		{
			PlatformData->Mips.Add(new FTexture2DMipMap());
		}
	}
	for (int32 MipIndex = 0; MipIndex < NumMips; ++MipIndex)
	{
		PlatformData->Mips[FirstMipToSerialize + MipIndex].Serialize(Ar, Texture, MipIndex);
	}

	Ar << bIsVirtual;
	if (bIsVirtual)
	{
		if (Ar.IsLoading() && PlatformData->VTData == nullptr)
		{
			PlatformData->VTData = new FVirtualTextureBuiltData();
		}
		else
		{
			check(PlatformData->VTData);
		}

		PlatformData->VTData->Serialize(Ar, Texture, FirstMipToSerialize);
	}

	if (bIsVirtual == false)
	{
		for (int32 MipIndex = 0; MipIndex < BulkDataMipFlags.Num(); ++MipIndex)
		{
			check(Ar.IsSaving());
			PlatformData->Mips[MipIndex].BulkData.ResetBulkDataFlags(BulkDataMipFlags[MipIndex]);
		}
	}
	else
	{
		for (int32 ChunkIndex = 0; ChunkIndex < BulkDataMipFlags.Num(); ++ChunkIndex)
		{
			check(Ar.IsSaving() && bCooked);
			PlatformData->VTData->Chunks[ChunkIndex].BulkData.ResetBulkDataFlags(BulkDataMipFlags[ChunkIndex]);
		}
	}
}

void FTexturePlatformData::Serialize(FArchive& Ar, UTexture* Owner)
{
	const bool bCooking = false;
	const bool bStreamable = false;
	SerializePlatformData(Ar, this, Owner, bCooking, bStreamable);
}

void FTexturePlatformData::SerializeCooked(FArchive& Ar, UTexture* Owner, bool bStreamable)
{
	SerializePlatformData(Ar, this, Owner, true, bStreamable);
	if (Ar.IsLoading())
	{
		// Patch up Size as due to mips being stripped out during cooking it could be wrong.
		if (Mips.Num() > 0)
		{
			SizeX = Mips[0].SizeX;
			SizeY = Mips[0].SizeY;
			
			// SizeZ is not the same as NumSlices for texture arrays and cubemaps.
			if (Owner && Owner->IsA(UVolumeTexture::StaticClass()))
			{
				SetNumSlices(Mips[0].SizeZ);
			}
		}
		else if ( VTData )
		{
			SizeX = VTData->Width;
			SizeY = VTData->Height;
		}
	}
}

/*------------------------------------------------------------------------------
	Texture derived data interface.
------------------------------------------------------------------------------*/

void UTexture2D::GetMipData(int32 FirstMipToLoad, void** OutMipData)
{
	if (PlatformData->TryLoadMips(FirstMipToLoad, OutMipData, this) == false)
	{
		// Unable to load mips from the cache. Rebuild the texture and try again.
		UE_LOG(LogTexture,Warning,TEXT("GetMipData failed for %s (%s)"),
			*GetPathName(), GPixelFormats[GetPixelFormat()].Name);
#if WITH_EDITOR
		if (!GetOutermost()->bIsCookedForEditor)
		{
			ForceRebuildPlatformData();
			if (PlatformData->TryLoadMips(FirstMipToLoad, OutMipData, this) == false)
			{
				UE_LOG(LogTexture, Error, TEXT("Failed to build texture %s."), *GetPathName());
			}
		}
#endif // #if WITH_EDITOR
	}
}

void UTextureCube::GetMipData(int32 FirstMipToLoad, void** OutMipData)
{
	if (PlatformData->TryLoadMips(FirstMipToLoad, OutMipData, this) == false)
	{
		// Unable to load mips from the cache. Rebuild the texture and try again.
		UE_LOG(LogTexture,Warning,TEXT("GetMipData failed for %s (%s)"),
			*GetPathName(), GPixelFormats[GetPixelFormat()].Name);
#if WITH_EDITOR
		if (!GetOutermost()->bIsCookedForEditor)
		{
			ForceRebuildPlatformData();
			if (PlatformData->TryLoadMips(FirstMipToLoad, OutMipData, this) == false)
			{
				UE_LOG(LogTexture, Error, TEXT("Failed to build texture %s."), *GetPathName());
			}
		}
#endif // #if WITH_EDITOR
	}
}

void UTexture::UpdateCachedLODBias()
{
	CachedCombinedLODBias = UDeviceProfileManager::Get().GetActiveProfile()->GetTextureLODSettings()->CalculateLODBias(this);
}

#if WITH_EDITOR
void UTexture::CachePlatformData(bool bAsyncCache, bool bAllowAsyncBuild, bool bAllowAsyncLoading, ITextureCompressorModule* Compressor)
{
	FTexturePlatformData** PlatformDataLinkPtr = GetRunningPlatformData();
	if (PlatformDataLinkPtr)
	{
		FTexturePlatformData*& PlatformDataLink = *PlatformDataLinkPtr;
		if (Source.IsValid() && FApp::CanEverRender())
		{
			FString DerivedDataKey;
			TArray<FTextureBuildSettings> BuildSettings;
			GetBuildSettingsForRunningPlatform(*this, BuildSettings);
			check(BuildSettings.Num() == Source.GetNumLayers());
			GetTextureDerivedDataKey(*this, BuildSettings.GetData(), DerivedDataKey);

			if (PlatformDataLink == NULL || PlatformDataLink->DerivedDataKey != DerivedDataKey)
			{
				// Release our resource if there is existing derived data.
				if (PlatformDataLink)
				{
					ReleaseResource();

					// Need to wait for any previous InitRHI() to complete before modifying PlatformData
					// We could remove this flush if InitRHI() was modified to not access PlatformData directly
					FlushRenderingCommands();
				}
				else
				{
					PlatformDataLink = new FTexturePlatformData();
				}
				int32 CacheFlags = 
					(bAsyncCache ? ETextureCacheFlags::Async : ETextureCacheFlags::None) |
					(bAllowAsyncBuild? ETextureCacheFlags::AllowAsyncBuild : ETextureCacheFlags::None) |
					(bAllowAsyncLoading? ETextureCacheFlags::AllowAsyncLoading : ETextureCacheFlags::None);

				PlatformDataLink->Cache(*this, BuildSettings.GetData(), CacheFlags, Compressor);
			}
		}
		else if (PlatformDataLink == NULL)
		{
			// If there is no source art available, create an empty platform data container.
			PlatformDataLink = new FTexturePlatformData();
		}

		
		UpdateCachedLODBias();
	}
}

void UTexture::BeginCachePlatformData()
{
	CachePlatformData(true, true, true);

#if 0 // don't cache in post load, this increases our peak memory usage, instead cache just before we save the package
	// enable caching in postload for derived data cache commandlet and cook by the book
	ITargetPlatformManagerModule* TPM = GetTargetPlatformManager();
	if (TPM && (TPM->RestrictFormatsToRuntimeOnly() == false))
	{
		ITargetPlatformManagerModule* TPM = GetTargetPlatformManager();
		TArray<ITargetPlatform*> Platforms = TPM->GetActiveTargetPlatforms();
		// Cache for all the shader formats that the cooking target requires
		for (int32 FormatIndex = 0; FormatIndex < Platforms.Num(); FormatIndex++)
		{
			BeginCacheForCookedPlatformData(Platforms[FormatIndex]);
		}
	}
#endif
}

void UTexture::BeginCacheForCookedPlatformData( const ITargetPlatform *TargetPlatform )
{
	TMap<FString, FTexturePlatformData*>* CookedPlatformDataPtr = GetCookedPlatformData();
	if (CookedPlatformDataPtr && !GetOutermost()->HasAnyPackageFlags(PKG_FilterEditorOnly))
	{
		TMap<FString,FTexturePlatformData*>& CookedPlatformData = *CookedPlatformDataPtr;
		
		// Make sure the pixel format enum has been cached.
		UTexture::GetPixelFormatEnum();

		// Retrieve formats to cache for targetplatform.
		
		//TArray<FName> PlatformFormats;

		FTextureBuildSettings BuildSettings;
		GetTextureBuildSettings(*this, TargetPlatform->GetTextureLODSettings(), *TargetPlatform, BuildSettings);
		
		TArray< TArray<FTextureBuildSettings> > BuildSettingsToCache;
		GetBuildSettingsPerFormat(*this, BuildSettings, TargetPlatform, BuildSettingsToCache);

		
		/*TargetPlatform->GetTextureFormats(this, 0, PlatformFormats);
		for (int32 FormatIndex = 0; FormatIndex < PlatformFormats.Num(); ++FormatIndex)
		{
			BuildSettings.TextureFormatName = PlatformFormats[FormatIndex];
			BuildSettingsToCache.Add(BuildSettings);
		}*/

		uint32 CacheFlags = ETextureCacheFlags::Async | ETextureCacheFlags::InlineMips;

		// If source data is resident in memory then allow the texture to be built
		// in a background thread.
		bool bAllowAsyncBuild = Source.BulkData.IsBulkDataLoaded();

		if (bAllowAsyncBuild)
		{
			CacheFlags |= ETextureCacheFlags::AllowAsyncBuild;
		}


		// Cull redundant settings by comparing derived data keys.
		for (int32 SettingsIndex = 0; SettingsIndex < BuildSettingsToCache.Num(); ++SettingsIndex)
		{
			check(BuildSettingsToCache[SettingsIndex].Num() == Source.GetNumLayers());

			FString DerivedDataKey;
			GetTextureDerivedDataKey(*this, BuildSettingsToCache[SettingsIndex].GetData(), DerivedDataKey);

			FTexturePlatformData *PlatformData = CookedPlatformData.FindRef( DerivedDataKey );

			if ( PlatformData == NULL )
			{
				// UE_LOG(LogTemp, Warning, TEXT("Caching data for texture %s with id %s"), *GetName(), *DerivedDataKey);
				uint32 CurrentCacheFlags = CacheFlags;
				// if the cached data key exists already then we don't need to allowasync build
				// if it doesn't then allow async builds
				
				{
					if ( GetDerivedDataCacheRef().CachedDataProbablyExists( *DerivedDataKey ) == false )
					{
						CurrentCacheFlags |= ETextureCacheFlags::AllowAsyncBuild;
						CurrentCacheFlags |= ETextureCacheFlags::AllowAsyncLoading;
					}
				}

				FTexturePlatformData* PlatformDataToCache;
				PlatformDataToCache = new FTexturePlatformData();
				PlatformDataToCache->Cache(
					*this,
					BuildSettingsToCache[SettingsIndex].GetData(),
					CurrentCacheFlags,
					nullptr
					);
				CookedPlatformData.Add( DerivedDataKey, PlatformDataToCache );
			}
		}
	}
}

void UTexture::ClearCachedCookedPlatformData( const ITargetPlatform* TargetPlatform )
{
	// I feel like bobby fisher, always four moves ahead of
	// my competition, listen they ain't gonna stop me ever
	// I feel as large as Biggie, swear it could not get better, 
	// I feel in charge like Biggie, wearing that Cosby sweater
	TMap<FString, FTexturePlatformData*> *CookedPlatformDataPtr = GetCookedPlatformData();

	if ( CookedPlatformDataPtr )
	{
		TMap<FString,FTexturePlatformData*>& CookedPlatformData = *CookedPlatformDataPtr;

		// Make sure the pixel format enum has been cached.
		UTexture::GetPixelFormatEnum();

		// Retrieve formats to cache for targetplatform.
		FTextureBuildSettings BuildSettings;
		GetTextureBuildSettings(*this, TargetPlatform->GetTextureLODSettings(), *TargetPlatform, BuildSettings);

		TArray< TArray<FTextureBuildSettings> > BuildSettingsToCache;
		GetBuildSettingsPerFormat(*this, BuildSettings, TargetPlatform, BuildSettingsToCache);


		//TargetPlatform->GetTextureFormats(this, 0, PlatformFormats);
		/*for (int32 FormatIndex = 0; FormatIndex < PlatformFormats.Num(); ++FormatIndex)
		{
			BuildSettings.TextureFormatName = PlatformFormats[FormatIndex];
			// if (BuildSettings.TextureFormatName != NAME_None)
			{
				BuildSettingsToCache.Add(BuildSettings);
			}
		}*/

		// Cull redundant settings by comparing derived data keys.
		for (int32 SettingsIndex = 0; SettingsIndex < BuildSettingsToCache.Num(); ++SettingsIndex)
		{
			check(BuildSettingsToCache[SettingsIndex].Num() == Source.GetNumLayers());

			FString DerivedDataKey;
			GetTextureDerivedDataKey(*this, BuildSettingsToCache[SettingsIndex].GetData(), DerivedDataKey);

			if ( CookedPlatformData.Contains( DerivedDataKey ) )
			{
				FTexturePlatformData *PlatformData = CookedPlatformData.FindAndRemoveChecked( DerivedDataKey );
				delete PlatformData;
			}
		}
	}
}

void UTexture::ClearAllCachedCookedPlatformData()
{
	TMap<FString, FTexturePlatformData*> *CookedPlatformDataPtr = GetCookedPlatformData();

	if ( CookedPlatformDataPtr )
	{
		TMap<FString, FTexturePlatformData*> &CookedPlatformData = *CookedPlatformDataPtr;

		for ( auto It : CookedPlatformData )
		{
			delete It.Value;
		}

		CookedPlatformData.Empty();
	}
}

bool UTexture::IsCachedCookedPlatformDataLoaded( const ITargetPlatform* TargetPlatform ) 
{ 
	const TMap<FString, FTexturePlatformData*> *CookedPlatformDataPtr = GetCookedPlatformData();
	if ( CookedPlatformDataPtr == NULL )
		return true; // we should always have cookedplatformDataPtr in the case of WITH_EDITOR

	FTextureBuildSettings BuildSettings;
	TArray<FTexturePlatformData*> PlatformDataToSerialize;
	GetTextureBuildSettings(*this, TargetPlatform->GetTextureLODSettings(), *TargetPlatform, BuildSettings);

	TArray< TArray<FTextureBuildSettings> > BuildSettingsToCache;
	GetBuildSettingsPerFormat(*this, BuildSettings, TargetPlatform, BuildSettingsToCache);

	for (int32 SettingIndex = 0; SettingIndex < BuildSettingsToCache.Num(); SettingIndex++)
	{
		check(BuildSettingsToCache[SettingIndex].Num() == Source.GetNumLayers());

		FString DerivedDataKey;
		GetTextureDerivedDataKey(*this, BuildSettingsToCache[SettingIndex].GetData(), DerivedDataKey);

		FTexturePlatformData *PlatformData= (*CookedPlatformDataPtr).FindRef(DerivedDataKey);

		// begin cache hasn't been called
		if ( !PlatformData )
			return false;

		if ( (PlatformData->AsyncTask != NULL) && ( PlatformData->AsyncTask->IsWorkDone() == true ) )
		{
			PlatformData->FinishCache();
		}

		if ( PlatformData->AsyncTask)
		{
			return false;
		}
	}
	// if we get here all our stuff is cached :)
	return true;
}

bool UTexture::IsAsyncCacheComplete()
{
	bool bComplete = true;
	FTexturePlatformData** RunningPlatformDataPtr = GetRunningPlatformData();
	if (RunningPlatformDataPtr)
	{
		FTexturePlatformData* RunningPlatformData = *RunningPlatformDataPtr;
		if (RunningPlatformData )
		{
			bComplete &= (RunningPlatformData->AsyncTask == NULL) || RunningPlatformData->AsyncTask->IsWorkDone();
		}
	}

	TMap<FString,FTexturePlatformData*>* CookedPlatformDataPtr = GetCookedPlatformData();
	if (CookedPlatformDataPtr)
	{
		for ( auto It : *CookedPlatformDataPtr )
		{
			FTexturePlatformData* PlatformData = It.Value;
			if (PlatformData)
			{
				bComplete &= (PlatformData->AsyncTask == NULL) || PlatformData->AsyncTask->IsWorkDone();
			}
		}
	}

	return bComplete;
}
	
void UTexture::FinishCachePlatformData()
{
	FTexturePlatformData** RunningPlatformDataPtr = GetRunningPlatformData();
	if (RunningPlatformDataPtr)
	{
		FTexturePlatformData*& RunningPlatformData = *RunningPlatformDataPtr;
		
		if (Source.IsValid() && FApp::CanEverRender())
		{
			if ( RunningPlatformData == NULL )
			{
				// begin cache never called
				CachePlatformData();
			}
			else
			{
				// make sure async requests are finished
				RunningPlatformData->FinishCache();
			}

#if DO_CHECK
			if (!GetOutermost()->HasAnyPackageFlags(PKG_FilterEditorOnly))
			{
				FString DerivedDataKey;
				TArray<FTextureBuildSettings> BuildSettings;
				GetBuildSettingsForRunningPlatform(*this, BuildSettings);
				check(BuildSettings.Num() == Source.GetNumLayers());
				GetTextureDerivedDataKey(*this, BuildSettings.GetData(), DerivedDataKey);

				check(!RunningPlatformData || RunningPlatformData->DerivedDataKey == DerivedDataKey);
			}
#endif
		}
	}

	UpdateCachedLODBias();
}

void UTexture::ForceRebuildPlatformData()
{
	FTexturePlatformData** PlatformDataLinkPtr = GetRunningPlatformData();
	if (PlatformDataLinkPtr && *PlatformDataLinkPtr && FApp::CanEverRender())
	{
		FTexturePlatformData *&PlatformDataLink = *PlatformDataLinkPtr;
		FlushRenderingCommands();
		TArray<FTextureBuildSettings> BuildSettings;
		GetBuildSettingsForRunningPlatform(*this, BuildSettings);
		check(BuildSettings.Num() == Source.GetNumLayers());
		PlatformDataLink->Cache(
			*this,
			BuildSettings.GetData(),
			ETextureCacheFlags::ForceRebuild,
			nullptr
			);
	}
}



void UTexture::MarkPlatformDataTransient()
{
	FDerivedDataCacheInterface& DDC = GetDerivedDataCacheRef();

	FTexturePlatformData** RunningPlatformData = GetRunningPlatformData();
	if (RunningPlatformData)
	{
		FTexturePlatformData* PlatformData = *RunningPlatformData;
		if (PlatformData)
		{
			for (int32 MipIndex = 0; MipIndex < PlatformData->Mips.Num(); ++MipIndex)
			{
				FTexture2DMipMap& Mip = PlatformData->Mips[MipIndex];
				if (Mip.DerivedDataKey.Len() > 0)
				{
					DDC.MarkTransient(*Mip.DerivedDataKey);
				}
			}
			DDC.MarkTransient(*PlatformData->DerivedDataKey);
		}
	}

	TMap<FString, FTexturePlatformData*> *CookedPlatformData = GetCookedPlatformData();
	if ( CookedPlatformData )
	{
		for ( auto It : *CookedPlatformData )
		{
			FTexturePlatformData* PlatformData = It.Value;
			for (int32 MipIndex = 0; MipIndex < PlatformData->Mips.Num(); ++MipIndex)
			{
				FTexture2DMipMap& Mip = PlatformData->Mips[MipIndex];
				if (Mip.DerivedDataKey.Len() > 0)
				{
					DDC.MarkTransient(*Mip.DerivedDataKey);
				}
			}
			DDC.MarkTransient(*PlatformData->DerivedDataKey);
		}
	}
}
#endif // #if WITH_EDITOR

void UTexture::GetVirtualTextureBuildSettings(FVirtualTextureBuildSettings& OutSettings) const
{
	OutSettings.Init();
}

void UTexture::CleanupCachedRunningPlatformData()
{
	FTexturePlatformData **RunningPlatformDataPtr = GetRunningPlatformData();

	if ( RunningPlatformDataPtr )
	{
		FTexturePlatformData *&RunningPlatformData = *RunningPlatformDataPtr;

		if ( RunningPlatformData != NULL )
		{
			delete RunningPlatformData;
			RunningPlatformData = NULL;
		}
	}
}


void UTexture::SerializeCookedPlatformData(FArchive& Ar)
{
	if (IsTemplate() )
	{
		return;
	}

	DECLARE_SCOPE_CYCLE_COUNTER( TEXT("UTexture::SerializeCookedPlatformData"), STAT_Texture_SerializeCookedData, STATGROUP_LoadTime );

	UEnum* PixelFormatEnum = UTexture::GetPixelFormatEnum();


#if WITH_EDITOR
	if (Ar.IsCooking() && Ar.IsPersistent())
	{
		if (!Ar.CookingTarget()->IsServerOnly())
		{
			FTextureBuildSettings BuildSettings;
			GetTextureBuildSettings(*this, Ar.CookingTarget()->GetTextureLODSettings(), *Ar.CookingTarget(), BuildSettings);

			TArray<FTexturePlatformData*> PlatformDataToSerialize;

			if (GetOutermost()->bIsCookedForEditor)
			{
				// For cooked packages, simply grab the current platform data and serialize it
				FTexturePlatformData** RunningPlatformDataPtr = GetRunningPlatformData();
				if (RunningPlatformDataPtr == nullptr)
				{
					return;
				}
				FTexturePlatformData* RunningPlatformData = *RunningPlatformDataPtr;
				if (RunningPlatformData == nullptr)
				{
					return;
				}
				PlatformDataToSerialize.Add(RunningPlatformData);
			}
			else
			{
				TMap<FString, FTexturePlatformData*> *CookedPlatformDataPtr = GetCookedPlatformData();
				if (CookedPlatformDataPtr == NULL)
					return;

				TArray< TArray<FTextureBuildSettings> > BuildSettingsToCache;
				GetBuildSettingsPerFormat(*this, BuildSettings, Ar.CookingTarget(), BuildSettingsToCache);

				for (int32 SettingIndex = 0; SettingIndex < BuildSettingsToCache.Num(); SettingIndex++)
				{
					check(BuildSettingsToCache[SettingIndex].Num() == Source.GetNumLayers());

					FString DerivedDataKey;
					GetTextureDerivedDataKey(*this, BuildSettingsToCache[SettingIndex].GetData(), DerivedDataKey);

					FTexturePlatformData *PlatformDataPtr = (*CookedPlatformDataPtr).FindRef(DerivedDataKey);
					if (PlatformDataPtr == NULL)
					{
						PlatformDataPtr = new FTexturePlatformData();
						PlatformDataPtr->Cache(*this, BuildSettingsToCache[SettingIndex].GetData(), ETextureCacheFlags::InlineMips | ETextureCacheFlags::Async, nullptr);

						CookedPlatformDataPtr->Add(DerivedDataKey, PlatformDataPtr);

					}
					PlatformDataToSerialize.Add(PlatformDataPtr);
				}
			}

			for (int32 i = 0; i < PlatformDataToSerialize.Num(); ++i)
			{
				FTexturePlatformData* PlatformDataToSave = PlatformDataToSerialize[i];
				PlatformDataToSave->FinishCache();
				FName PixelFormatName = PixelFormatEnum->GetNameByValue(PlatformDataToSave->PixelFormat);
				Ar << PixelFormatName;
				int64 SkipOffsetLoc = Ar.Tell();
				int64 SkipOffset = 0;

				{
					FArchive::FScopeSetDebugSerializationFlags S(Ar,DSF_IgnoreDiff);
					Ar << SkipOffset;
				}

				// Pass streamable flag for inlining mips
				PlatformDataToSave->SerializeCooked(Ar, this, BuildSettings.bStreamable);
				SkipOffset = Ar.Tell();
				Ar.Seek(SkipOffsetLoc);
				Ar << SkipOffset;
				Ar.Seek(SkipOffset);
			}
		}
		FName PixelFormatName = NAME_None;
		Ar << PixelFormatName;
	}
	else
#endif // #if WITH_EDITOR
	{

		FTexturePlatformData** RunningPlatformDataPtr = GetRunningPlatformData();
		if ( RunningPlatformDataPtr == NULL )
			return;

		FTexturePlatformData*& RunningPlatformData = *RunningPlatformDataPtr;

		FName PixelFormatName = NAME_None;

		CleanupCachedRunningPlatformData();
		check( RunningPlatformData == NULL );

		RunningPlatformData = new FTexturePlatformData();
		Ar << PixelFormatName;
		while (PixelFormatName != NAME_None)
		{
			EPixelFormat PixelFormat = (EPixelFormat)PixelFormatEnum->GetValueByName(PixelFormatName);
			int64 SkipOffset = 0;
			Ar << SkipOffset;
			bool bFormatSupported = GPixelFormats[PixelFormat].Supported;
			if (RunningPlatformData->PixelFormat == PF_Unknown && bFormatSupported)
			{
				// Extra arg is unused here because we're loading
				const bool bStreamable = false;
				RunningPlatformData->SerializeCooked(Ar, this, bStreamable);
			}
			else
			{
				Ar.Seek(SkipOffset);
			}
			Ar << PixelFormatName;
		}
	}

	if( Ar.IsLoading() )
	{
		LODBias = 0;
	}
}

int32 UTexture::GMinTextureResidentMipCount = NUM_INLINE_DERIVED_MIPS;

void UTexture::SetMinTextureResidentMipCount(int32 InMinTextureResidentMipCount)
{
	int32 MinAllowedMipCount = FPlatformProperties::RequiresCookedData() ? 1 : NUM_INLINE_DERIVED_MIPS;
	GMinTextureResidentMipCount = FMath::Max(InMinTextureResidentMipCount, MinAllowedMipCount);
}