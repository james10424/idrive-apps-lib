// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 Robert Nisipeanu
// Declares the API of RAMSES (https://github.com/bmwcarit/ramses), BMW Car IT GmbH, MPL-2.0.
// Basic types and enums of the head unit's libramses-shared-lib-wayland-ivi-egl-es-3-0.so.19.0.
// Evidence and VERIFIED / INFERRED label for each value: ../../ABI.md.
#ifndef RAMSES19_TYPES_H
#define RAMSES19_TYPES_H

#include <stdint.h>

namespace ramses
{
    typedef uint32_t status_t;
    static const status_t StatusOK = 0u;

    typedef uint64_t sceneId_t;
    typedef uint64_t sceneVersionTag_t;
    static const sceneVersionTag_t InvalidSceneVersionTag = ~sceneVersionTag_t(0);
    typedef uint32_t displayId_t;
    typedef uint32_t splineTimeStamp_t;
    typedef uint32_t sequenceTimeStamp_t;
    typedef int64_t timeMilliseconds_t;
    typedef uint64_t globalTimeStamp_t;

    // The library's own template has two parameters and a user-provided copy
    // constructor, so it is passed by hidden reference, not in a register.
    template <typename BaseType, typename UniqueId>
    class StronglyTypedValue
    {
    public:
        explicit StronglyTypedValue(BaseType value) : m_value(value) {}
        StronglyTypedValue(const StronglyTypedValue& other) : m_value(other.m_value) {}
        BaseType getValue() const { return m_value; }
    private:
        BaseType m_value;
    };

    struct resourceCacheFlagTag {};
    typedef StronglyTypedValue<uint32_t, resourceCacheFlagTag> resourceCacheFlag_t;
    static const resourceCacheFlag_t ResourceCacheFlag_DoNotCache(0u);

    struct MipLevelData
    {
        MipLevelData(uint32_t size, const uint8_t* data) : m_size(size), m_data(data) {}
        uint32_t m_size;
        const uint8_t* m_data;
    };

    // Object types and uniform data types. Their values are not reconstructed:
    // a client learns them at run time from objects it makes (getType(),
    // UniformInput::getDataType()). 32 bits wide, as the library's, so any
    // value the library returns is a value of the type.
    enum ERamsesObjectType : uint32_t { ERamsesObjectType_Invalid = 0 };
    enum EEffectInputDataType : uint32_t { EEffectInputDataType_Invalid = 0 };

    enum EScenePublicationMode
    {
        EScenePublicationMode_LocalAndRemote = 0,
        EScenePublicationMode_LocalOnly = 1
    };

    // Only ESceneFlushMode_Synchronous is accepted; any other value fails with "Unknown flushMode".
    enum ESceneFlushMode
    {
        ESceneFlushMode_Asynchronous = 0,
        ESceneFlushMode_Synchronous = 1
    };

    enum EClearFlags
    {
        EClearFlags_None = 0,
        EClearFlags_Color = 1 << 0,
        EClearFlags_Depth = 1 << 1,
        EClearFlags_Stencil = 1 << 2,
        EClearFlags_All = EClearFlags_Color | EClearFlags_Depth | EClearFlags_Stencil
    };

    enum ETextureFormat
    {
        ETextureFormat_Invalid = 0,
        ETextureFormat_R8 = 1,
        ETextureFormat_RG8 = 2,
        ETextureFormat_RGB8 = 3,
        ETextureFormat_RGB565 = 4,
        ETextureFormat_RGBA8 = 5,
        ETextureFormat_RGBA4 = 6,
        ETextureFormat_RGBA5551 = 7,
        ETextureFormat_BGR8 = 8,
        ETextureFormat_BGRA8 = 9,
        ETextureFormat_ETC2RGB = 10,
        ETextureFormat_ETC2RGBA = 11,
        ETextureFormat_R16F = 12,
        ETextureFormat_R32F = 13,
        ETextureFormat_RG16F = 14,
        ETextureFormat_RG32F = 15,
        ETextureFormat_RGB16F = 16,
        ETextureFormat_RGB32F = 17,
        ETextureFormat_RGBA16F = 18,
        ETextureFormat_RGBA32F = 19,
        ETextureFormat_SRGB8 = 20,
        ETextureFormat_SRGB8_ALPHA8 = 21
        // 22..49: ASTC RGBA / SRGBA block formats (see ABI.md).
    };

    enum ETextureAddressMode
    {
        ETextureAddressMode_Clamp = 0,
        ETextureAddressMode_Repeat = 1,
        ETextureAddressMode_Mirror = 2
    };

    enum ETextureSamplingMethod
    {
        ETextureSamplingMethod_Nearest = 0,
        ETextureSamplingMethod_NearestWithMipmaps = 1,
        ETextureSamplingMethod_Bilinear = 2,
        ETextureSamplingMethod_BilinearWithMipMaps = 3,
        ETextureSamplingMethod_Trilinear = 4,
        ETextureSamplingMethod_MinLinearMagNearest = 5,
        ETextureSamplingMethod_MinNearestMagLinear = 6
    };

    enum EBlendFactor
    {
        EBlendFactor_Zero = 0,
        EBlendFactor_One = 1,
        EBlendFactor_SrcAlpha = 2,
        EBlendFactor_OneMinusSrcAlpha = 3,
        EBlendFactor_DstAlpha = 4,
        EBlendFactor_OneMinusDstAlpha = 5
    };

    enum EBlendOperation
    {
        EBlendOperation_Disabled = 0,
        EBlendOperation_Add = 1,
        EBlendOperation_Subtract = 2,
        EBlendOperation_ReverseSubtract = 3,
        EBlendOperation_Min = 4,
        EBlendOperation_Max = 5
    };

    enum ECullMode
    {
        ECullMode_Disabled = 0,
        ECullMode_FrontFacing = 1,
        ECullMode_BackFacing = 2,
        ECullMode_FrontAndBackFacing = 3
    };

    enum EDepthFunc
    {
        EDepthFunc_Disabled = 0,
        EDepthFunc_Greater = 1,
        EDepthFunc_GreaterEqual = 2,
        EDepthFunc_Less = 3,
        EDepthFunc_LessEqual = 4,
        EDepthFunc_Equal = 5,
        EDepthFunc_NotEqual = 6,
        EDepthFunc_Always = 7,
        EDepthFunc_Never = 8
    };

    enum EDepthWrite
    {
        EDepthWrite_Disabled = 0,
        EDepthWrite_Enabled = 1
    };

    enum EDrawMode
    {
        EDrawMode_Points = 0,
        EDrawMode_Lines = 1,
        EDrawMode_LineLoop = 2,
        EDrawMode_Triangles = 3,
        EDrawMode_TriangleStrip = 4
    };

    enum EStencilFunc
    {
        EStencilFunc_Disabled = 0,
        EStencilFunc_Never = 1,
        EStencilFunc_Always = 2,
        EStencilFunc_Equal = 3,
        EStencilFunc_NotEqual = 4,
        EStencilFunc_Less = 5,
        EStencilFunc_LessEqual = 6,
        EStencilFunc_Greater = 7,
        EStencilFunc_GreaterEqual = 8
    };

    enum EStencilOperation
    {
        EStencilOperation_Keep = 0,
        EStencilOperation_Zero = 1,
        EStencilOperation_Replace = 2,
        EStencilOperation_Increment = 3,
        EStencilOperation_IncrementWrap = 4,
        EStencilOperation_Decrement = 5,
        EStencilOperation_DecrementWrap = 6,
        EStencilOperation_Invert = 7
    };

    enum EEffectUniformSemantic
    {
        EEffectUniformSemantic_Invalid = 0,
        EEffectUniformSemantic_ProjectionMatrix = 1,
        EEffectUniformSemantic_ModelMatrix = 2,
        EEffectUniformSemantic_RendererViewMatrix = 3,
        EEffectUniformSemantic_CameraViewMatrix = 4,
        EEffectUniformSemantic_CameraWorldPosition = 5,
        EEffectUniformSemantic_ViewMatrix = 6,
        EEffectUniformSemantic_ModelViewMatrix = 7,
        EEffectUniformSemantic_ModelViewMatrix33 = 8,
        EEffectUniformSemantic_ModelViewProjectionMatrix = 9,
        EEffectUniformSemantic_NormalMatrix = 10,
        EEffectUniformSemantic_RendererScreenResolution = 11,
        EEffectUniformSemantic_TextTexture = 12
    };

    enum EEffectAttributeSemantic
    {
        EEffectAttributeSemantic_Invalid = 0,
        EEffectAttributeSemantic_TextPositions = 1,
        EEffectAttributeSemantic_TextTextureCoordinates = 2
    };

    enum EAnimationSystemFlags
    {
        EAnimationSystemFlags_Default = 0,
        EAnimationSystemFlags_ClientSideProcessing = 1
    };

    enum EAnimatedProperty
    {
        EAnimatedProperty_Translation = 0,
        EAnimatedProperty_Rotation = 1,
        EAnimatedProperty_Scaling = 2
    };

    enum EAnimatedPropertyComponent
    {
        EAnimatedPropertyComponent_X = 0,
        EAnimatedPropertyComponent_Y = 1,
        EAnimatedPropertyComponent_Z = 2,
        EAnimatedPropertyComponent_W = 3,
        EAnimatedPropertyComponent_All = 4
    };

    enum ELoopMode
    {
        ELoopMode_UpdateAndRender = 0,
        ELoopMode_UpdateOnly = 1
    };

    // Values accepted by RamsesFrameworkConfig::enableSomeIPCommunication (ERamsesCommunicationUser).
    // The full table, with each participant's SOME/IP instances, is in ABI.md.
    enum ERamsesCommunicationUser
    {
        ERamsesCommunicationUser_IC_HMI = 1,
        ERamsesCommunicationUser_HU_HMI = 2,
        ERamsesCommunicationUser_NAVI_HG = 3,
        ERamsesCommunicationUser_NAVI_JV = 4,
        ERamsesCommunicationUser_HU_SUA = 5,
        ERamsesCommunicationUser_HU_HMI_Test = 902,
        ERamsesCommunicationUser_MGUTestUserConnectingToAll = 801
    };
}

#endif
