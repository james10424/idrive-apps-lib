// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 Robert Nisipeanu
// Declares the API of RAMSES (https://github.com/bmwcarit/ramses), BMW Car IT GmbH, MPL-2.0.
// RamsesClient, resources and effects for libramses 19.0.14.
#ifndef RAMSES19_CLIENT_H
#define RAMSES19_CLIENT_H

#include "framework.h"

namespace ramses
{
    class Scene;

    class ClientObject : public RamsesObject
    {
    public:
        RAMSES19_LIBRARY_OWNED(ClientObject);
    };

    class Resource : public ClientObject
    {
    public:
        RAMSES19_LIBRARY_OWNED(Resource);
    };

    class FloatArray : public Resource { public: RAMSES19_LIBRARY_OWNED(FloatArray); };
    class Vector2fArray : public Resource { public: RAMSES19_LIBRARY_OWNED(Vector2fArray); };
    class Vector3fArray : public Resource { public: RAMSES19_LIBRARY_OWNED(Vector3fArray); };
    class Vector4fArray : public Resource { public: RAMSES19_LIBRARY_OWNED(Vector4fArray); };
    class UInt16Array : public Resource { public: RAMSES19_LIBRARY_OWNED(UInt16Array); };
    class UInt32Array : public Resource { public: RAMSES19_LIBRARY_OWNED(UInt32Array); };

    class Texture2D : public Resource
    {
    public:
        uint32_t getWidth() const;
        uint32_t getHeight() const;
        ETextureFormat getTextureFormat() const;
        RAMSES19_LIBRARY_OWNED(Texture2D);
    };

    class EffectInput : public StatusObject
    {
    public:
        const char* getName() const;
        bool isValid() const;
    protected:
        EffectInput() = delete;
        ~EffectInput();
    };

    // UniformInput, AttributeInput and EffectDescription export no destructor:
    // in 19 it is inline and runs the base destructor, as here.
    class UniformInput : public EffectInput
    {
    public:
        UniformInput();
        ~UniformInput() {}
        uint32_t getElementCount() const;
        EEffectInputDataType getDataType() const;
        RAMSES19_VALUE_STORAGE(UniformInput);
    };

    class AttributeInput : public EffectInput
    {
    public:
        AttributeInput();
        ~AttributeInput() {}
        RAMSES19_VALUE_STORAGE(AttributeInput);
    };

    class Effect : public Resource
    {
    public:
        uint32_t getUniformInputCount() const;
        uint32_t getAttributeInputCount() const;
        status_t getUniformInput(uint32_t index, UniformInput& uniformInput) const;
        status_t getAttributeInput(uint32_t index, AttributeInput& attributeInput) const;
        status_t findUniformInput(const char* inputName, UniformInput& uniformInput) const;
        status_t findAttributeInput(const char* inputName, AttributeInput& attributeInput) const;
        RAMSES19_LIBRARY_OWNED(Effect);
    };

    class EffectDescription : public StatusObject
    {
    public:
        EffectDescription();
        ~EffectDescription() {}
        status_t setVertexShader(const char* shaderSource);
        status_t setFragmentShader(const char* shaderSource);
        status_t setVertexShaderFromFile(const char* shaderSourceFileName);
        status_t setFragmentShaderFromFile(const char* shaderSourceFileName);
        status_t addCompilerDefine(const char* define);
        status_t setUniformSemantic(const char* inputName, EEffectUniformSemantic semanticType);
        status_t setAttributeSemantic(const char* inputName, EEffectAttributeSemantic semanticType);
        const char* getVertexShader() const;
        const char* getFragmentShader() const;
        RAMSES19_VALUE_STORAGE(EffectDescription);
    };

    class SceneConfig : public StatusObject
    {
    public:
        SceneConfig();
        ~SceneConfig();
        status_t setPublicationMode(EScenePublicationMode publicationMode);
        RAMSES19_VALUE_STORAGE(SceneConfig);
    };

    // A resource file to load from or save to, and a set of them.
    class ResourceFileDescription
    {
    public:
        explicit ResourceFileDescription(const char* filename);
        ~ResourceFileDescription();
        void add(const Resource* resource);
        RAMSES19_VALUE_STORAGE(ResourceFileDescription);
    };

    class ResourceFileDescriptionSet
    {
    public:
        ResourceFileDescriptionSet();
        ~ResourceFileDescriptionSet();
        void add(const ResourceFileDescription& description);
        RAMSES19_VALUE_STORAGE(ResourceFileDescriptionSet);
    };

    class RamsesClient : public RamsesObject
    {
    public:
        RamsesClient(const char* applicationName, RamsesFramework& framework);
        ~RamsesClient();

        Scene* createScene(sceneId_t sceneId, const SceneConfig& sceneConfig, const char* name = nullptr);
        status_t destroy(Scene& scene);
        status_t destroy(const Resource& resource);
        // A scene file keeps its scene id: the loaded scene has the id it was saved with.
        Scene* loadSceneFromFile(const char* fileName, const ResourceFileDescriptionSet& resourceFileInformation);
        status_t saveSceneToFile(const Scene& scene, const char* fileName,
                                 const ResourceFileDescriptionSet& resourceFileInformation, bool compress) const;
        RamsesObject* findObjectByName(const char* name);

        Effect* createEffect(const EffectDescription& effectDesc,
                             resourceCacheFlag_t cacheFlag = ResourceCacheFlag_DoNotCache, const char* name = nullptr);
        const FloatArray* createConstFloatArray(uint32_t numberOfFloats, const float* arrayData,
                                                resourceCacheFlag_t cacheFlag = ResourceCacheFlag_DoNotCache, const char* name = nullptr);
        const Vector2fArray* createConstVector2fArray(uint32_t numberOfVectors, const float* arrayData,
                                                      resourceCacheFlag_t cacheFlag = ResourceCacheFlag_DoNotCache, const char* name = nullptr);
        const Vector3fArray* createConstVector3fArray(uint32_t numberOfVectors, const float* arrayData,
                                                      resourceCacheFlag_t cacheFlag = ResourceCacheFlag_DoNotCache, const char* name = nullptr);
        const Vector4fArray* createConstVector4fArray(uint32_t numberOfVectors, const float* arrayData,
                                                      resourceCacheFlag_t cacheFlag = ResourceCacheFlag_DoNotCache, const char* name = nullptr);
        const UInt16Array* createConstUInt16Array(uint32_t numberOfIndices, const uint16_t* arrayData,
                                                  resourceCacheFlag_t cacheFlag = ResourceCacheFlag_DoNotCache, const char* name = nullptr);
        const UInt32Array* createConstUInt32Array(uint32_t numberOfIndices, const uint32_t* arrayData,
                                                  resourceCacheFlag_t cacheFlag = ResourceCacheFlag_DoNotCache, const char* name = nullptr);
        Texture2D* createTexture2D(uint32_t width, uint32_t height, ETextureFormat format, uint32_t mipMapCount,
                                   const MipLevelData mipLevelData[], bool generateMipChain = false,
                                   resourceCacheFlag_t cacheFlag = ResourceCacheFlag_DoNotCache, const char* name = nullptr);

        const char* getStatusMessage(status_t status) const;
        RAMSES19_VALUE_STORAGE(RamsesClient);
    };

    // The client's resources of a type (or of a type derived from it).
    class ResourceIterator
    {
    public:
        ResourceIterator(const RamsesClient& client, ERamsesObjectType objectType);
        ~ResourceIterator();
        RamsesObject* getNext();
        RAMSES19_VALUE_STORAGE(ResourceIterator);
    };
}

#endif
