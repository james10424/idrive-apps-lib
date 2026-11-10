// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 Robert Nisipeanu
// Declares the API of RAMSES (https://github.com/bmwcarit/ramses), BMW Car IT GmbH, MPL-2.0.
// Scene and scene objects for libramses 19.0.14.
#ifndef RAMSES19_SCENE_H
#define RAMSES19_SCENE_H

#include "client.h"

namespace ramses
{
    class AnimationSystem;
    class AnimationSystemRealTime;
    class SceneObject : public ClientObject
    {
    public:
        RAMSES19_LIBRARY_OWNED(SceneObject);
    };

    class Node : public SceneObject
    {
    public:
        status_t addChild(Node& node);
        status_t removeChild(Node& node);
        status_t removeAllChildren();
        status_t setParent(Node& node);
        status_t removeParent();
        bool hasChild() const;
        bool hasParent() const;
        uint32_t getChildCount() const;
        Node* getChild(uint32_t index);
        Node* getParent();

        status_t setTranslation(float x, float y, float z);
        status_t getTranslation(float& x, float& y, float& z) const;
        status_t translate(float x, float y, float z);
        // Euler angles in degrees.
        status_t setRotation(float x, float y, float z);
        status_t getRotation(float& x, float& y, float& z) const;
        status_t rotate(float x, float y, float z);
        status_t setScaling(float x, float y, float z);
        status_t getScaling(float& x, float& y, float& z) const;
        status_t scale(float x, float y, float z);

        status_t setVisibility(bool visible);
        bool getVisibility() const;

        status_t getModelMatrix(float (&modelMatrix)[16]) const;
        status_t getInverseModelMatrix(float (&inverseModelMatrix)[16]) const;
        RAMSES19_LIBRARY_OWNED(Node);
    };

    class GroupNode : public Node { public: RAMSES19_LIBRARY_OWNED(GroupNode); };

    class Appearance;
    class GeometryBinding;

    class MeshNode : public Node
    {
    public:
        status_t setAppearance(Appearance& appearance);
        status_t setGeometryBinding(GeometryBinding& geometry);
        status_t removeAppearanceAndGeometry();
        status_t setStartIndex(uint32_t startIndex);
        status_t setIndexCount(uint32_t indexCount);
        status_t setInstanceCount(uint32_t instanceCount);
        uint32_t getStartIndex() const;
        uint32_t getIndexCount() const;
        uint32_t getInstanceCount() const;
        Appearance* getAppearance();
        GeometryBinding* getGeometryBinding();
        RAMSES19_LIBRARY_OWNED(MeshNode);
    };

    class Camera : public Node { public: RAMSES19_LIBRARY_OWNED(Camera); };

    class LocalCamera : public Camera
    {
    public:
        status_t setViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
        status_t setFrustum(float leftPlane, float rightPlane, float bottomPlane, float topPlane,
                            float nearPlane, float farPlane);
        uint32_t getViewportX() const;
        uint32_t getViewportY() const;
        uint32_t getViewportWidth() const;
        uint32_t getViewportHeight() const;
        float getLeftPlane() const;
        float getRightPlane() const;
        float getBottomPlane() const;
        float getTopPlane() const;
        float getNearPlane() const;
        float getFarPlane() const;
        status_t getProjectionMatrix(float (&projectionMatrix)[16]) const;
        RAMSES19_LIBRARY_OWNED(LocalCamera);
    };

    class OrthographicCamera : public LocalCamera { public: RAMSES19_LIBRARY_OWNED(OrthographicCamera); };

    class PerspectiveCamera : public LocalCamera
    {
    public:
        using LocalCamera::setFrustum;
        status_t setFrustum(float fov, float aspectRatio, float nearPlane, float farPlane);
        float getVerticalFieldOfView() const;
        float getAspectRatio() const;
        RAMSES19_LIBRARY_OWNED(PerspectiveCamera);
    };

    class Texture2DBuffer : public SceneObject
    {
    public:
        status_t setData(const char* data, uint32_t mipLevel, uint32_t offsetX, uint32_t offsetY,
                         uint32_t width, uint32_t height);
        uint32_t getMipLevelCount() const;
        status_t getMipLevelSize(uint32_t mipLevel, uint32_t& widthOut, uint32_t& heightOut) const;
        RAMSES19_LIBRARY_OWNED(Texture2DBuffer);
    };

    class TextureSampler : public SceneObject
    {
    public:
        ETextureAddressMode getWrapUMode() const;
        ETextureAddressMode getWrapVMode() const;
        ETextureSamplingMethod getSamplingMethod() const;
        uint32_t getAnisotropyLevel() const;
        RAMSES19_LIBRARY_OWNED(TextureSampler);
    };

    class DataObject : public SceneObject { public: RAMSES19_LIBRARY_OWNED(DataObject); };

    class DataFloat : public DataObject
    {
    public:
        status_t setValue(float value);
        status_t getValue(float& value) const;
        RAMSES19_LIBRARY_OWNED(DataFloat);
    };

    class DataInt32 : public DataObject
    {
    public:
        status_t setValue(int32_t value);
        status_t getValue(int32_t& value) const;
        RAMSES19_LIBRARY_OWNED(DataInt32);
    };

    class DataVector3f : public DataObject
    {
    public:
        status_t setValue(float x, float y, float z);
        status_t getValue(float& x, float& y, float& z) const;
        RAMSES19_LIBRARY_OWNED(DataVector3f);
    };

    class DataVector2f : public DataObject
    {
    public:
        status_t setValue(float x, float y);
        status_t getValue(float& x, float& y) const;
        RAMSES19_LIBRARY_OWNED(DataVector2f);
    };

    class DataVector4f : public DataObject
    {
    public:
        status_t setValue(float x, float y, float z, float w);
        status_t getValue(float& x, float& y, float& z, float& w) const;
        RAMSES19_LIBRARY_OWNED(DataVector4f);
    };

    class DataMatrix44f : public DataObject
    {
    public:
        status_t setValue(const float (&matrix)[16]);
        status_t getValue(float (&matrix)[16]) const;
        RAMSES19_LIBRARY_OWNED(DataMatrix44f);
    };

    class Appearance : public SceneObject
    {
    public:
        status_t setBlendingFactors(EBlendFactor srcColor, EBlendFactor destColor, EBlendFactor srcAlpha, EBlendFactor destAlpha);
        status_t getBlendingFactors(EBlendFactor& srcColor, EBlendFactor& destColor, EBlendFactor& srcAlpha, EBlendFactor& destAlpha) const;
        status_t setBlendingOperations(EBlendOperation operationColor, EBlendOperation operationAlpha);
        status_t setDepthFunction(EDepthFunc func);
        status_t getDepthFunction(EDepthFunc& depthFunc) const;
        status_t setDepthWrite(bool enable);
        status_t getDepthWriteMode(EDepthWrite& mode) const;
        status_t setCullingMode(ECullMode mode);
        status_t getCullingMode(ECullMode& mode) const;
        status_t setDrawMode(EDrawMode mode);
        status_t setColorWriteMask(bool writeRed, bool writeGreen, bool writeBlue, bool writeAlpha);
        status_t setStencilFunction(EStencilFunc func, uint32_t ref, uint8_t mask);
        status_t setStencilOperation(EStencilOperation sfail, EStencilOperation dpfail, EStencilOperation dppass);

        const Effect& getEffect() const;

        status_t setInputValueFloat(const UniformInput& input, float value);
        status_t setInputValueFloat(const UniformInput& input, uint32_t elementCount, const float* values);
        status_t setInputValueInt32(const UniformInput& input, int32_t value);
        status_t setInputValueVector2f(const UniformInput& input, float x, float y);
        status_t setInputValueVector2f(const UniformInput& input, uint32_t elementCount, const float* values);
        status_t setInputValueVector3f(const UniformInput& input, float x, float y, float z);
        status_t setInputValueVector3f(const UniformInput& input, uint32_t elementCount, const float* values);
        status_t setInputValueVector4f(const UniformInput& input, float x, float y, float z, float w);
        status_t setInputValueVector4f(const UniformInput& input, uint32_t elementCount, const float* values);
        status_t setInputValueMatrix44f(const UniformInput& input, const float* matrix44fValue);
        status_t setInputValueMatrix44f(const UniformInput& input, uint32_t elementCount, const float* values);
        status_t setInputValueInt32(const UniformInput& input, uint32_t elementCount, const int32_t* values);
        status_t setInputValueVector2i(const UniformInput& input, uint32_t elementCount, const int32_t* values);
        status_t setInputValueVector3i(const UniformInput& input, uint32_t elementCount, const int32_t* values);
        status_t setInputValueVector4i(const UniformInput& input, uint32_t elementCount, const int32_t* values);
        status_t setInputValueMatrix22f(const UniformInput& input, uint32_t elementCount, const float* values);
        status_t setInputValueMatrix33f(const UniformInput& input, uint32_t elementCount, const float* values);
        status_t getInputValueFloat(const UniformInput& input, float& value) const;
        status_t getInputValueVector4f(const UniformInput& input, float& x, float& y, float& z, float& w) const;
        status_t setInputTexture(const UniformInput& input, const TextureSampler& textureSampler);
        status_t bindInput(const UniformInput& input, const DataObject& dataObject);
        status_t unbindInput(const UniformInput& input);
        bool isInputBound(const UniformInput& input) const;
        RAMSES19_LIBRARY_OWNED(Appearance);
    };

    class GeometryBinding : public SceneObject
    {
    public:
        status_t setIndices(const UInt16Array& indicesResource);
        status_t setIndices(const UInt32Array& indicesResource);
        status_t setInputBuffer(const AttributeInput& attributeInput, const FloatArray& resource, uint32_t instancingDivisor = 0);
        status_t setInputBuffer(const AttributeInput& attributeInput, const Vector2fArray& resource, uint32_t instancingDivisor = 0);
        status_t setInputBuffer(const AttributeInput& attributeInput, const Vector3fArray& resource, uint32_t instancingDivisor = 0);
        status_t setInputBuffer(const AttributeInput& attributeInput, const Vector4fArray& resource, uint32_t instancingDivisor = 0);
        RAMSES19_LIBRARY_OWNED(GeometryBinding);
    };

    class RenderGroup : public SceneObject
    {
    public:
        status_t addMeshNode(const MeshNode& mesh, int32_t orderWithinGroup = 0);
        status_t removeMeshNode(const MeshNode& mesh);
        bool containsMeshNode(const MeshNode& mesh) const;
        status_t addRenderGroup(const RenderGroup& renderGroup, int32_t orderWithinGroup = 0);
        status_t removeAllRenderables();
        RAMSES19_LIBRARY_OWNED(RenderGroup);
    };

    class RenderPass : public SceneObject
    {
    public:
        status_t setCamera(const Camera& camera);
        const Camera* getCamera() const;
        status_t addRenderGroup(const RenderGroup& renderGroup, int32_t orderWithinPass = 0);
        status_t removeRenderGroup(const RenderGroup& renderGroup);
        status_t removeAllRenderGroups();
        bool containsRenderGroup(const RenderGroup& renderGroup) const;
        status_t setClearFlags(uint32_t clearFlags);
        uint32_t getClearFlags() const;
        status_t setClearColor(float r, float g, float b, float a);
        void getClearColor(float& r, float& g, float& b, float& a) const;
        status_t setRenderOrder(int32_t renderOrder);
        int32_t getRenderOrder() const;
        status_t setEnabled(bool enable);
        bool isEnabled() const;
        status_t setRenderOnce(bool enable);
        bool isRenderOnce() const;
        status_t retriggerRenderOnce();
        RAMSES19_LIBRARY_OWNED(RenderPass);
    };

    class Scene : public ClientObject
    {
    public:
        GroupNode* createGroupNode(const char* name = nullptr);
        MeshNode* createMeshNode(const char* name = nullptr);
        PerspectiveCamera* createPerspectiveCamera(const char* name = nullptr);
        OrthographicCamera* createOrthographicCamera(const char* name = nullptr);
        RenderPass* createRenderPass(const char* name = nullptr);
        RenderGroup* createRenderGroup(const char* name = nullptr);
        Appearance* createAppearance(const Effect& effect, const char* name = nullptr);
        GeometryBinding* createGeometryBinding(const Effect& effect, const char* name = nullptr);

        TextureSampler* createTextureSampler(ETextureAddressMode wrapUMode, ETextureAddressMode wrapVMode,
                                             ETextureSamplingMethod samplingMethod, const Texture2D& texture,
                                             uint32_t anisotropyLevel = 1, const char* name = nullptr);
        TextureSampler* createTextureSampler(ETextureAddressMode wrapUMode, ETextureAddressMode wrapVMode,
                                             ETextureSamplingMethod samplingMethod, const Texture2DBuffer& textureBuffer,
                                             uint32_t anisotropyLevel = 1, const char* name = nullptr);
        Texture2DBuffer* createTexture2DBuffer(uint32_t mipLevelCount, uint32_t width, uint32_t height,
                                               ETextureFormat textureFormat, const char* name = nullptr);

        DataFloat* createDataFloat(const char* name = nullptr);
        DataInt32* createDataInt32(const char* name = nullptr);
        DataVector2f* createDataVector2f(const char* name = nullptr);
        DataVector3f* createDataVector3f(const char* name = nullptr);
        DataVector4f* createDataVector4f(const char* name = nullptr);
        DataMatrix44f* createDataMatrix44f(const char* name = nullptr);
        RamsesObject* findObjectByName(const char* name);

        AnimationSystem* createAnimationSystem(uint32_t flags = EAnimationSystemFlags_Default, const char* name = nullptr);
        AnimationSystemRealTime* createRealTimeAnimationSystem(uint32_t flags = EAnimationSystemFlags_Default, const char* name = nullptr);

        status_t destroy(SceneObject& object);
        status_t flush(ESceneFlushMode flushMode = ESceneFlushMode_Synchronous,
                       sceneVersionTag_t sceneVersion = InvalidSceneVersionTag);
        status_t publish(EScenePublicationMode publicationMode = EScenePublicationMode_LocalAndRemote);
        status_t unpublish();
        bool isPublished() const;
        sceneId_t getSceneId() const;
        status_t setExpirationTimestamp(uint64_t ptpExpirationTimestampInMilliseconds);
        RAMSES19_LIBRARY_OWNED(Scene);
    };

    // The scene's objects of a type (or of a type derived from it).
    class SceneObjectIterator
    {
    public:
        SceneObjectIterator(const Scene& scene, ERamsesObjectType objectType);
        ~SceneObjectIterator();
        RamsesObject* getNext();
        RAMSES19_VALUE_STORAGE(SceneObjectIterator);
    };
}

#endif
